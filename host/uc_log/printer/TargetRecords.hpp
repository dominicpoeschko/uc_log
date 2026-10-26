#pragma once

#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/Le32.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/// What the firmware keeps in RAM for a debugger, decoded from raw target bytes: the crash records (fault, stack
/// snapshot, panic), the health and boot-guard records (all CRC-checked, Persistent.hpp), the sanitizer counter, a
/// painted stack and the trace rings. The layouts are the SDK's (Kvasir_SDK src/kvasir/Runtime: FaultRecord.hpp,
/// Panic.hpp, BootGuardRules.hpp, ubsan.hpp, StackUsage.hpp; src/kvasir/Health/HealthRecords.hpp; src/kvasir/Util:
/// Persistent.hpp, Trace.hpp): the one host copy of them - a struct here is a record's field order, and the SDK's
/// tests/records_cross_tests.cpp holds every id, size, offset and name table to the SDK's headers.
namespace uc_log::printer::records {

/// The word at `at`; the bytes past the end of `raw` read as 0
constexpr std::uint32_t word(std::span<std::byte const> raw,
                             std::size_t                at) {
    if(at >= raw.size()) { return 0; }
    return static_cast<std::uint32_t>(
      leValue(raw.subspan(at, std::min<std::size_t>(4, raw.size() - at))));
}

constexpr std::uint16_t half(std::span<std::byte const> raw,
                             std::size_t                at) {
    return static_cast<std::uint16_t>(word(raw, at) & 0xFFFFU);
}

// ---- Kvasir::Persistent (Util/Persistent.hpp) --------------------------------------------------
// word 0 the id (the record kind's validity magic), the value's words, then zlib's crc32 over everything before it.

/// The value bytes (`valueBytes`, whole words) of the Persistent record `raw` begins with; nullopt for another id, a
/// CRC that does not match (never stored, cleared, torn, corrupt) or too few bytes.
constexpr std::optional<std::span<std::byte const>> persistentValue(std::span<std::byte const> raw,
                                                                    std::uint32_t              id,
                                                                    std::size_t valueBytes) {
    auto const bytes = 4 + valueBytes + 4;
    if(valueBytes % 4 != 0 || raw.size() < bytes || word(raw, 0) != id
       || word(raw, bytes - 4) != crc32(raw.first(bytes - 4)))
    {
        return std::nullopt;
    }
    return raw.subspan(4, valueBytes);
}

/// A Persistent<T, Tag>'s size on the target: id, T, crc.
template<typename T>
inline constexpr std::uint32_t recordSize = 4 + sizeof(T) + 4;

/// The record T a Persistent holds, its bytes taken as they are (the struct is the field order): nullopt as
/// persistentValue. The target and every host this builds for are little endian.
template<typename T>
constexpr std::optional<T> decodeRecord(std::span<std::byte const> raw,
                                        std::uint32_t              id) {
    static_assert(
      std::endian::native == std::endian::little,
      "a record's bytes are taken as they are: the host must be little endian like the target");
    static_assert(std::is_trivially_copyable_v<T>, "a record is plain bytes");
    static_assert(std::has_unique_object_representations_v<T>,
                  "no padding in a record struct: name the target's reserved bytes");
    static_assert(sizeof(T) % 4 == 0, "a Persistent value is whole words");
    auto const value = persistentValue(raw, id, sizeof(T));
    if(!value) { return std::nullopt; }
    std::array<std::byte, sizeof(T)> bytes{};
    std::ranges::copy(*value, bytes.begin());
    return std::bit_cast<T>(bytes);
}

/// Kvasir::persistentId("KFLT"): the four characters as a little-endian word.
consteval std::uint32_t persistentId(char const (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0]))
         | static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8U
         | static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16U
         | static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24U;
}

// ---- Kvasir::Fault::lastFault (Runtime/FaultRecord.hpp: Persistent<FullRecord, RecordTag>) -------
inline constexpr std::string_view faultSymbol{"Kvasir::Fault::lastFault"};
inline constexpr std::uint32_t    faultId    = persistentId("KFLT");
inline constexpr std::size_t      faultWords = 33;   // sizeof(Fault::FullRecord) / 4
inline constexpr std::uint32_t    faultSize  = 4 * (1 + faultWords + 1);

