#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// The printer's control socket protocol (doc/control_protocol/ is generated from it): JSON
/// Lines tagged by "cmd", one Request in, one Answer out; after a Subscribe, only Events out.
namespace uc_log::control {

/// Raised whenever a line changes meaning; clients refuse any other version.
inline constexpr int ProtocolVersion = 1;

inline constexpr std::string_view SocketName = "control.sock";

inline constexpr std::size_t   MaxPieces        = 64;
inline constexpr std::size_t   MaxReadBytes     = 4096;
inline constexpr std::uint32_t MaxWaitBytes     = 8;
inline constexpr std::uint32_t MaxWaitTimeoutMs = 600'000;

struct Piece {
    std::uint32_t address{};
    std::uint32_t size{};
};

enum class Condition : std::uint8_t { changed, eq, ne, lt, gt };
enum class Stream : std::uint8_t { log, messages, metrics };
enum class FirmwareState : std::uint8_t { unchecked, match, different };
/// Mirrors uc_log::LogLevel.
enum class Level : std::uint8_t { trace, debug, info, warn, error, crit };

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

/// Polls one little-endian value until the condition holds or the timeout runs out.
struct Wait {
    Piece                        piece;
    Condition                    condition{Condition::changed};
    std::optional<std::uint64_t> value;
    std::optional<std::uint64_t> mask;
    std::uint32_t                timeout_ms{};
};

/// Answered once the log runs again.
struct Reset {};

/// Downloads the printer's hex file; answered once the log runs again.
struct Flash {};

/// Filters apply to the log stream only. `since_seq` / `last` send the history first, then a
/// BacklogEnd; `follow` false closes the connection after it.
struct Subscribe {
    std::vector<Stream>          streams;
    std::optional<Level>         min_level;
    std::vector<std::string>     modules;
    std::vector<std::string>     not_modules;
    std::optional<std::uint64_t> since_seq;
    std::optional<std::uint32_t> last;
    bool                         follow{true};
};

using Request = std::variant<Ping, Status, Messages, Read, Wait, Reset, Flash, Subscribe>;

struct PingAnswer {
    int protocol{ProtocolVersion};
};

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
    std::uint64_t errors{};       // error lines in the Status tab
    std::uint64_t log_seq{};      // the seq the next log line gets
    std::int64_t  started_us{};   // seqs are only valid for this start
};

/// A line of the Status tab.
struct StatusMessage {
    std::string time;
    std::string level;
    std::string text;
};

struct MessagesAnswer {
    std::vector<StatusMessage> messages;
};

/// `data`: one lowercase hex string per piece. `unix_us`: host clock, the one of recv_time.
struct ReadAnswer {
    std::int64_t             unix_us{};
    std::vector<std::string> data;
};

struct WaitAnswer {
    bool          hit{};
    std::int64_t  unix_us{};
    std::int64_t  waited_us{};
    std::uint64_t first{};
    std::uint64_t last{};
};

struct ResetAnswer {};

struct FlashAnswer {};

struct SubscribeAnswer {
    int protocol{ProtocolVersion};
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
                            Error>;

/// The columns of a .rttlog row, and `seq`: numbered from 0 at the printer's start.
struct LogLine {
    std::uint64_t seq{};
    std::string   recv_time;   // ISO 8601 UTC, host clock
    std::uint32_t channel{};
    std::string   file;
    std::uint32_t line{};
    std::string   function;
    Level         level{Level::info};
    std::int64_t  uc_time_ns{};
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
    std::uint64_t next_seq{};
    std::uint64_t lost{};
};

using Event = std::variant<LogLine, StatusMessage, MetricSample, BacklogEnd>;

}   // namespace uc_log::control

// Explicit specializations: they win over FTXUIGui.hpp's catch-all enum meta.
template<>
struct glz::meta<uc_log::control::Condition> {
    using enum uc_log::control::Condition;
    static constexpr auto value = glz::enumerate(changed, eq, ne, lt, gt);
};

template<>
struct glz::meta<uc_log::control::Stream> {
    using enum uc_log::control::Stream;
    static constexpr auto value = glz::enumerate(log, messages, metrics);
};

template<>
struct glz::meta<uc_log::control::FirmwareState> {
    using enum uc_log::control::FirmwareState;
    static constexpr auto value = glz::enumerate(unchecked, match, different);
};

template<>
struct glz::meta<uc_log::control::Level> {
    using enum uc_log::control::Level;
    static constexpr auto value = glz::enumerate(trace, debug, info, warn, error, crit);
};

template<>
struct glz::meta<uc_log::control::Request> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto             ids
      = std::array{"ping", "status", "messages", "read", "wait", "reset", "flash", "subscribe"};
};

template<>
struct glz::meta<uc_log::control::Answer> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto             ids = std::array{"ping",
                                                       "status",
                                                       "messages",
                                                       "read",
                                                       "wait",
                                                       "reset",
                                                       "flash",
                                                       "subscribe",
                                                       "error"};
};

template<>
struct glz::meta<uc_log::control::Event> {
    static constexpr std::string_view tag = "cmd";
    static constexpr auto             ids = std::array{"log", "message", "metric", "backlog_end"};
};

namespace uc_log::control {

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

/// "i2c.bus" is under "i2c" and under itself, not under "i2" or "i2c.b".
inline bool moduleUnder(std::string_view module,
                        std::string_view prefix) {
    return module == prefix
        || (module.size() > prefix.size() && module.starts_with(prefix)
            && module[prefix.size()] == '.');
}

inline bool passes(Subscribe const& filter,
                   Level            level,
                   std::string_view module) {
    if(filter.min_level && level < *filter.min_level) { return false; }
    auto const under = [&](std::vector<std::string> const& prefixes) {
        for(auto const& p : prefixes) {
            if(moduleUnder(module, p)) { return true; }
        }
        return false;
    };
    if(!filter.modules.empty() && !under(filter.modules)) { return false; }
    return !under(filter.not_modules);
}

inline bool passes(Subscribe const& filter,
                   LogLine const&   line) {
    return passes(filter, line.level, line.module);
}

inline std::optional<std::string> subscribeError(Subscribe const& s) {
    bool const log = std::ranges::find(s.streams, Stream::log) != s.streams.end();
    if((s.since_seq || s.last) && !log) {
        return "subscribe: since_seq and last need the log stream";
    }
    if(s.since_seq && s.last) { return "subscribe: since_seq or last, not both"; }
    if(!s.follow && !s.since_seq && !s.last) {
        return "subscribe: follow false needs since_seq or last";
    }
    if(!s.follow && std::ranges::any_of(s.streams, [](Stream x) { return x != Stream::log; })) {
        return "subscribe: follow false takes the log stream only";
    }
    return std::nullopt;
}

template<typename T>
constexpr Stream streamOf() {
    if constexpr(std::is_same_v<T, LogLine> || std::is_same_v<T, BacklogEnd>) {
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

}   // namespace uc_log::control
