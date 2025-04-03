#pragma once

#include "uc_log/LogLevel.hpp"

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

#include <optional>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace uc_log { namespace detail {

    static std::string
    to_time_string_with_milliseconds(std::chrono::system_clock::time_point const& value) {
        //00:00:00.000
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
          value - std::chrono::time_point_cast<std::chrono::minutes>(value));
        auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
          value - std::chrono::time_point_cast<std::chrono::seconds>(value));
        return fmt::format("{:%H:%M}:{:02}.{:03}", value, seconds.count(), milliseconds.count());
    }
    struct LogEntry {
        struct Channel {
            std::size_t channel;
        };

        struct UcTime {
            std::chrono::nanoseconds time{};
            constexpr UcTime() = default;
            constexpr UcTime(std::uint64_t value, std::uint64_t num, std::uint64_t den)
              //TODO overflow...
              : time{std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>{(static_cast<double>(value) * num) / den})} {}
            constexpr auto operator<=>(UcTime const&) const = default;
        };

        Channel          channel{};
        UcTime           ucTime{};
        std::string      fileName{};
        std::size_t      line{};
        uc_log::LogLevel logLevel{};
        std::string      functionName{};
        std::string      logMsg{};

        template<typename Ratio>
        static constexpr auto makeLookUp(std::string_view suffix, Ratio) {
            return std::make_tuple(
              suffix,
              static_cast<std::uint64_t>(Ratio::type::num),
              static_cast<std::uint64_t>(Ratio::type::den));
        }

        static std::optional<UcTime> parseTimeStringStdDuration(std::string_view ts) {
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

            std::uint64_t value;
            auto const [ptr, ec] = std::from_chars(ts.begin(), ts.end(), value);
            if(ec != std::errc{} || ptr == ts.end()) {
                return std::nullopt;
            }

            ts = ts.substr(static_cast<std::size_t>(std::distance(ts.begin(), ptr)));

            for(auto const& [prefix, num, den] : durationLookup) {
                if(ts == prefix) {
                    return {
                      {value, num, den}
                    };
                }
            }

            return std::nullopt;
        }

        static std::optional<UcTime> parseTimeString(std::string_view ts) {
            if(!ts.ends_with("]s")) {
                return parseTimeStringStdDuration(ts);
            }
            ts.remove_suffix(2);

            std::uint64_t value;
            {
                auto const [ptr, ec] = std::from_chars(ts.begin(), ts.end(), value);
                if(ec != std::errc{} || ptr == ts.end()) {
                    return std::nullopt;
                }
                ts.remove_prefix(static_cast<std::size_t>(std::distance(ts.begin(), ptr)));
            }
            if(!ts.starts_with('[')) {
                return std::nullopt;
            }
            ts.remove_prefix(1);

            std::uint64_t num;
            {
                auto const [ptr, ec] = std::from_chars(ts.begin(), ts.end(), num);
                if(ec != std::errc{}) {
                    return std::nullopt;
                }
                ts.remove_prefix(static_cast<std::size_t>(std::distance(ts.begin(), ptr)));
            }

            if(ts.empty()) {
                return {
                  {value, num, 1}
                };
            }
            if(!ts.starts_with('/')) {
                return std::nullopt;
            }
            ts.remove_prefix(1);

            std::uint64_t den;
            {
                auto const [ptr, ec] = std::from_chars(ts.begin(), ts.end(), den);
                if(ec != std::errc{}) {
                    return std::nullopt;
                }
                ts.remove_prefix(static_cast<std::size_t>(std::distance(ts.begin(), ptr)));
            }

            if(ts.empty()) {
                return {
                  {value, num, den}
                };
            }

            return std::nullopt;
        }

        LogEntry(std::size_t channel_, std::string_view msg) : channel{channel_} {
            auto const pos = msg.find("\"\"\")");
            if(pos == std::string_view::npos || !msg.starts_with("(")) {
                logMsg = msg;
                return;
            }
            logMsg          = msg.substr(pos + 4);
            auto contextMsg = msg.substr(1, pos - 1);

            if(std::count(contextMsg.begin(), contextMsg.end(), ',') <= 3) {
                return;
            }

            auto fileNameSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(fileNameSv.size());

            if(!contextMsg.starts_with(", ")) {
                return;
            }
            contextMsg.remove_prefix(2);

            if(
              2 > fileNameSv.size() || !fileNameSv.starts_with("\"") || !fileNameSv.ends_with("\""))
            {
                return;
            }
            fileNameSv.remove_prefix(1);
            fileNameSv.remove_suffix(1);

            auto const lineSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(lineSv.size());
            if(!contextMsg.starts_with(", ")) {
                return;
            }
            contextMsg.remove_prefix(2);
            std::uint16_t line_;
            {
                auto const [ptr, ec] = std::from_chars(lineSv.begin(), lineSv.end(), line_);
                if(ec != std::errc{} || ptr != lineSv.end()) {
                    return;
                }
            }

            auto const logLevelSv = contextMsg.substr(0, contextMsg.find_first_of(','));
            contextMsg.remove_prefix(logLevelSv.size());
            if(!contextMsg.starts_with(", ")) {
                return;
            }
            contextMsg.remove_prefix(2);
            std::uint8_t logLevel_;
            {
                auto const [ptr, ec]
                  = std::from_chars(logLevelSv.begin(), logLevelSv.end(), logLevel_);
                if(ec != std::errc{} || ptr != logLevelSv.end()) {
                    return;
                }
            }

            auto const timeSv  = contextMsg.substr(0, contextMsg.find_first_of(','));
            auto       oUcTime = parseTimeString(timeSv);
            if(!oUcTime) {
                return;
            }

            contextMsg.remove_prefix(timeSv.size());
            if(!contextMsg.starts_with(", \"\"\"")) {
                return;
            }
            contextMsg.remove_prefix(5);
            auto const functionNameSv = contextMsg;

            ucTime       = *oUcTime;
            fileName     = fileNameSv;
            line         = line_;
            logLevel     = static_cast<uc_log::LogLevel>(logLevel_);
            functionName = functionNameSv;
        }
    };
}}   // namespace uc_log::detail