struct Fault {
    enum Flag : std::uint32_t {   // FullRecord::Flag
        frameValid  = 1U << 0,
        calleeSaved = 1U << 1,
        faultRegs   = 1U << 2,
        fpFrame     = 1U << 3,
        stackLimits = 1U << 4,
        secureRegs  = 1U << 5,
        stackCopied = 1U << 6,
    };

    std::uint32_t count{};   // faults since a boot line last reported one; registers: the first
    std::uint32_t flags{};
    std::array<std::uint32_t, 4> r{};   // r0-r3, stacked
    std::uint32_t                r12{};
    std::uint32_t                lr{};
    std::uint32_t                pc{};
    std::uint32_t                xpsr{};
    std::array<std::uint32_t, 8> high{};   // r4-r11 at the handler's entry
    std::uint32_t                excReturn{};
    std::uint32_t                msp{};
    std::uint32_t                psp{};
    std::uint32_t                control{};
    std::uint32_t                sp{};   // the faulting context's SP before the frame; 0: unknown
    std::uint32_t                cfsr{};
    std::uint32_t                hfsr{};
    std::uint32_t                mmfar{};
    std::uint32_t                bfar{};
    std::uint32_t                shcsr{};
    std::uint32_t                icsr{};
    std::uint32_t                sfsr{};
    std::uint32_t                sfar{};
    std::uint32_t                msplim{};
    std::uint32_t                psplim{};

    static constexpr unsigned coreShift = 8;   // FullRecord::coreShift: bits 11:8 the core
    static constexpr unsigned archShift
      = 12;   // FullRecord::archShift: bits 15:12 the architecture (6, 7, 8)

    /// The exception the fault was taken in (xPSR.IPSR of the faulting context).
    [[nodiscard]] constexpr std::uint32_t exception() const { return xpsr & 0x1FFU; }

    [[nodiscard]] constexpr std::uint32_t core() const { return (flags >> coreShift) & 0xFU; }

    /// The architecture's major version: 6, 7 or 8 (Armv<N>-M).
    [[nodiscard]] constexpr std::uint32_t arch() const { return (flags >> archShift) & 0xFU; }

    /// Whether r0-r3, r12, lr, pc, xpsr are the stacked frame (else 0: it was not pushed, or lay outside RAM).
    [[nodiscard]] constexpr bool hasFrame() const { return (flags & frameValid) != 0; }

    /// The 33 words in record order (faultFieldNames names them).
    [[nodiscard]] constexpr std::array<std::uint32_t,
                                       faultWords>
    words() const {
        return std::bit_cast<std::array<std::uint32_t, faultWords>>(*this);
    }
};

static_assert(sizeof(Fault) == faultWords * 4 && recordSize<Fault> == faultSize);

/// The record's words by name, in record order: the names host tools print (excReturn is "exc_return").
inline constexpr std::array<std::string_view, faultWords> faultFieldNames{
  "count",      "flags", "r0",   "r1",      "r2",     "r3",    "r12",  "lr",    "pc",
  "xpsr",       "r4",    "r5",   "r6",      "r7",     "r8",    "r9",   "r10",   "r11",
  "exc_return", "msp",   "psp",  "control", "sp",     "cfsr",  "hfsr", "mmfar", "bfar",
  "shcsr",      "icsr",  "sfsr", "sfar",    "msplim", "psplim"};

/// FullRecord::Flag by bit, from bit 0.
inline constexpr std::array<std::string_view, 7> faultFlagNames{"frameValid",
                                                                "calleeSaved",
                                                                "faultRegs",
                                                                "fpFrame",
                                                                "stackLimits",
                                                                "secureRegs",
                                                                "stackCopied"};

/// nullopt: no record (never stored, taken by a boot report, cleared, torn, corrupt).
constexpr std::optional<Fault> decodeFault(std::span<std::byte const> raw) {
    return decodeRecord<Fault>(raw, faultId);
}

// ---- Kvasir::Fault::lastStack<N> (Runtime/FaultRecord.hpp: Persistent<StackSnapshot<N>, StackTag>) ---------
// One symbol per N (the policy's stackBytes): its size is the record's, from N - a GNU ld map shows no size for it.
inline constexpr std::string_view stackSnapshotSymbol{
  "Kvasir::Fault::lastStack<"};   // a prefix: <N>
inline constexpr std::uint32_t stackSnapshotId        = persistentId("KSTK");
inline constexpr std::uint32_t stackSnapshotHeadWords = 2;   // from, bytes

