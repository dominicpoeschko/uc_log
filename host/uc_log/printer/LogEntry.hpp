#pragma once

#include "uc_log/LogLevel.hpp"
#include "uc_log/printer/SignatureTable.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wreserved-macro-identifier"
    #pragma clang diagnostic ignored "-Wduplicate-enum"
    #pragma clang diagnostic ignored "-Wswitch-enum"
    #pragma clang diagnostic ignored "-Wswitch-default"
    #pragma clang diagnostic ignored "-Wglobal-constructors"
    #pragma clang diagnostic ignored "-Wfloat-equal"
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif

#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <limits>
#include <optional>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace uc_log { namespace printer {

    struct LogEntry {
        struct Channel {
            std::size_t channel;
        };

        struct UcTime {
            std::chrono::nanoseconds time{};
            constexpr UcTime() = default;

            // integer arithmetic end to end: double's 53 bit mantissa would corrupt long
            // or high resolution runs (~104 days at ns resolution). Saturates at the
            // representable maximum instead of overflowing.
            constexpr UcTime(std::uint64_t value,
                             std::uint64_t num,
                             std::uint64_t den)
              : time{[&]() {
                  __extension__ using u128            = unsigned __int128;
                  constexpr std::uint64_t nsPerSecond = 1'000'000'000U;
                  constexpr u128          maxNs
                    = static_cast<u128>(std::numeric_limits<std::chrono::nanoseconds::rep>::max());

                  if(den == 0) { return std::chrono::nanoseconds{0}; }
                  // value * num always fits u128 (both < 2^64); splitting off the
                  // remainder keeps the * nsPerSecond step in range as well
                  u128 const scaled = static_cast<u128>(value) * num;
                  u128 const whole  = scaled / den;
                  u128 const rest   = ((scaled % den) * nsPerSecond) / den;

                  if(whole > maxNs / nsPerSecond) {
                      return std::chrono::nanoseconds{
                        std::numeric_limits<std::chrono::nanoseconds::rep>::max()};
                  }
                  u128 const total = whole * nsPerSecond + rest;
                  if(total > maxNs) {
                      return std::chrono::nanoseconds{
                        std::numeric_limits<std::chrono::nanoseconds::rep>::max()};
                  }
                  return std::chrono::nanoseconds{
                    static_cast<std::chrono::nanoseconds::rep>(total)};
              }()} {}

            constexpr auto operator<=>(UcTime const&) const = default;
        };

        Channel          channel{};
        UcTime           ucTime;
        std::string      fileName;
        std::size_t      line{};
        uc_log::LogLevel logLevel{};
        std::string      functionName;
        // empty for a global function without an explicit module
        std::string module;
        // the call site's demangled signature (SignatureTable); empty when the table does not know it
        std::string signature;
        std::string logMsg;
        // false when the header did not parse: such entries carry defaults (trace, time 0,
        // no location) and must be rendered/counted as unparsed, not trusted
        bool parsedOk{true};

        template<typename Ratio>
        static constexpr auto makeLookUp(std::string_view suffix,
                                         Ratio) {
            return std::make_tuple(suffix,
                                   static_cast<std::uint64_t>(Ratio::type::num),
                                   static_cast<std::uint64_t>(Ratio::type::den));
        }

        static std::optional<UcTime> parseTimeStringStdDuration(std::string_view timeString) {
            static constexpr std::array durationLookup{
              makeLookUp("as", std::atto{}),
              makeLookUp("fs", std::femto{}),
              makeLookUp("ps", std::pico{}),
              makeLookUp("ns", std::nano{}),
              makeLookUp("us", std::micro{}),
              makeLookUp("µs", std::micro{}),
              makeLookUp("ms", std::milli{}),
              makeLookUp("cs", std::centi{}),
              makeLookUp("ds", std::deci{}),
              makeLookUp("s", std::chrono::seconds::period{}),
              makeLookUp("das", std::deca{}),
              makeLookUp("hs", std::hecto{}),
              makeLookUp("ks", std::kilo{}),
              makeLookUp("Ms", std::mega{}),
              makeLookUp("Gs", std::giga{}),
              makeLookUp("Ts", std::tera{}),
              makeLookUp("Ps", std::peta{}),
              makeLookUp("Es", std::exa{}),
              makeLookUp("min", std::chrono::minutes::period{}),
              makeLookUp("h", std::chrono::hours::period{}),
              makeLookUp("d", std::chrono::days::period{})};

            std::uint64_t value{};
            auto const [ptr, ec] = std::from_chars(timeString.begin(), timeString.end(), value);
            if(ec != std::errc{} || ptr == timeString.end()) { return std::nullopt; }

            timeString
              = timeString.substr(static_cast<std::size_t>(std::distance(timeString.begin(), ptr)));

            for(auto const& [prefix, num, den] : durationLookup) {
                if(timeString == prefix) {
                    return {
                      {value, num, den}
                    };
                }
            }

            return std::nullopt;
        }

        static std::optional<UcTime> parseTimeString(std::string_view timeString) {
            if(!timeString.ends_with("]s")) { return parseTimeStringStdDuration(timeString); }
            timeString.remove_suffix(2);

            std::uint64_t value{};
            {
                auto const [ptr, ec] = std::from_chars(timeString.begin(), timeString.end(), value);
                if(ec != std::errc{} || ptr == timeString.end()) { return std::nullopt; }
                timeString.remove_prefix(
                  static_cast<std::size_t>(std::distance(timeString.begin(), ptr)));
            }
            if(!timeString.starts_with('[')) { return std::nullopt; }
            timeString.remove_prefix(1);

            std::uint64_t num{};
            {
                auto const [ptr, ec] = std::from_chars(timeString.begin(), timeString.end(), num);
                if(ec != std::errc{}) { return std::nullopt; }
                timeString.remove_prefix(
                  static_cast<std::size_t>(std::distance(timeString.begin(), ptr)));
            }

            if(timeString.empty()) {
                return {
                  {value, num, 1}
                };
            }
            if(!timeString.starts_with('/')) { return std::nullopt; }
            timeString.remove_prefix(1);

            std::uint64_t den{};
            {
                auto const [ptr, ec] = std::from_chars(timeString.begin(), timeString.end(), den);
                if(ec != std::errc{}) { return std::nullopt; }
                timeString.remove_prefix(
                  static_cast<std::size_t>(std::distance(timeString.begin(), ptr)));
            }

            if(timeString.empty()) {
                return {
                  {value, num, den}
                };
            }

            return std::nullopt;
        }

        // without `site` (SignatureTable) the function is "?"
        LogEntry(std::size_t          channel_,
                 std::string_view     msg,
                 SignatureInfo const* site = nullptr)
          : channel{channel_}
          , parsedOk{false} {
            auto const pos = msg.find(R"("""))");
            if(pos == std::string_view::npos || !msg.starts_with('(')) {
                logMsg = msg;
                return;
            }
            logMsg          = msg.substr(pos + 4);
            auto contextMsg = msg.substr(1, pos - 1);

            if(std::ranges::count(contextMsg, ',') <= 3) { return; }

            auto fileNameSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(fileNameSv.size());

            if(!contextMsg.starts_with(", ")) { return; }
            contextMsg.remove_prefix(2);

            if(2 > fileNameSv.size() || !fileNameSv.starts_with('\"')
               || !fileNameSv.ends_with('\"'))
            {
                return;
            }
            fileNameSv.remove_prefix(1);
            fileNameSv.remove_suffix(1);

            auto const lineSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(lineSv.size());
            if(!contextMsg.starts_with(", ")) { return; }
            contextMsg.remove_prefix(2);
            // uint32, as the producer emits it
            std::uint32_t line_{};
            {
                auto const [ptr, ec] = std::from_chars(lineSv.begin(), lineSv.end(), line_);
                if(ec != std::errc{} || ptr != lineSv.end()) { return; }
            }

            auto const logLevelSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(logLevelSv.size());
            if(!contextMsg.starts_with(", ")) { return; }
            contextMsg.remove_prefix(2);
            std::uint8_t logLevel_{};
            {
                auto const [ptr, ec]
                  = std::from_chars(logLevelSv.begin(), logLevelSv.end(), logLevel_);
                if(ec != std::errc{} || ptr != logLevelSv.end()) { return; }
                if(logLevel_ > static_cast<std::uint8_t>(uc_log::LogLevel::crit)) { return; }
            }

            auto const timeSv  = contextMsg.substr(0, contextMsg.find_first_of(','));
            auto       oUcTime = parseTimeString(timeSv);
            if(!oUcTime) { return; }

            contextMsg.remove_prefix(timeSv.size());
            // optional `, "module"` before the empty `, """`; LogEnv.hpp forbids quotes in it
            std::string_view moduleSv{};
            if(!contextMsg.starts_with(R"(, """)")) {
                if(!contextMsg.starts_with(R"(, ")")) { return; }
                contextMsg.remove_prefix(3);
                auto const end = contextMsg.find('"');
                if(end == 0 || end == std::string_view::npos) { return; }
                moduleSv = contextMsg.substr(0, end);
                contextMsg.remove_prefix(end + 1);
                if(!contextMsg.starts_with(R"(, """)")) { return; }
            }

            ucTime   = *oUcTime;
            fileName = fileNameSv;
            line     = line_;
            logLevel = static_cast<uc_log::LogLevel>(logLevel_);
            module   = moduleSv;
            parsedOk = true;

            if(site == nullptr) {
                functionName = "?";
                return;
            }
            functionName = site->function;
            signature    = site->signature;
            if(module.empty()) { module = site->module; }
        }
    };
}}   // namespace uc_log::printer

