#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// What the firmware keeps in RAM for a debugger, decoded from raw target bytes: the fault and
/// panic records, the sanitizer counter, a painted stack and the trace rings. The layouts are the
/// SDK's (Kvasir_SDK src/kvasir/Util: FaultHandler.hpp, Panic.hpp, ubsan.hpp, StackUsage.hpp,
/// Trace.hpp) - the same ones `kvasir_bench.py crash|ub|stack|trace` decode.
namespace uc_log::detail::records {

inline std::uint32_t word(std::span<std::byte const> raw,
                          std::size_t                at) {
    std::uint32_t v{};
    for(std::size_t i = 0; i != 4 && at + i < raw.size(); ++i) {
        v |= std::to_integer<std::uint32_t>(raw[at + i]) << (8U * i);
    }
    return v;
}

inline std::uint16_t half(std::span<std::byte const> raw,
                          std::size_t                at) {
    return static_cast<std::uint16_t>(word(raw, at) & 0xFFFFU);
}

// ---- Kvasir::Fault::lastFault ----------------------------------------------------------------
inline constexpr std::string_view FaultSymbol{"Kvasir::Fault::lastFault"};
inline constexpr std::uint32_t    FaultMagic = 0xFA17'C0DEU;
inline constexpr std::uint32_t    FaultSize  = 44;

struct Fault {
    std::uint32_t count{};   // faults since a boot line last reported one; registers: the first
    std::uint32_t pc{};
    std::uint32_t lr{};
    std::uint32_t xpsr{};
    std::uint32_t excReturn{};
    std::array<std::uint32_t, 4> r{};
    std::uint32_t                r12{};

    /// The exception the fault was taken in (xPSR.IPSR of the faulting context).
    std::uint32_t exception() const { return xpsr & 0x1FFU; }
};

/// nullopt: no record (the magic is not there - no fault since the last report).
inline std::optional<Fault> decodeFault(std::span<std::byte const> raw) {
    if(raw.size() < FaultSize || word(raw, 0) != FaultMagic) { return std::nullopt; }
    return Fault{
      word(raw, 4),
      word(raw, 8),
      word(raw, 12),
      word(raw, 16),
      word(raw, 20),
      {word(raw, 24), word(raw, 28), word(raw, 32), word(raw, 36)},
      word(raw, 40)
    };
}

// ---- Kvasir::Panic::lastPanic ----------------------------------------------------------------
inline constexpr std::string_view PanicSymbol{"Kvasir::Panic::lastPanic"};
inline constexpr std::uint32_t    PanicMagic = 0x9A41'C0DEU;

/// Kvasir::Panic::Cause, in order (Panic.hpp's name()).
inline constexpr std::array<std::string_view, 14> PanicCauses{"assertion",
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
                                                              "flash image CRC mismatch"};

struct Panic {
    std::uint32_t                count{};
    std::uint32_t                cause{};
    std::uint32_t                pc{};       // raise()'s return address; 0 = no program site
    std::optional<std::uint32_t> detail{};   // images from before `detail` have 16 bytes

    std::string causeName() const {
        return cause < PanicCauses.size() ? std::string{PanicCauses[cause]}
                                          : "cause " + std::to_string(cause);
    }

    /// The call that raised it: a call that does not return may end its function, so the
    /// return address can name the next one.
    std::optional<std::uint32_t> site() const {
        if(pc == 0) { return std::nullopt; }
        return (pc & ~1U) - 2U;
    }
};

/// `raw` is the whole symbol: 16 bytes (old images) or 20.
inline std::optional<Panic> decodePanic(std::span<std::byte const> raw) {
    if(raw.size() < 16 || word(raw, 0) != PanicMagic) { return std::nullopt; }
    Panic p{word(raw, 4), word(raw, 8), word(raw, 12), std::nullopt};
    if(raw.size() >= 20) { p.detail = word(raw, 16); }
    return p;
}

// ---- Kvasir::Ubsan::ubsanReports -------------------------------------------------------------
inline constexpr std::string_view UbsanSymbol{"Kvasir::Ubsan::ubsanReports"};

struct Ubsan {
    std::uint32_t count{};
    std::uint32_t lastReturnAddress{};
};

inline std::optional<Ubsan> decodeUbsan(std::span<std::byte const> raw) {
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
inline constexpr std::uint32_t StackPattern = 0x5AC3'5AC3U;

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
    while(at < words && word(raw, 4 * at) != StackPattern) { ++at; }
    std::size_t free = 0;
    while(at + free < words && word(raw, 4 * (at + free)) == StackPattern) { ++free; }
    if(free == 0) { return std::nullopt; }
    auto const size = static_cast<std::uint32_t>(4 * words);
    return StackUse{size,
                    static_cast<std::uint32_t>(size - 4 * (at + free)),
                    static_cast<std::uint32_t>(4 * free)};
}

// ---- Kvasir::Trace::Ring ---------------------------------------------------------------------
inline constexpr std::uint32_t TraceMagic      = 0x4352'544BU;
inline constexpr std::uint32_t TraceHeaderSize = 16;

struct RingHeader {
    std::uint16_t fields{};
    std::uint16_t capacity{};
    std::uint32_t count{};    // records ever written
    std::uint32_t layout{};   // address of "name:field,field,...\0"

    std::uint32_t storageSize() const { return TraceHeaderSize + 4U * fields * capacity; }
};

inline std::optional<RingHeader> decodeRingHeader(std::span<std::byte const> raw) {
    if(raw.size() < TraceHeaderSize || word(raw, 0) != TraceMagic) { return std::nullopt; }
    RingHeader h{half(raw, 4), half(raw, 6), word(raw, 8), word(raw, 12)};
    if(h.fields == 0 || h.capacity == 0) { return std::nullopt; }
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
        auto const at = TraceHeaderSize + 4U * h.fields * (i % h.capacity);
        for(std::uint32_t k = 0; k != h.fields; ++k) {
            row.values.push_back(word(storage, at + 4 * k));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}   // namespace uc_log::detail::records