/// lastStack<N>'s size on the target: id, from, bytes, N bytes, crc.
constexpr std::uint32_t stackSnapshotSize(std::uint32_t bytes) {
    return 4 + 4 * stackSnapshotHeadWords + bytes + 4;
}

/// N of a lastStack<N> from its demangled name as a map or nm writes it ("...lastStack<(unsigned short)256>",
/// "...lastStack<256u>"); nullopt for another name, or an N that is no whole number of words.
constexpr std::optional<std::uint32_t> stackSnapshotBytes(std::string_view name) {
    if(!name.starts_with(stackSnapshotSymbol) || !name.ends_with('>')) { return std::nullopt; }
    auto arg
      = name.substr(stackSnapshotSymbol.size(), name.size() - stackSnapshotSymbol.size() - 1);
    if(arg.starts_with('(')) {
        auto const close = arg.find(')');
        if(close == std::string_view::npos) { return std::nullopt; }
        arg.remove_prefix(close + 1);
    }
    while(arg.ends_with('u') || arg.ends_with('U')) { arg.remove_suffix(1); }
    if(arg.empty()) { return std::nullopt; }
    // the bound inside the loop: gcc 16 -O2 got `arg.size() > 5` before it wrong in this constexpr function
    std::uint32_t n{};
    for(char const c : arg) {
        if(c < '0' || c > '9') { return std::nullopt; }
        n = 10 * n + static_cast<std::uint32_t>(c - '0');
        if(n > 0xFFFFU) { return std::nullopt; }   // N is a std::uint16_t
    }
    if(n % 4 != 0) { return std::nullopt; }
    return n;
}

struct StackSnapshot {
    std::uint32_t              from{};    // the address of words[0]
    std::uint32_t              bytes{};   // the valid bytes from `from`; the rest of the copy is 0
    std::vector<std::uint32_t> words;     // the whole copy, N / 4 words

    /// The valid words.
    [[nodiscard]] std::span<std::uint32_t const> body() const {
        return std::span{words}.first(std::min<std::size_t>(bytes / 4, words.size()));
    }
};

/// `raw`: the whole record (stackSnapshotSize(N) bytes); nullopt as decodeFault.
inline std::optional<StackSnapshot> decodeStackSnapshot(std::span<std::byte const> raw) {
    if(raw.size() < stackSnapshotSize(0)) { return std::nullopt; }
    auto const value = persistentValue(raw, stackSnapshotId, raw.size() - 8);
    if(!value) { return std::nullopt; }
    StackSnapshot s{word(*value, 0), word(*value, 4), {}};
    for(std::size_t at = std::size_t{4} * stackSnapshotHeadWords; at + 4 <= value->size(); at += 4)
    {
        s.words.push_back(word(*value, at));
    }
    return s;
}

/// CFSR's bit names (Armv8-M ARM DDI0553B.y D1.2.11: MMFSR 7:0, BFSR 15:8, UFSR 31:16), as kvasir_bench.py's.
/// Bit 7 under its Armv7-M name MMARVALID (DDI0403E.e B3.2.15, md l.21398), which the core SVDs use too; Armv8-M
/// calls it MMFARVALID (DDI0553B.y D1.2.167, md l.85820), same bit.
inline std::string cfsrText(std::uint32_t cfsr) {
    static constexpr std::array<std::pair<unsigned, std::string_view>, 20> bits{
      {{0, "IACCVIOL"},     {1, "DACCVIOL"},    {3, "MUNSTKERR"},  {4, "MSTKERR"},
       {5, "MLSPERR"},      {7, "MMARVALID"},   {8, "IBUSERR"},    {9, "PRECISERR"},
       {10, "IMPRECISERR"}, {11, "UNSTKERR"},   {12, "STKERR"},    {13, "LSPERR"},
       {15, "BFARVALID"},   {16, "UNDEFINSTR"}, {17, "INVSTATE"},  {18, "INVPC"},
       {19, "NOCP"},        {20, "STKOF"},      {24, "UNALIGNED"}, {25, "DIVBYZERO"}}
    };
    std::string out;
    for(auto const& [bit, name] : bits) {
        if(((cfsr >> bit) & 1U) == 0) { continue; }
        if(!out.empty()) { out += ' '; }
        out += name;
    }
    return out.empty() ? std::string{"-"} : out;
}