template<>
struct fmt::formatter<uc_log::LogLevel> {
    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    // the level's name (the .rttlog column, usblog's lines); the TUI colours a level itself
    template<typename FormatContext>
    auto format(uc_log::LogLevel const& level,
                FormatContext&          ctx) const {
        constexpr std::array<std::string_view, 6> names{"trace",
                                                        "debug",
                                                        "info",
                                                        "warn",
                                                        "error",
                                                        "crit"};
        auto                                      index = static_cast<std::size_t>(level);
        if(index >= names.size()) { index = 0; }
        return fmt::format_to(ctx.out(), "{}", names[index]);
    }
};

template<>
struct fmt::formatter<uc_log::printer::LogEntry::UcTime> {
    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(uc_log::printer::LogEntry::UcTime const& time,
                FormatContext&                           ctx) const {
        auto const days  = std::chrono::duration_cast<std::chrono::days>(time.time);
        auto const hours = std::chrono::duration_cast<std::chrono::hours>(time.time - days);
        auto const minutes
          = std::chrono::duration_cast<std::chrono::minutes>(time.time - (days + hours));
        auto const seconds
          = std::chrono::duration_cast<std::chrono::seconds>(time.time - (days + hours + minutes));
        auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
          time.time - (days + hours + minutes + seconds));
        auto const microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
          time.time - (days + hours + minutes + seconds + milliseconds));
        auto const nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
          time.time - (days + hours + minutes + seconds + milliseconds + microseconds));

        if(days != std::chrono::days{}) { fmt::format_to(ctx.out(), "{:%Q} ", days); }

        return fmt::format_to(ctx.out(),
                              "{:0>2%Q}:{:0>2%Q}:{:0>2%Q}.{:0>3%Q}.{:0>3%Q}.{:0>3%Q}",
                              hours,
                              minutes,
                              seconds,
                              milliseconds,
                              microseconds,
                              nanoseconds);
    }
};