template<>
struct fmt::formatter<uc_log::LogLevel> {
    bool unformated{false};

    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        if(ctx.begin() != ctx.end() && *ctx.begin() == '#') {
            unformated = true;
            return ctx.begin() + 1;
        }
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(uc_log::LogLevel const& l, FormatContext& ctx) const {
        constexpr std::array<std::pair<fmt::text_style, std::string_view>, 6> LCS{
          {{fmt::fg(fmt::terminal_color::yellow), "trace"},
           {fmt::fg(fmt::terminal_color::green), "debug"},
           {fmt::fg(fmt::terminal_color::bright_blue), "info"},
           {fmt::fg(fmt::terminal_color::magenta), "warn"},
           {fmt::fg(fmt::terminal_color::red), "error"},
           {fmt::bg(fmt::terminal_color::bright_red) | fmt::fg(fmt::terminal_color::white),
           "crit"}}
        };

        constexpr std::size_t MaxLength
          = std::max_element(LCS.begin(), LCS.end(), [](auto rhs, auto lhs) {
                return rhs.second.size() < lhs.second.size();
            })->second.size();

        auto index = static_cast<std::size_t>(l);

        if(index >= LCS.size()) {
            index = 0;
        }

        if(unformated) {
            return fmt::format_to(ctx.out(), "{}", LCS[index].second);
        } else {
            return fmt::format_to(
              ctx.out(),
              "{:{}}",
              fmt::styled(LCS[index].second, LCS[index].first),
              MaxLength);
        }
    }
};

template<>
struct fmt::formatter<uc_log::detail::LogEntry::Channel> {
    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(uc_log::detail::LogEntry::Channel const& c, FormatContext& ctx) const {
        constexpr std::array<fmt::text_style, 6> Colors{
          {fmt::bg(fmt::terminal_color::green) | fmt::fg(fmt::terminal_color::black),
           fmt::fg(fmt::terminal_color::red),
           fmt::fg(fmt::terminal_color::bright_blue),
           fmt::fg(fmt::terminal_color::magenta),
           fmt::fg(fmt::terminal_color::cyan),
           fmt::fg(fmt::terminal_color::yellow)}
        };

        return fmt::format_to(
          ctx.out(),
          "{}",
          fmt::styled(c.channel, Colors[c.channel % Colors.size()]));
    }
};

