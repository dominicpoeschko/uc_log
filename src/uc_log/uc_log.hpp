#pragma once

#include "ComBackend.hpp"
#include "LogClock.hpp"
#include "LogLevel.hpp"
#include "Tag.hpp"
#include "detail/LevelBoundBackend.hpp"
#include "metric.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "rtt/rtt.hpp"

// Formats reflectable structs. Not left to the user: it is a formatter specialization, so including it after a log
// call site that uses the type would be an ODR violation.
#include "aglio/remote_fmt.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace uc_log { namespace detail {
    struct FileName {
    private:
        std::string_view sv;

        consteval auto basename(std::string_view f) {
            auto const pos = f.find('/');
            if(pos == std::string_view::npos) { return f; }
            return f.substr(pos + 1);
        }

    public:
        template<std::convertible_to<std::string_view> S>
        consteval FileName(S const& s) : sv{basename(std::string_view{s})} {}

        constexpr operator std::string_view() const { return sv; }
    };

    // The no-log build's stand-in for a log call, named in the dead arm of a constant-false
    // conditional: nothing is evaluated or emitted, but the arguments count as used, so no
    // unused warnings. Const references so bit-fields bind; returns int so the arms agree.
    template<typename... Ts>
    constexpr int touch(Ts const&...) noexcept {
        return 0;
    }

    // Validates every log argument. Arrays pass (char[N] keeps formatter<char[N]>'s N-1
    // semantics, other arrays log as ranges). Of pointers only void* passes, logged as an
    // address; char pointers are rejected because their length would need strlen, which a
    // missing nul makes undefined - std::string_view is the caller stating that contract.
    template<typename T>
    constexpr T const& normalizeLogArgument(T const& value) {
        static_assert(!std::is_same_v<T, char*> && !std::is_same_v<T, char const*>,
                      "char pointers cannot be logged: the length is not safely knowable from "
                      "a pointer - log the literal or array itself, or state the contract by "
                      "wrapping it in std::string_view");
        static_assert(!std::is_pointer_v<T> || std::is_same_v<T, char*>
                        || std::is_same_v<T, char const*> || std::is_same_v<T, void*>
                        || std::is_same_v<T, void const*>,
                      "typed pointers cannot be logged: log the pointee, cast to void* to "
                      "log the address, or wrap a buffer in std::span or std::string_view");
        return value;
    }

    // Only used inside decltype: yields the argument types without evaluating or binding
    // anything, so packed-struct members are fine here. The preferred overload keeps array
    // types intact (no decay); calls containing a volatile lvalue, which const& cannot bind,
    // fall back via the tag to by-value deduction, which strips the volatile. The constraint
    // keeps deduction from absorbing a volatile into Args instead of falling back.
    struct LogArgumentsFallback {};

    struct LogArgumentsPreferred : LogArgumentsFallback {};

    template<typename... Args>
        requires(!(std::is_volatile_v<Args> || ...))
    auto logArgumentTypes(LogArgumentsPreferred,
                          Args const&...) -> std::tuple<Args...>;

    template<typename... Args>
    auto logArgumentTypes(LogArgumentsFallback,
                          Args...) -> std::tuple<Args...>;

    // Per-argument parameter type for Log::log. Trivially copyable types go by value: that
    // covers everything a packed struct can contain, and the load at the call site is
    // alignment-correct. A reference would bind to a possibly misaligned address - undefined
    // behaviour that GCC rejects but clang silently accepts, with no compile-time detection
    // possible. Everything else (vectors, maps, strings - never packed) goes by reference,
    // copy-free. Arrays go by reference too, since C++ cannot pass them by value; the one
    // construct this cannot make safe is an array member of a packed struct on clang (GCC
    // materializes an aligned temporary, -fsanitize=alignment reports it at runtime).
    template<typename T>
    using LogArgument_t
      = std::conditional_t<std::is_array_v<T>,
                           T const&,
                           std::conditional_t<std::is_trivially_copyable_v<T>, T, T const&>>;

    // Second phase of UC_LOG_IMPL's two-phase call; logArgumentTypes is the first. The split
    // keeps __VA_ARGS__ inside ordinary call parentheses both times, so braced arguments with
    // commas (Point{1, 2}) survive the preprocessor.
    template<typename>
    struct Log;

    template<typename... Args>
    struct Log<std::tuple<Args...>> {
        template<typename ComBackend,
                 char... chars>
        static constexpr void log(sc::StringConstant<chars...> fmt,
                                  LogArgument_t<Args>... args) {
            // Held for the whole record, not each write() it is made of; empty unless the
            // backend names one (detail/LevelBoundBackend.hpp).
            [[maybe_unused]] typename ComBackend::RecordGuard const guard{};
            remote_fmt::Printer<ComBackend>::staticPrint(injectMetricFmtString(fmt, args...),
                                                         normalizeLogArgument(args)...);
        }
    };

}}   // namespace uc_log::detail

