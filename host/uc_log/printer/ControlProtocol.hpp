#pragma once

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/ModulePath.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// The printer's control socket protocol (doc/control_protocol/ is generated from it): JSON
/// Lines tagged by "cmd", one Request in, one Answer out; after a Subscribe, only Events out.
namespace uc_log::printer::control {

inline constexpr std::string_view socketName = "control.sock";

inline constexpr std::size_t   maxPieces        = 64;
inline constexpr std::size_t   maxReadBytes     = 4096;
inline constexpr std::uint32_t maxWaitBytes     = 8;
inline constexpr std::uint32_t maxWaitTimeoutMs = 600'000;
inline constexpr std::size_t   maxWriteWords    = 16;

struct Piece {
    std::uint32_t address{};
    std::uint32_t size{};
};

enum class Condition : std::uint8_t { changed, eq, ne, lt, gt };
enum class Stream : std::uint8_t { log, messages, metrics };
enum class FirmwareState : std::uint8_t { unchecked, match, different };
/// Mirrors uc_log::LogLevel.
enum class Level : std::uint8_t { trace, debug, info, warn, error, crit };
/// What a `records` request reads: `crash` the fault record, its stack snapshot and the panic record; `health` the
/// health supervisor's starvation and recovery records and the boot guard's state; `ubsan` the sanitizer counter;
/// `stacks` the painted stacks; `trace` the Trace::Rings.
enum class RecordKind : std::uint8_t { crash, health, ubsan, stacks, trace };

struct Ping {};

struct Status {};

/// The newest `count` lines of the printer's Status tab, oldest first.
struct Messages {
    std::uint32_t count{50};
};

/// Target memory, read while the core runs.
struct Read {
    std::vector<Piece> pieces;
};

struct WordWrite {
    std::uint32_t address{};
    std::uint32_t value{};
};

/// Aligned 32-bit words written in order while the core runs, each read back after its write.
struct Write {
    std::vector<WordWrite> words;
};

/// Polls one little-endian value until the condition holds or the timeout runs out.
struct Wait {
    Piece                        piece;
    Condition                    condition{Condition::changed};
    std::optional<std::uint64_t> value;
    std::optional<std::uint64_t> mask;
    std::uint32_t                timeoutMs{};
};

/// Answered once the log runs again. `line`: through the probe's reset pin (nRESET on the chip's reset input, e.g. an
/// RP2040/RP2350 RUN pin): a chip-level reset, where the ordinary one is the target's own reset strategy.
struct Reset {
    std::optional<bool> line;
};

/// Downloads the printer's hex file; answered once the log runs again.
struct Flash {};

/// Halts / resumes the core through the probe's own calls (the J-Link DLL's halt and go): a halt or resume written
/// into DHCSR behind the probe's back hangs the J-Link DLL, so `write` refuses DHCSR. Answered with the state after.
struct Halt {};

struct Go {};

/// The core's registers through the probe's own calls, only while it is halted (`halt` first): r0..r12, sp, lr, pc,
/// xpsr, msp, psp in that order.
struct Registers {};

/// Core registers written through the probe's own calls, only while halted; `index` as in Registers (below
/// cortex_m::coreRegisterCount, CortexM.hpp).
struct RegisterWrite {
    std::uint32_t index{};
    std::uint32_t value{};
};

struct WriteRegisters {
    std::vector<RegisterWrite> writes;
};

/// What the firmware leaves for a debugger, found in the printer's map (--map_file) and decoded by the printer
/// (TargetRecords.hpp, RecordReader.hpp): the one host decoder of the records - kvasir_bench.py asks it.
struct Records {
    std::vector<RecordKind> kinds;
};

/// Makes the core call Kvasir::Panic::raise(cause), as the TUI's "Raise panic" does (PanicRaise.hpp): `cause` one of
/// RecordsAnswer::panicCauses, or that name with dashes for spaces ("check-failed"; KVASIR_PANIC's is "user").
struct RaisePanic {
    std::string cause;
};

/// Filters apply to the log stream only. `since_seq` / `last` send the history first, then a
/// BacklogEnd; `follow` false closes the connection after it.
struct Subscribe {
    std::vector<Stream>          streams;
    std::optional<Level>         minLevel;
    std::vector<std::string>     modules;
    std::vector<std::string>     notModules;
    std::optional<std::uint64_t> sinceSeq;
    std::optional<std::uint32_t> last;
    bool                         follow{true};
};

using Request = std::variant<Ping,
                             Status,
                             Messages,
                             Read,
                             Wait,
                             Reset,
                             Flash,
                             Subscribe,
                             Write,
                             Halt,
                             Go,
                             Registers,
                             WriteRegisters,
                             Records,
                             RaisePanic>;

struct PingAnswer {};

/// Target flash compared with the hex file at session start; `build` = CRC-32 of its data bytes.
struct FirmwareCheck {
    FirmwareState                state{FirmwareState::unchecked};
    std::optional<std::uint32_t> build;
};

struct StatusAnswer {
    bool          running{};   // a J-Link session is up and reading RTT
    bool          halted{};
    bool          flashing{};
    std::uint32_t sessions{};   // J-Link sessions since the printer started
    FirmwareCheck firmware;
    std::uint64_t errors{};      // error lines in the Status tab
    std::uint64_t logSeq{};      // the seq the next log line gets
    std::int64_t  startedUs{};   // seqs are only valid for this start
    // started with --ram_image: flash and reset load the image into RAM and start it there
    bool ramImage{};
    // log records the target said it dropped (its "N log records lost" lines, LossPolicy::Counted)
    // since the printer started
    std::uint64_t targetLost{};
    // bytes of log the printer read from the target's rings since it started (a throughput measurement's counter)
    std::uint64_t rttBytes{};
    // the serial number of the J-Link it holds (0 before the first session)
    std::uint32_t probeSerial{};
};

/// What a message is when it is not a plain one; the key is absent for a plain message. `halt`: the core halted on its
/// own (a fault handler's or a panic's bkpt, a lockup) or by the TUI's captured halt - its text is the `core halted:`
/// line. `format_error`: a log line lost to its format string (the printer could not render it; the TUI's 📝 count).
/// A client matches the kind, never the text.
enum class MessageKind : std::uint8_t { halt, formatError };

/// A line of the Status tab.
struct StatusMessage {
    std::string                time;
    std::string                level;
    std::string                text;
    std::optional<MessageKind> kind{};
};

struct MessagesAnswer {
    std::vector<StatusMessage> messages;
};

/// `data`: one lowercase hex string per piece. `unix_us`: host clock, the one of recv_time.
struct ReadAnswer {
    std::int64_t             unixUs{};
    std::vector<std::string> data;
};

struct WriteAnswer {
    std::int64_t             unixUs{};
    std::vector<std::string> data;
};

struct WaitAnswer {
    bool          hit{};
    std::int64_t  unixUs{};
    std::int64_t  waitedUs{};
    std::uint64_t first{};
    std::uint64_t last{};
};

struct ResetAnswer {};

struct FlashAnswer {};

struct HaltAnswer {
    bool halted{};
};

struct GoAnswer {
    bool halted{};
};

struct RegistersAnswer {
    std::vector<std::uint32_t> values;   // Registers' order
};

struct WriteRegistersAnswer {};

struct SubscribeAnswer {};

/// The map a `records` answer was read with: the printer's --map_file, and how many parses of it (a rebuild under the
/// running printer is a new one) came before.
struct MapInUse {
    std::string   path;
    std::uint64_t generation{};
};

// One record of a `records` answer, where the image has its symbol (else the answer has no key for it): `valid` false
// = its bytes are no record (never stored, taken by a boot report, cleared, torn) or the read failed (`error`). A
// valid one has `record`, its fields by their names in TargetRecords.hpp, and what they mean decoded beside it.

/// One of the fault record's words: its name as a host tool prints it (faultFieldNames).
struct NamedWord {
    std::string   name;
    std::uint32_t value{};
};

struct FaultAnswer {
    std::uint32_t                           address{};
    bool                                    valid{};
    std::optional<std::string>              error;
    std::optional<records::Fault>           record;
    std::optional<std::uint32_t>            core;
    std::optional<std::uint32_t>            arch;   // 6, 7, 8: Armv<N>-M
    std::optional<std::vector<std::string>> flagNames;
    std::optional<std::vector<std::string>> cfsrNames;
    std::optional<std::uint32_t>            exception;   // xPSR.IPSR of the faulting context
    std::optional<std::string>              exceptionName;
    std::optional<std::vector<NamedWord>>   words;   // all 33, in record order
};

struct StackSnapshotAnswer {
    std::uint32_t                             address{};
    bool                                      valid{};
    std::optional<std::string>                error;
    std::optional<std::uint32_t>              from;    // the address of body[0]
    std::optional<std::uint32_t>              bytes;   // the valid bytes from `from`
    std::optional<std::vector<std::uint32_t>> body;    // the valid words
};

struct PanicAnswer {
    std::uint32_t                 address{};
    bool                          valid{};
    std::optional<std::string>    error;
    std::optional<records::Panic> record;
    std::optional<std::string>    causeName;
    std::optional<std::uint64_t>  timeUs;   // 0: no counter, or it did not run
    std::optional<std::uint32_t>  site;     // the raising call; absent: no program site (pc 0)
};

struct StarvationAnswer {
    std::uint32_t                      address{};
    bool                               valid{};
    std::optional<std::string>         error;
    std::optional<records::Starvation> record;
};

struct RecoveriesAnswer {
    std::uint32_t                      address{};
    bool                               valid{};
    std::optional<std::string>         error;
    std::optional<records::Recoveries> record;
    // only while an onStarved ran (inProgress not 0): the check and the attempt a reset cut short
    std::optional<std::uint32_t> inProgressCheck;
    std::optional<std::uint32_t> inProgressAttempt;
};

struct GuardAnswer {
    std::uint32_t                           address{};
    bool                                    valid{};
    std::optional<std::string>              error;
    std::optional<records::GuardState>      record;
    std::optional<std::string>              failureName;
    std::optional<std::vector<std::string>> guardFlagNames;
};

struct UbsanAnswer {
    std::uint32_t                 address{};
    bool                          valid{};
    std::optional<std::string>    error;
    std::optional<records::Ubsan> record;
};

/// A core's stack; `valid` false: the read failed (`error`), or without an error it is not painted (no
/// Kvasir::StackUsage for it).
struct StackAnswer {
    std::string                  name;   // "core 0 stack", "core 1 stack"
    std::uint32_t                low{};
    std::uint32_t                high{};
    bool                         valid{};
    std::optional<std::string>   error;
    std::optional<std::uint32_t> size;
    std::optional<std::uint32_t> used;    // the deepest use so far
    std::optional<std::uint32_t> never;   // bytes still painted
};

struct RingRowAnswer {
    std::uint32_t              index{};   // the record's number since boot
    std::vector<std::uint32_t> values;
};

/// A Trace::Ring; `valid` false: its bytes are no ring, or the read failed (`error`). `rows`: the records still in
/// it, oldest first, without those the records written while it was read may have overwritten.
struct RingAnswer {
    std::string                               symbol;      // the storage's
    std::uint32_t                             address{};   // the storage
    bool                                      valid{};
    std::optional<std::string>                error;
    std::optional<std::string>                name;         // from its layout string
    std::optional<std::vector<std::string>>   fieldNames;   // one per field
    std::optional<std::uint32_t>              capacity;
    std::optional<std::uint32_t>              count;   // records ever written, when the read began
    std::optional<std::uint32_t>              writtenWhileReading;
    std::optional<std::vector<RingRowAnswer>> rows;
};

/// A record kind not asked for, or one the image has no symbol for, has no key. `panic_causes`: Kvasir::Panic::Cause
/// by number; `exception_names`: exceptions 0-15 by number (16 and up are IRQ n - 16).
struct RecordsAnswer {
    MapInUse                                map;
    std::optional<FaultAnswer>              fault;
    std::optional<StackSnapshotAnswer>      stack;
    std::optional<PanicAnswer>              panic;
    std::optional<StarvationAnswer>         starvation;
    std::optional<RecoveriesAnswer>         recoveries;
    std::optional<GuardAnswer>              guard;
    std::optional<UbsanAnswer>              ubsan;
    std::optional<std::vector<StackAnswer>> stacks;
    std::optional<std::vector<RingAnswer>>  rings;
    std::vector<std::string>                panicCauses;
    std::vector<std::string>                exceptionNames;
};

/// raise() was called with `cause` (`name`: its record name); the next boot line names the panic.
struct RaisePanicAnswer {
    std::uint32_t cause{};
    std::string   name;
};

struct Error {
    std::string error;
};

using Answer = std::variant<PingAnswer,
                            StatusAnswer,
                            MessagesAnswer,
                            ReadAnswer,
                            WaitAnswer,
                            ResetAnswer,
                            FlashAnswer,
                            SubscribeAnswer,
                            WriteAnswer,
                            HaltAnswer,
                            GoAnswer,
                            RegistersAnswer,
                            WriteRegistersAnswer,
                            RecordsAnswer,
                            RaisePanicAnswer,
                            Error>;

/// The columns of a .rttlog row, and `seq`: numbered from 0 at the printer's start.
struct LogLine {
    std::uint64_t seq{};
    std::string   recvTime;   // ISO 8601 UTC, host clock
    std::uint32_t channel{};
    std::string   file;
    std::uint32_t line{};
    std::string   function;
    Level         level{Level::info};
    std::int64_t  ucTimeNs{};
    std::string   message;
    std::string   module;
};

/// `time`: seconds on the target's clock.
struct MetricSample {
    std::string name;
    std::string scope;
    std::string unit;
    double      time{};
    double      value{};
};

/// Ends a subscription's history; `lost`: requested lines the printer no longer holds.
struct BacklogEnd {
    std::uint64_t nextSeq{};
    std::uint64_t lost{};
};

/// Events this client did not get: its send queue was full (it read too slowly), so `lines` events were dropped for it
/// alone. Sent once the queue has room again, in the stream where the gap is.
struct Dropped {
    std::uint64_t lines{};
};

using Event = std::variant<LogLine, StatusMessage, MetricSample, BacklogEnd, Dropped>;

/// The wire keys keep the protocol's spelling (doc/control_protocol, the golden files) while the members follow the
/// code's: a struct with a renamed member gives glaze the old key through rename_key (member -> key).
constexpr std::string_view wireKey(std::string_view member UC_LOG_LIFETIMEBOUND,
                                   std::initializer_list<std::pair<std::string_view,
                                                                   std::string_view>> keys) {
    for(auto const& [m, k] : keys) {
        if(m == member) { return k; }
    }
    return member;
}
}   // namespace uc_log::printer::control

