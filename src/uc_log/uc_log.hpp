#pragma once

#include "ComBackend.hpp"
#include "LogClock.hpp"
#include "LogEnv.hpp"
#include "LogFilter.hpp"
#include "LogLevel.hpp"
#include "Tag.hpp"
#include "detail/LevelBoundBackend.hpp"
#include "detail/Lifetimebound.hpp"
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

        consteval auto basename(std::string_view f UC_LOG_LIFETIMEBOUND) {
            auto const pos = f.find('/');
            if(pos == std::string_view::npos) { return f; }
            return f.substr(pos + 1);
        }

    public:
        template<std::size_t N>
        consteval FileName(char const (&s UC_LOG_LIFETIMEBOUND)[N])
          : sv{basename(std::string_view{s})} {}

        // No mark here: for a view (StartUp.hpp passes a std::string_view) the result points where
        // `s` points, not into `s` - clang 23 reports a mark on it as unverifiable.
        template<std::convertible_to<std::string_view> S>
            requires(!std::is_array_v<S>)
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
    constexpr T const& normalizeLogArgument(T const& value UC_LOG_LIFETIMEBOUND) {
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

    // now() is a time_point or a duration (defined below)
    template<typename Time>
    constexpr auto ticks(Time time);

    // What every cataloged record has around its arguments: the time stamp, the backend's record
    // guard, the printer with its staging bytes, the record's start with the site's id and its
    // end. Constructor and destructor are out of line, so this code exists ONCE per backend and
    // clock; a record body (Log::record) is then a frame, two calls and its own arguments.
    template<typename ComBackend, typename ClockTag>
    struct RecordFrame {
        using Ticks = decltype(ticks(LogClock<ClockTag>::now()));

        // In this order: the clock is read before the guard masks anything. The guard is held
        // for the whole record, not each write() it is made of; empty unless the backend names
        // one (detail/LevelBoundBackend.hpp).
        Ticks const                                                  time;
        [[no_unique_address]] typename ComBackend::RecordGuard const guard{};
        remote_fmt::Printer<ComBackend>                              printer;

        // No stack protector (dominic, 2026-10-05): the function has no buffer of its own, and
        // -fstack-protector-strong would guard it only because the clock hands its time back
        // through a local - a second canary check in every log line.
        [[gnu::noinline,
          gnu::no_stack_protector]] explicit RecordFrame(remote_fmt::catalog_id site)
          : time{ticks(LogClock<ClockTag>::now())} {
            printer.beginCataloged(remote_fmt::SiteId{site});
            printer.formatArguments(time);
        }

        [[gnu::noinline]] ~RecordFrame() { printer.endCataloged(); }

        RecordFrame(RecordFrame const&)            = delete;
        RecordFrame& operator=(RecordFrame const&) = delete;
    };

    // Ticks is the type of the time stamp every line starts with (ticks() of the clock's now()),
    // Args the line's own arguments.
    template<typename Ticks, typename... Args>
    struct Log<std::tuple<Ticks, Args...>> {
        // The format with the metrics' markers put in.
        template<typename Fmt>
        using Final = decltype(injectMetricFmtString(Fmt{},
                                                     std::declval<LogArgument_t<Ticks>>(),
                                                     std::declval<LogArgument_t<Args>>()...));

        // The record of a cataloged line: ONE body per argument list (and backend and clock),
        // shared by every call site with these argument types. Not inlined, and the site is a
        // run-time value: as a template argument, or inlined, each string had its own copy of
        // the whole record (84 bytes a string on the Cortex-M0+, more with arguments). The clock
        // is read in the frame for the same reason: a call site is its arguments, the id and
        // one call.
        template<typename ComBackend,
                 typename ClockTag>
        [[gnu::noinline]] static void record(remote_fmt::catalog_id site,
                                             LogArgument_t<Args>... args) {
            RecordFrame<ComBackend, ClockTag> frame{site};
            frame.printer.formatArguments(normalizeLogArgument(args)...);
        }

        // `site` is a value, not the site's lambda type: that would copy this function per
        // instantiation of the caller.
        template<typename ComBackend,
                 typename ClockTag,
                 char... chars>
        [[gnu::always_inline]] static constexpr void log(remote_fmt::catalog_id       site,
                                                         sc::StringConstant<chars...> fmt,
                                                         LogArgument_t<Args>... args) {
            static_assert(std::is_same_v<Ticks, decltype(ticks(LogClock<ClockTag>::now()))>);
            if constexpr(remote_fmt::use_catalog) {
                // the string is needed for this check only, which makes no code
                remote_fmt::Printer<ComBackend>::
                  template checkCataloged<Ticks const&, decltype(normalizeLogArgument(args))...>(
                    fmt);
                record<ComBackend, ClockTag>(site, args...);
            } else {
                Ticks const time = ticks(LogClock<ClockTag>::now());
                [[maybe_unused]] typename ComBackend::RecordGuard const guard{};
                remote_fmt::Printer<ComBackend>::staticPrint(fmt,
                                                             time,
                                                             normalizeLogArgument(args)...);
            }
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
inline constexpr LogLevel minLevel = LogLevel::UC_LOG_MIN_LEVEL;

namespace detail {
    // UC_LOG_MIN_LEVEL or the filter file's `*`, whichever is higher (LogFilter.hpp).
    consteval std::uint8_t globalFloor() {
        auto const level = static_cast<std::uint8_t>(minLevel);
        return filterTable.global > level ? filterTable.global : level;
    }

    // A module rule of the filter file replaces the scope's floor. `signature` is read only when
    // there are module rules (Signature.hpp scanSignature).
    template<LogLevel Level,
             typename Env>
    consteval bool lineEnabled(std::string_view signature) {
        auto floor = static_cast<std::uint8_t>(scopeMinLevel(Env{}));
        if constexpr(filterTable.hasModuleRules()) {
            using Name = EnvModule_t<Env>;
            int rule   = -1;
            if constexpr(!std::is_same_v<Name, NoModule>) {
                rule = filterTable.levelFor(Name::stringView);
            } else {
                auto const module = moduleOf(scanSignature(signature), signature);
                rule              = filterTable.levelFor(module.view());
            }
            if(rule >= 0) { floor = static_cast<std::uint8_t>(rule); }
        }
        auto const global = globalFloor();
        return static_cast<std::uint8_t>(Level) >= (floor > global ? floor : global);
    }

    // Time goes out as a tick count, the header says "<count>[num/den]s".
    template<typename Tag>
    using LogTimePoint = decltype(LogClock<Tag>::now());

    template<typename Tag>
    consteval auto timeSuffix() {
        using namespace ::sc::literals;
        using Period = typename LogTimePoint<Tag>::period;
        return ::sc::detail::format<static_cast<std::uint64_t>(Period::num),
                                    static_cast<std::uint64_t>(Period::den)>("[{}/{}]s"_sc);
    }

    // now() is a time_point or a duration
    template<typename Time>
    constexpr auto ticks(Time const time) {
        static_assert(std::is_integral_v<typename Time::rep>,
                      "LogClock needs an integral tick count");
        if constexpr(requires { time.time_since_epoch(); }) {
            return time.time_since_epoch().count();
        } else {
            return time.count();
        }
    }
}   // namespace detail
}   // namespace uc_log

#ifdef USE_UC_LOG
    // Shared assembly of the compile-time header string; expects the UC_LOG_DO_NOT_USE_ names of
    // UC_LOG_IMPL and the sc literal namespaces in scope. Names the macros introduce carry the
    // UC_LOG_DO_NOT_USE_ prefix: they expand into the caller's scope (-Wshadow).
    #define UC_LOG_DETAIL_FMT(level, line, filename, fmt)                                        \
        "(\""_sc + SC_LIFT(::uc_log::detail::FileName{filename}) + "\", "_sc                     \
          + ::sc::detail::format<static_cast<std::uint32_t>(line),                               \
                                 static_cast<std::uint8_t>(level)>("{}, {}"_sc)                  \
          + ", {}"_sc + ::uc_log::detail::timeSuffix<::uc_log::detail::EnvTag_t<uc_log_env_t>>() \
          + ", "_sc + ::uc_log::detail::headerTail<uc_log_env_t>() + "\"\"\")"_sc + SC_LIFT(fmt)

    // Only the filter file's module rules need the signature.
    #ifdef UC_LOG_HAS_FILTER_FILE
        #define UC_LOG_DETAIL_FILTER_SIGNATURE                                     \
            std::string_view{__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 1}
    #else
        #define UC_LOG_DETAIL_FILTER_SIGNATURE \
            std::string_view {}
    #endif

    // The argument list appears twice: unevaluated to harvest the types, then as the real
    // call; nothing is evaluated twice. uc_log_env_t is unqualified on purpose (LogEnv.hpp).
    #define UC_LOG_IMPL(level, line, filename, fmt, ...)                                         \
        do {                                                                                     \
            if constexpr(::uc_log::detail::lineEnabled<static_cast<::uc_log::LogLevel>(level),   \
                                                       uc_log_env_t>(                            \
                           UC_LOG_DETAIL_FILTER_SIGNATURE))                                      \
            {                                                                                    \
                if(!std::is_constant_evaluated()) {                                              \
                    using namespace ::remote_fmt::detail;                                        \
                    using namespace ::sc::literals;                                              \
                    using UC_LOG_DO_NOT_USE_LOG                                                  \
                      = ::uc_log::detail::Log<decltype(::uc_log::detail::logArgumentTypes(       \
                        ::uc_log::detail::LogArgumentsPreferred{},                               \
                        ::uc_log::detail::ticks(                                                 \
                          ::uc_log::LogClock<::uc_log::detail::EnvTag_t<uc_log_env_t>>::now())   \
                          __VA_OPT__(, ) __VA_ARGS__))>;                                         \
                    constexpr typename UC_LOG_DO_NOT_USE_LOG::template Final<                    \
                      decltype(UC_LOG_DETAIL_FMT(level, line, filename, fmt))>                   \
                      UC_LOG_DO_NOT_USE_FMT{};                                                   \
                    UC_LOG_DO_NOT_USE_LOG::template log<                                         \
                      ::uc_log::detail::ResolveBackend<::uc_log::detail::EnvTag_t<uc_log_env_t>, \
                                                       static_cast<::uc_log::LogLevel>(level)>,  \
                      ::uc_log::detail::EnvTag_t<uc_log_env_t>>(                                 \
                      REMOTE_FMT_SITE()(UC_LOG_DO_NOT_USE_FMT),                                  \
                      UC_LOG_DO_NOT_USE_FMT __VA_OPT__(, ) __VA_ARGS__);                         \
                }                                                                                \
            }                                                                                    \
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