// Compile-time level floor: call sites below it expand to nothing at all -- no code,
// no format string in the catalog. Set by LogLevel enumerator name so the build system
// needs no copy of the numbering (-DUC_LOG_MIN_LEVEL=warn keeps warn/error/crit); an
// unknown name fails on the line below. The discarded branch is still semantically
// checked, so an unloggable argument is an error at every level.
#ifndef UC_LOG_MIN_LEVEL
    #define UC_LOG_MIN_LEVEL trace
#endif

namespace uc_log {
    inline constexpr LogLevel minLevel = LogLevel::UC_LOG_MIN_LEVEL; }   // namespace uc_log

#ifdef USE_UC_LOG
    // Shared assembly of the compile-time header string; expects
    // UC_LOG_DO_NOT_USE_FUNCTION_NAME and the sc literal namespaces in scope.
    //
    // Every name this macro introduces carries the UC_LOG_DO_NOT_USE_ prefix, because a macro
    // expands into the caller's scope and anything shorter will eventually shadow one of their
    // locals. The lambda parameters below were `c`, which shadowed a perfectly reasonable
    // `auto& c` at a call site and produced -Wshadow warnings pointing into this header
    // rather than at anything the caller could see was wrong.
    #define UC_LOG_DETAIL_FMT(level, line, filename, fmt)                              \
        "(\""_sc + SC_LIFT(::uc_log::detail::FileName{filename}) + "\", "_sc           \
          + ::sc::detail::format<static_cast<std::uint32_t>(line),                     \
                                 static_cast<std::uint8_t>(level)>("{}, {}"_sc)        \
          + ", {}, \"\"\""_sc                                                          \
          + ::sc::escape(                                                              \
            SC_LIFT(UC_LOG_DO_NOT_USE_FUNCTION_NAME),                                  \
            [](auto UC_LOG_DO_NOT_USE_CHAR) {                                          \
                return UC_LOG_DO_NOT_USE_CHAR == '{' || UC_LOG_DO_NOT_USE_CHAR == '}'; \
            },                                                                         \
            [](auto UC_LOG_DO_NOT_USE_CHAR) { return UC_LOG_DO_NOT_USE_CHAR; })        \
          + "\"\"\")"_sc + SC_LIFT(fmt)

    // The argument list appears twice: unevaluated to harvest the types, then as the real
    // call; nothing is evaluated twice.
    #define UC_LOG_IMPL(level, line, filename, fmt, ...)                                           \
        do {                                                                                       \
            if constexpr(static_cast<::uc_log::LogLevel>(level) >= ::uc_log::minLevel) {           \
                if(!std::is_constant_evaluated()) {                                                \
                    constexpr auto UC_LOG_DO_NOT_USE_FUNCTION_NAME = __FUNCTION__;                 \
                    using namespace ::remote_fmt::detail;                                          \
                    using namespace ::sc::literals;                                                \
                    ::uc_log::detail::Log<decltype(::uc_log::detail::logArgumentTypes(             \
                      ::uc_log::detail::LogArgumentsPreferred{},                                   \
                      ::uc_log::LogClock<::uc_log::Tag::User>::now() __VA_OPT__(, )                \
                        __VA_ARGS__))>::                                                           \
                      template log<                                                                \
                        ::uc_log::detail::ResolveBackend<::uc_log::Tag::User,                      \
                                                         static_cast<::uc_log::LogLevel>(level)>>( \
                        UC_LOG_DETAIL_FMT(level, line, filename, fmt),                             \
                        ::uc_log::LogClock<::uc_log::Tag::User>::now() __VA_OPT__(, )              \
                          __VA_ARGS__);                                                            \
                }                                                                                  \
            }                                                                                      \
        } while(false)
#else
    // Same arguments and literal scope as the real call, never evaluated (detail::touch).
    #define UC_LOG_IMPL(level, line, filename, fmt, ...)                                          \
        do {                                                                                      \
            using namespace ::sc::literals;                                                       \
            static_cast<void>(false                                                               \
                                ? ::uc_log::detail::touch(static_cast<::uc_log::LogLevel>(level), \
                                                          fmt __VA_OPT__(, ) __VA_ARGS__)         \
                                : 0);                                                             \
        } while(false)
#endif

#define UC_LOG(level, fmt, ...)                                                 \
    UC_LOG_IMPL(level, __LINE__, __FILE_NAME__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define UC_LOG_T(...) UC_LOG(::uc_log::LogLevel::trace, __VA_ARGS__)
#define UC_LOG_D(...) UC_LOG(::uc_log::LogLevel::debug, __VA_ARGS__)
#define UC_LOG_I(...) UC_LOG(::uc_log::LogLevel::info, __VA_ARGS__)
#define UC_LOG_W(...) UC_LOG(::uc_log::LogLevel::warn, __VA_ARGS__)
#define UC_LOG_E(...) UC_LOG(::uc_log::LogLevel::error, __VA_ARGS__)
#define UC_LOG_C(...) UC_LOG(::uc_log::LogLevel::crit, __VA_ARGS__)