// Explicit specializations: they win over Window.hpp's catch-all enum meta.
template<>
struct glz::meta<uc_log::printer::control::Condition> {
    using enum uc_log::printer::control::Condition;
    static constexpr auto value = glz::enumerate(changed, eq, ne, lt, gt);
};

template<>
struct glz::meta<uc_log::printer::control::Stream> {
    using enum uc_log::printer::control::Stream;
    static constexpr auto value = glz::enumerate(log, messages, metrics);
};

template<>
struct glz::meta<uc_log::printer::control::FirmwareState> {
    using enum uc_log::printer::control::FirmwareState;
    static constexpr auto value = glz::enumerate(unchecked, match, different);
};

template<>
struct glz::meta<uc_log::printer::control::MessageKind> {
    using enum uc_log::printer::control::MessageKind;
    static constexpr auto value = glz::enumerate("halt", halt, "format_error", formatError);
};

template<>
struct glz::meta<uc_log::printer::control::Level> {
    using enum uc_log::printer::control::Level;
    static constexpr auto value = glz::enumerate(trace, debug, info, warn, error, crit);
};

template<>
struct glz::meta<uc_log::printer::control::RecordKind> {
    using enum uc_log::printer::control::RecordKind;
    static constexpr auto value = glz::enumerate(crash, health, ubsan, stacks, trace);
};