// ---- Kvasir::Panic::lastPanic (Runtime/Panic.hpp: Persistent<FullRecord, RecordTag>) ------------
inline constexpr std::string_view panicSymbol{"Kvasir::Panic::lastPanic"};
inline constexpr std::uint32_t    panicId    = persistentId("KPNC");
inline constexpr std::size_t      panicWords = 8;   // sizeof(Panic::FullRecord) / 4
inline constexpr std::uint32_t    panicSize  = 4 * (1 + panicWords + 1);

/// Kvasir::Panic::Cause, in order (Panic.hpp's name()); the SDK's tests/records_cross_tests.cpp checks it.
inline constexpr std::array<std::string_view, 18> panicCauses{"assertion",
                                                              "abort",
                                                              "stack smash",
                                                              "division by zero",
                                                              "allocation without a heap",
                                                              "unhandled interrupt",
                                                              "undefined behaviour",
                                                              "fault",
                                                              "KVASIR_PANIC",
                                                              "check failed",
                                                              "register wait timed out",
                                                              "boot loop",
                                                              "health check starved",
                                                              "image CRC mismatch",
                                                              "clock restart failed",
                                                              "secondary core did not come up",
                                                              "stack overflow",
                                                              "clock init failed"};

struct Panic {
    std::uint32_t count{};
    std::uint32_t cause{};
    std::uint32_t pc{};   // raise()'s return address; 0 = no program site
    std::uint32_t
      detail{};   // the cause's number: an unhandled interrupt's exception, exit()'s status, ...
    std::uint32_t core{};       // 0, or 1 when raised on the secondary core's stack
    std::uint32_t sp{};         // the stack pointer where the record was written
    std::uint32_t timeLow{};    // the chip's free-running microsecond counter when it was written
    std::uint32_t timeHigh{};   // (Port::FreeRunningUs; RP: the TIMER), 0 without one

    [[nodiscard]] constexpr std::uint64_t timeUs() const {
        return (static_cast<std::uint64_t>(timeHigh) << 32U) | timeLow;
    }

    [[nodiscard]] std::string causeName() const {
        return cause < panicCauses.size() ? std::string{panicCauses[cause]}
                                          : "cause " + std::to_string(cause);
    }

    /// The call that raised it: a call that does not return may end its function, so the
    /// return address can name the next one.
    [[nodiscard]] constexpr std::optional<std::uint32_t> site() const {
        if(pc == 0) { return std::nullopt; }
        return (pc & ~1U) - 2U;
    }
};

static_assert(sizeof(Panic) == panicWords * 4 && recordSize<Panic> == panicSize);

/// nullopt: no record (as decodeFault).
constexpr std::optional<Panic> decodePanic(std::span<std::byte const> raw) {
    return decodeRecord<Panic>(raw, panicId);
}

// ---- Kvasir::Health::lastStarvation, lastRecovery (Health/HealthRecords.hpp) -------------------------------
inline constexpr std::string_view starvationSymbol{"Kvasir::Health::lastStarvation"};
inline constexpr std::uint32_t    starvationId    = persistentId("KHLT");
inline constexpr std::size_t      starvationWords = 6;

/// The starvation that ended a run.
struct Starvation {
    std::uint32_t check{};      // index in the supervisor's list
    std::uint32_t nameHash{};   // FNV-1a of the check's name
    std::uint32_t silentMs{};
    std::uint32_t allowedMs{};
    std::uint32_t uptimeMs{};
    std::uint32_t silentMask{};   // the checks past half their window at that moment
};

static_assert(sizeof(Starvation) == starvationWords * 4);

constexpr std::optional<Starvation> decodeStarvation(std::span<std::byte const> raw) {
    return decodeRecord<Starvation>(raw, starvationId);
}

inline constexpr std::string_view recoveriesSymbol{"Kvasir::Health::lastRecovery"};
inline constexpr std::uint32_t    recoveriesId    = persistentId("KHRV");
inline constexpr std::size_t      recoveriesWords = 6;

/// The recoveries of a run by a check's onStarved, and the one a reset cut short.
struct Recoveries {
    std::uint32_t total{};   // recoveries that ended in a beat, this run
    std::uint32_t lastCheck{};
    std::uint32_t lastNameHash{};
    std::uint32_t lastUptimeMs{};
    std::uint32_t inProgress{};   // 0, or (check + 1) | (attempt << 16) while an onStarved runs
    std::uint32_t escalatedAttempts{};