template<>
struct fmt::formatter<uc_log::detail::LogEntry::UcTime> {
    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(uc_log::detail::LogEntry::UcTime const& t, FormatContext& ctx) const {
        auto const days  = std::chrono::duration_cast<std::chrono::days>(t.time);
        auto const hours = std::chrono::duration_cast<std::chrono::hours>(t.time - days);
        auto const minutes
          = std::chrono::duration_cast<std::chrono::minutes>(t.time - (days + hours));
        auto const seconds
          = std::chrono::duration_cast<std::chrono::seconds>(t.time - (days + hours + minutes));
        auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
          t.time - (days + hours + minutes + seconds));
        auto const microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
          t.time - (days + hours + minutes + seconds + milliseconds));
        auto const nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
          t.time - (days + hours + minutes + seconds + milliseconds + microseconds));

        if(days != std::chrono::days{}) {
            fmt::format_to(ctx.out(), "{:%Q} ", days);
        }

        return fmt::format_to(
          ctx.out(),
          "{:0>2%Q}:{:0>2%Q}:{:0>2%Q}.{:0>3%Q}.{:0>3%Q}.{:0>3%Q}",
          hours,
          minutes,
          seconds,
          milliseconds,
          microseconds,
          nanoseconds);
    }
};

static inline std::size_t stringSizeWithoutColor(std::string_view s) {
    std::size_t const size = s.size();
    std::size_t       escapeSize{};
    auto              pos = s.find('\033');
    while(pos != std::string_view::npos) {
        s.remove_prefix(pos);
        auto pos2 = s.find('m');
        if(pos2 == std::string::npos) {
            break;
        }
        s.remove_prefix(pos2);
        escapeSize += pos2 + 1;
        pos = s.find('\033');
    }

    return size - escapeSize;
}

template<>
struct fmt::formatter<uc_log::detail::LogEntry> {
    std::size_t width{};
    bool        alternate{false};

    template<typename ParseContext>
    constexpr auto parse(ParseContext& ctx) {
        auto it = ctx.begin();
        if(it == ctx.end() || *it == '}') {
            return ctx.begin();
        }
        if(*it != '<') {
            return ctx.begin();
        }
        ++it;
        auto const result = std::from_chars(it, ctx.end(), width);
        if(result.ec != std::errc{}) {
            return ctx.begin();
        }
        it = result.ptr;
        if(*it != '}') {
            if(*it == '#') {
                ++it;
                alternate = true;
            } else {
                ctx.begin();
            }
        }
        return it;
    }

    template<typename FormatContext>
    auto format(uc_log::detail::LogEntry const& entry, FormatContext& ctx) const {
        std::string const prefix{[&]() {
            auto out      = fmt::memory_buffer();
            auto appender = fmt::appender(out);
            fmt::format_to(appender, "{}", entry.channel);
            fmt::format_to(
              appender,
              "{}",
              fmt::styled(entry.ucTime, fmt::fg(fmt::terminal_color::bright_magenta)));
            fmt::format_to(appender, " {}: ", entry.logLevel);
            return fmt::to_string(out);
        }()};

        std::string const postfix{[&]() {
            auto out      = fmt::memory_buffer();
            auto appender = fmt::appender(out);
            if(alternate) {
                fmt::format_to(
                  appender,
                  " {}",
                  fmt::styled(entry.functionName, fmt::fg(fmt::terminal_color::bright_red)));
            }
            fmt::format_to(
              appender,
              fmt::fg(fmt::terminal_color::bright_blue),
              "{}({}:{})",
              alternate ? "" : " ",
              entry.fileName,
              entry.line);
            fmt::format_to(appender, "{}", entry.channel);
            return fmt::to_string(out);
        }()};
        auto const        log_message_size_diff
          = entry.logMsg.size() - stringSizeWithoutColor(entry.logMsg);
        auto const raw_msg_size = stringSizeWithoutColor(prefix) + stringSizeWithoutColor(postfix);
        auto const align_size
          = width > raw_msg_size ? (width - raw_msg_size) + log_message_size_diff : 0;

        return fmt::format_to(ctx.out(), "{}{:<{}}{}", prefix, entry.logMsg, align_size, postfix);
    }
};