template<>
struct glz::meta<uc_log::printer::control::Request> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto             ids = std::array{"ping",
                                                       "status",
                                                       "messages",
                                                       "read",
                                                       "wait",
                                                       "reset",
                                                       "flash",
                                                       "subscribe",
                                                       "write",
                                                       "halt",
                                                       "go",
                                                       "registers",
                                                       "write_registers",
                                                       "records",
                                                       "raise_panic"};
};

template<>
struct glz::meta<uc_log::printer::control::Answer> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto             ids = std::array{"ping",
                                                       "status",
                                                       "messages",
                                                       "read",
                                                       "wait",
                                                       "reset",
                                                       "flash",
                                                       "subscribe",
                                                       "write",
                                                       "halt",
                                                       "go",
                                                       "registers",
                                                       "write_registers",
                                                       "records",
                                                       "raise_panic",
                                                       "error"};
};

template<>
struct glz::meta<uc_log::printer::control::Event> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto ids = std::array{"log", "message", "metric", "backlog_end", "dropped"};
};

template<>
struct glz::meta<uc_log::printer::control::Wait> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"timeoutMs", "timeout_ms"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::Subscribe> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {  "minLevel",   "min_level"},
                                                   {"notModules", "not_modules"},
                                                   {  "sinceSeq",   "since_seq"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::StatusAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {     "logSeq",      "log_seq"},
                                                   {  "startedUs",   "started_us"},
                                                   {   "ramImage",    "ram_image"},
                                                   { "targetLost",  "target_lost"},
                                                   {   "rttBytes",    "rtt_bytes"},
                                                   {"probeSerial", "probe_serial"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::ReadAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"unixUs", "unix_us"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::WriteAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"unixUs", "unix_us"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::WaitAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {  "unixUs",   "unix_us"},
                                                   {"waitedUs", "waited_us"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::FaultAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {    "flagNames",     "flag_names"},
                                                   {    "cfsrNames",     "cfsr_names"},
                                                   {"exceptionName", "exception_name"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::PanicAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"causeName", "cause_name"},
                                                   {   "timeUs",    "time_us"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::RecoveriesAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {  "inProgressCheck",   "in_progress_check"},
                                                   {"inProgressAttempt", "in_progress_attempt"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::GuardAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {   "failureName",     "failure_name"},
                                                   {"guardFlagNames", "guard_flag_names"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::RingAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(
          member,
          {
            {         "fieldNames",           "field_names"},
            {"writtenWhileReading", "written_while_reading"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::RecordsAnswer> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {   "panicCauses",    "panic_causes"},
                                                   {"exceptionNames", "exception_names"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::LogLine> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"recvTime",  "recv_time"},
                                                   {"ucTimeNs", "uc_time_ns"}
        });
    }
};

template<>
struct glz::meta<uc_log::printer::control::BacklogEnd> {
    static constexpr std::string_view rename_key(std::string_view member UC_LOG_LIFETIMEBOUND) {
        return uc_log::printer::control::wireKey(member,
                                                 {
                                                   {"nextSeq", "next_seq"}
        });
    }
};

namespace uc_log::printer::control {

struct WriteOptions : glz::opts {
    bool escape_control_characters = true;
};

/// Replaces invalid UTF-8 by U+FFFD (only strings can hold such bytes).
inline void makeUtf8(std::string& text) {
    auto const  byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    auto const  cont = [&](std::size_t i) { return i < text.size() && (byte(i) & 0xC0U) == 0x80U; };
    std::string out;
    std::size_t copied = 0;
    std::size_t i      = 0;
    while(i < text.size()) {
        unsigned char const b = byte(i);
        std::size_t         n = 0;
        if(b < 0x80U) {
            n = 1;
        } else if(b >= 0xC2U && b <= 0xDFU && cont(i + 1)) {
            n = 2;
        } else if(b >= 0xE0U && b <= 0xEFU && cont(i + 1) && cont(i + 2)) {
            unsigned char const b1        = byte(i + 1);
            bool const          overlong  = b == 0xE0U && b1 < 0xA0U;
            bool const          surrogate = b == 0xEDU && b1 >= 0xA0U;
            n                             = overlong || surrogate ? 0 : 3;
        } else if(b >= 0xF0U && b <= 0xF4U && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            unsigned char const b1       = byte(i + 1);
            bool const          overlong = b == 0xF0U && b1 < 0x90U;
            bool const          tooLarge = b == 0xF4U && b1 >= 0x90U;
            n                            = overlong || tooLarge ? 0 : 4;
        }
        if(n != 0) {
            i += n;
            continue;
        }
        out.append(text, copied, i - copied);
        out += "\xEF\xBF\xBD";
        ++i;
        copied = i;
    }
    if(copied != 0) {
        out.append(text, copied);
        text = std::move(out);
    }
}

template<typename T>
std::string toLine(T const& value) {
    std::string line;
    if(auto const ec = glz::write<WriteOptions{}>(value, line); ec) {
        // cannot happen for these types; keep the stream line based anyway
        line = R"({"cmd":"error","error":"cannot serialize"})";
    }
    makeUtf8(line);
    line += '\n';
    return line;
}

template<typename T>
std::variant<T,
             Error>
fromLine(std::string_view line) {
    while(!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    // glaze reads up to a terminating '\0'; a view would be read past its end
    std::string const buffer{line};
    T                 value{};
    if(auto const ec = glz::read_json(value, buffer); ec) {
        return Error{.error = "bad line: " + glz::format_error(ec, buffer)};
    }
    return value;
}

inline std::variant<Request,
                    Error>
parseRequest(std::string_view line) {
    return fromLine<Request>(line);
}

inline bool passes(Subscribe const& filter,
                   Level            level,
                   std::string_view module) {
    if(filter.minLevel && level < *filter.minLevel) { return false; }
    auto const under = [&](std::vector<std::string> const& prefixes) {
        for(auto const& p : prefixes) {
            if(printer::moduleUnder(module, p)) { return true; }
        }
        return false;
    };
    if(!filter.modules.empty() && !under(filter.modules)) { return false; }
    return !under(filter.notModules);
}

inline bool passes(Subscribe const& filter,
                   LogLine const&   line) {
    return passes(filter, line.level, line.module);
}

inline std::optional<std::string> subscribeError(Subscribe const& s) {
    bool const log = std::ranges::find(s.streams, Stream::log) != s.streams.end();
    if((s.sinceSeq || s.last) && !log) {
        return "subscribe: since_seq and last need the log stream";
    }
    if(s.sinceSeq && s.last) { return "subscribe: since_seq or last, not both"; }
    if(!s.follow && !s.sinceSeq && !s.last) {
        return "subscribe: follow false needs since_seq or last";
    }
    if(!s.follow && std::ranges::any_of(s.streams, [](Stream x) { return x != Stream::log; })) {
        return "subscribe: follow false takes the log stream only";
    }
    return std::nullopt;
}

template<typename T>
constexpr Stream streamOf() {
    // Dropped: per client, never published; it counts with the log stream for whoever visits every event
    if constexpr(std::is_same_v<T, LogLine> || std::is_same_v<T, BacklogEnd>
                 || std::is_same_v<T, Dropped>)
    {
        return Stream::log;
    } else if constexpr(std::is_same_v<T, StatusMessage>) {
        return Stream::messages;
    } else {
        static_assert(std::is_same_v<T, MetricSample>);
        return Stream::metrics;
    }
}

template<typename T>
bool delivers(Subscribe const& subscription,
              T const&         event) {
    bool subscribed = false;
    for(auto const s : subscription.streams) { subscribed = subscribed || s == streamOf<T>(); }
    if(!subscribed) { return false; }
    if constexpr(std::is_same_v<T, LogLine>) {
        return passes(subscription, event);
    } else {
        return true;
    }
}

}   // namespace uc_log::printer::control