    [[nodiscard]] constexpr std::uint32_t inProgressCheck() const {
        return (inProgress & 0xFFFFU) - 1;
    }

    [[nodiscard]] constexpr std::uint32_t inProgressAttempt() const { return inProgress >> 16U; }
};

static_assert(sizeof(Recoveries) == recoveriesWords * 4);

constexpr std::optional<Recoveries> decodeRecoveries(std::span<std::byte const> raw) {
    return decodeRecord<Recoveries>(raw, recoveriesId);
}

// ---- Kvasir::BootGuard::state (Runtime/BootGuardRules.hpp: Persistent<State, Tag>) --------------------------
inline constexpr std::string_view guardSymbol{"Kvasir::BootGuard::state"};
inline constexpr std::uint32_t    guardId    = persistentId("KBTG");
inline constexpr std::size_t      guardWords = 3;

/// BootGuard::Failure, in order.
inline constexpr std::array<std::string_view, 6> guardFailures{"none",
                                                               "panic",
                                                               "fault",
                                                               "watchdog",
                                                               "starvation",
                                                               "external"};
/// BootGuard::Flag by bit, from bit 0.
inline constexpr std::array<std::string_view, 5> guardFlagNames{"runOpen",
                                                                "plannedResetPending",
                                                                "halted",
                                                                "safeMode",
                                                                "healthyThisRun"};

struct GuardState {
    std::uint32_t bootCount{};
    std::uint16_t consecutive{};   // failed runs since the last healthy one
    std::uint16_t total{};         // failed runs in all (saturating)
    std::uint8_t  lastFailure{};   // guardFailures
    std::uint8_t  lastDetail{};    // a panic's cause, a starvation's check
    std::uint8_t  flags{};         // guardFlagNames
    std::uint8_t  reserved{};

    [[nodiscard]] std::string failureName() const {
        return lastFailure < guardFailures.size()
               ? std::string{guardFailures[lastFailure]}
               : "failure " + std::to_string(unsigned{lastFailure});
    }
};

static_assert(sizeof(GuardState) == guardWords * 4);

constexpr std::optional<GuardState> decodeGuard(std::span<std::byte const> raw) {
    return decodeRecord<GuardState>(raw, guardId);
}

// ---- Kvasir::Ubsan::ubsanReports -------------------------------------------------------------
inline constexpr std::string_view ubsanSymbol{"Kvasir::Ubsan::ubsanReports"};

inline constexpr std::uint32_t ubsanSize = 8;

struct Ubsan {
    std::uint32_t count{};
    std::uint32_t lastReturnAddress{};
};

constexpr std::optional<Ubsan> decodeUbsan(std::span<std::byte const> raw) {
    if(raw.size() < 8) { return std::nullopt; }
    return Ubsan{word(raw, 0), word(raw, 4)};
}

// ---- exceptions ------------------------------------------------------------------------------
inline std::string exceptionName(std::uint32_t exception) {
    switch(exception) {
    case 0:  return "thread mode";
    case 2:  return "NMI";
    case 3:  return "HardFault";
    case 4:  return "MemManage";
    case 5:  return "BusFault";
    case 6:  return "UsageFault";
    case 7:  return "SecureFault";
    case 11: return "SVCall";
    case 12: return "DebugMonitor";
    case 14: return "PendSV";
    case 15: return "SysTick";
    default: break;
    }
    return exception >= 16 ? "IRQ " + std::to_string(exception - 16)
                           : "exception " + std::to_string(exception);
}

// ---- Kvasir::StackUsage ----------------------------------------------------------------------
inline constexpr std::uint32_t stackPattern = 0x5AC3'5AC3U;

struct StackUse {
    std::uint32_t size{};
    std::uint32_t used{};    // the deepest use so far
    std::uint32_t never{};   // bytes still painted
};

/// nullopt: not painted (no Kvasir::StackUsage in the Startup list, or a core that does not
/// paint its stack). As StackUsage::free(): skip what precedes the first pattern word (the
/// StackProtector's sentinel), count the pattern run.
inline std::optional<StackUse> stackUse(std::span<std::byte const> raw) {
    auto const  words = raw.size() / 4;
    std::size_t at    = 0;
    while(at < words && word(raw, 4 * at) != stackPattern) { ++at; }
    std::size_t free = 0;
    while(at + free < words && word(raw, 4 * (at + free)) == stackPattern) { ++free; }
    if(free == 0) { return std::nullopt; }
    auto const size = static_cast<std::uint32_t>(4 * words);
    return StackUse{size,
                    static_cast<std::uint32_t>(size - 4 * (at + free)),
                    static_cast<std::uint32_t>(4 * free)};
}

