#pragma once

#include "ComBackend.hpp"
#include "LogClock.hpp"
#include "LogLevel.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "rtt/rtt.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>

namespace uc_log { namespace detail {
    struct FileName {
    private:
        std::string_view sv;
        consteval auto   basename(std::string_view f) {
            auto it = std::find(f.begin(), f.end(), '/');
            if(it == f.end()) {
                return f;
            }
            return std::string_view{++it, f.end()};
        }

    public:
        template<std::convertible_to<std::string_view> S>
        consteval FileName(S const& s) : sv{basename(std::string_view{s})} {}

        constexpr operator std::string_view() const { return sv; }
    };

    template<typename ComBackend, char... chars, typename... Args>
    constexpr void log(sc::StringConstant<chars...> fmt, Args&&... args) {
        remote_fmt::Printer<ComBackend>::staticPrint(fmt, std::forward<Args>(args)...);
    }

}}   // namespace uc_log::detail

#ifdef USE_UC_LOG
    #define UC_LOG_IMPL(level, line, filename, fmt, ...)                                          \
        do {                                                                                      \
            if(!std::is_constant_evaluated()) {                                                   \
                constexpr auto UC_LOG_DO_NOT_USE_FUNCTION_NAME = __PRETTY_FUNCTION__;             \
                using namespace ::remote_fmt::detail;                                             \
                using namespace ::sc::literals;                                                   \
                ::uc_log::detail::log<::uc_log::ComBackend<::uc_log::Tag::User>>(                 \
                  "(\""_sc + SC_LIFT(::uc_log::detail::FileName{filename}) + "\", "_sc            \
                    + ::sc::detail::                                                              \
                      format<static_cast<std::uint32_t>(line), static_cast<std::uint8_t>(level)>( \
                        "{}, {}"_sc)                                                              \
                    + ", {}, \"\"\""_sc                                                           \
                    + ::sc::escape(                                                               \
                      SC_LIFT(UC_LOG_DO_NOT_USE_FUNCTION_NAME),                                   \
                      [](auto c) { return c == '{' || c == '}'; },                                \
                      [](auto c) { return c; })                                                   \
                    + "\"\"\")"_sc + SC_LIFT(fmt),                                                \
                  ::uc_log::LogClock<::uc_log::Tag::User>::now() __VA_OPT__(, ) __VA_ARGS__);     \
            }                                                                                     \
        } while(false)
#else
    #define UC_LOG_IMPL(level, line, filename, fmt, ...) (void)0
#endif

#ifdef USE_UC_LOG
    #define UC_LOG(level, fmt, ...) \
        UC_LOG_IMPL(level, __LINE__, __FILE_NAME__, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
    #define UC_LOG(level, fmt, ...) (void)0
#endif

#define UC_LOG_T(...) UC_LOG(::uc_log::LogLevel::trace, __VA_ARGS__)
#define UC_LOG_D(...) UC_LOG(::uc_log::LogLevel::debug, __VA_ARGS__)
#define UC_LOG_I(...) UC_LOG(::uc_log::LogLevel::info, __VA_ARGS__)
#define UC_LOG_W(...) UC_LOG(::uc_log::LogLevel::warn, __VA_ARGS__)
#define UC_LOG_E(...) UC_LOG(::uc_log::LogLevel::error, __VA_ARGS__)
#define UC_LOG_C(...) UC_LOG(::uc_log::LogLevel::crit, __VA_ARGS__)