// ---- Kvasir::Trace::Ring ---------------------------------------------------------------------
// A ring is two objects: the constant `descriptor` (magic, fields, capacity, layout address: 12 bytes) and the RAM
// `storage` (count, then the records).
inline constexpr std::uint32_t traceMagic          = 0x4352'544BU;
inline constexpr std::uint32_t traceDescriptorSize = 12;
inline constexpr std::uint32_t traceCountAt        = 0;   // in `storage`
inline constexpr std::uint32_t traceRecordsAt      = 4;   // in `storage`, after the count

struct RingHeader {
    std::uint16_t fields{};
    std::uint16_t capacity{};
    std::uint32_t count{};    // records ever written
    std::uint32_t layout{};   // address of "name:field,field,...\0"

    [[nodiscard]] std::uint32_t storageSize() const {
        return traceRecordsAt + 4U * fields * capacity;
    }
};

/// The storage a descriptor names: the count, then fields x capacity words. nullopt: no ring descriptor. The storage
/// is read by this size, not the map's (a GNU ld map may show none).
constexpr std::optional<std::uint32_t> ringStorageSize(std::span<std::byte const> descriptor) {
    if(descriptor.size() < traceDescriptorSize || word(descriptor, 0) != traceMagic) {
        return std::nullopt;
    }
    auto const fields   = half(descriptor, 4);
    auto const capacity = half(descriptor, 6);
    if(fields == 0 || capacity == 0) { return std::nullopt; }
    return traceRecordsAt + 4U * fields * capacity;
}

inline std::optional<RingHeader> decodeRing(std::span<std::byte const> descriptor,
                                            std::span<std::byte const> storage) {
    if(descriptor.size() < traceDescriptorSize || storage.size() < traceRecordsAt
       || word(descriptor, 0) != traceMagic)
    {
        return std::nullopt;
    }
    RingHeader const h{half(descriptor, 4),
                       half(descriptor, 6),
                       word(storage, traceCountAt),
                       word(descriptor, 8)};
    if(h.fields == 0 || h.capacity == 0 || h.storageSize() > storage.size()) {
        return std::nullopt;
    }
    return h;
}

struct RingLayout {
    std::string              name;
    std::vector<std::string> fields;
};

/// "name:f1,f2" (up to the first NUL).
inline RingLayout decodeRingLayout(std::span<std::byte const> raw) {
    std::string text;
    for(auto b : raw) {
        auto const c = std::to_integer<char>(b);
        if(c == '\0') { break; }
        text.push_back(c);
    }
    RingLayout out;
    auto const colon = text.find(':');
    out.name         = text.substr(0, colon);
    if(colon == std::string::npos) { return out; }
    std::string_view rest{text};
    rest.remove_prefix(colon + 1);
    while(true) {
        auto const comma = rest.find(',');
        out.fields.emplace_back(rest.substr(0, comma));
        if(comma == std::string_view::npos) { break; }
        rest.remove_prefix(comma + 1);
    }
    return out;
}

struct RingRow {
    std::uint32_t              index{};   // the record's number since boot
    std::vector<std::uint32_t> values;
};

/// The records still in the ring, oldest first. `countAfter` is the header's count read again
/// after the records: what record() wrote meanwhile overwrote the oldest, which are dropped.
inline std::vector<RingRow> decodeRingRows(std::span<std::byte const> storage,
                                           RingHeader const&          h,
                                           std::uint32_t              countAfter) {
    std::vector<RingRow> rows;
    if(storage.size() < h.storageSize()) { return rows; }
    auto const kept  = std::min<std::uint32_t>(h.count, h.capacity);
    auto const first = countAfter > h.capacity ? countAfter - h.capacity : 0U;
    for(std::uint32_t i = h.count - kept; i != h.count; ++i) {
        if(i < first) { continue; }
        RingRow    row{i, {}};
        auto const at = traceRecordsAt + 4U * h.fields * (i % h.capacity);
        for(std::uint32_t k = 0; k != h.fields; ++k) {
            row.values.push_back(word(storage, at + 4 * k));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}   // namespace uc_log::printer::records
