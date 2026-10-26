// The image's config header comes first: UC_LOG_CONFIG_HEADER (a quoted path, set on the image target by its build)
// names the one header that specialises ComBackend<Tag::User> and LogClock<Tag::User>, complete. Every TU that logs
// then sees them before any log line, whatever it included before. Without the macro nothing is read: the TU (or a
// header before it) specialises them itself.
//
// The config header must not reach this header, directly or through another one: the check sits before the include
// guard, so a re-entry is this error instead of a skipped file and "undeclared identifier UC_LOG_I" later.
#ifdef UC_LOG_DETAIL_READING_CONFIG
    #error \
      "the config header (UC_LOG_CONFIG_HEADER) includes uc_log/uc_log.hpp, directly or through another header: it may only include the backend, clock and LogClock.hpp headers"
#endif
#ifndef UC_LOG_UC_LOG_HPP
    #define UC_LOG_UC_LOG_HPP

    #ifdef UC_LOG_CONFIG_HEADER
        #define UC_LOG_DETAIL_READING_CONFIG
        #include UC_LOG_CONFIG_HEADER
        #undef UC_LOG_DETAIL_READING_CONFIG
    #endif

    #include "ComBackend.hpp"
    #include "LogClock.hpp"
    #include "LogEnv.hpp"
    #include "LogFilter.hpp"
    #include "LogLevel.hpp"
    #include "Tag.hpp"
    #include "detail/LevelBoundBackend.hpp"
    #include "detail/Lifetimebound.hpp"
    #include "detail/LostRecord.hpp"
    #include "metric.hpp"
    #include "string_constant/string_constant.hpp"

    // A variant without USE_UC_LOG parses only the macro front end above: levels, environments, the filter, metric() and
    // string_constant. No remote_fmt, fmt, glaze, enchantum or mp-units reaches it from here.
    #ifdef USE_UC_LOG
        #include "remote_fmt/descriptor.hpp"
        #include "remote_fmt/remote_fmt.hpp"

        // UC_LOG_USE_AGLIO (0/1): whether reflectable structs (aglio/glaze) are formattable. One value per image, set by the
        // build (kvasir_executable_variants(... LOG_REFLECTED_STRUCTS ON|OFF)), never per TU: the aglio formatter is a
        // specialization, so TUs that disagree are an ODR violation. Off, logging such a struct is "implicit
        // instantiation of undefined template remote_fmt::formatter<S>".
        #ifndef UC_LOG_USE_AGLIO
            #define UC_LOG_USE_AGLIO 1
        #endif
        #if UC_LOG_USE_AGLIO
            #include "aglio/remote_fmt.hpp"
        #endif
    #endif

    #include <cstddef>
    #include <cstdint>
    #include <string_view>
    #include <tuple>
    #include <type_traits>
    #include <utility>

namespace uc_log { namespace detail {
    // The no-log build's stand-in for a log call, named in the dead arm of a constant-false
    // conditional: nothing is evaluated or emitted, but the arguments count as used, so no
    // unused warnings. Const references so bit-fields bind; returns int so the arms agree.
    template<typename... Ts>
    constexpr int touch(Ts const&...) noexcept {
        return 0;
    }
}}   // namespace uc_log::detail

    #ifdef USE_UC_LOG
namespace uc_log { namespace detail {
    // __FILE_NAME__, the file's name without its directory
    struct FileName {
    private:
        std::string_view sv;

    public:
        template<std::size_t N>
        consteval FileName(char const (&s UC_LOG_LIFETIMEBOUND)[N]) : sv{std::string_view{s}} {}

        constexpr operator std::string_view() const { return sv; }
    };

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

    // Second phase of UC_LOG_AT's two-phase call; logArgumentTypes is the first. The split
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

        // No stack protector: the function has no buffer of its own, and
        // -fstack-protector-strong would guard it only because the clock hands its time back
        // through a local - a second canary check in every log line.
        [[gnu::noinline,
          gnu::no_stack_protector]] explicit RecordFrame(remote_fmt::catalog_id site)
          : time{ticks(LogClock<ClockTag>::now())} {
            open();
            printer.beginCataloged(remote_fmt::SiteId{site});
            printer.formatArguments(time);
        }

        // Without the catalog (host builds) the record starts with its described string instead of the site's id;
        // the rest of the frame is the same.
        template<char... Chars>
        [[gnu::no_stack_protector]] explicit RecordFrame(sc::StringConstant<Chars...> described)
          : time{ticks(LogClock<ClockTag>::now())} {
            open();
            printer.beginInline(described);
            printer.formatArguments(time);
        }

        [[gnu::noinline]] ~RecordFrame() {
            if constexpr(remote_fmt::useCatalog) {
                printer.endCataloged();
            } else {
                printer.endInline();
            }
        }

        // What every record does before its start: the boot noted (RttEpoch.hpp), and with LossPolicy::Counted the
        // records this ring dropped told first, as one line.
        [[gnu::always_inline,
          gnu::no_stack_protector]] void
        open() {
            if constexpr(requires { ComBackend::noteRecord(); }) { ComBackend::noteRecord(); }
            if constexpr(requires { ComBackend::lossMark(); }) {
                if(auto const mark = ComBackend::lossMark(); mark.pending != 0) [[unlikely]] {
                    reportLost(mark);
                }
            }
        }

        // The lost-record line (detail/LostRecord.hpp), a warn line of module uc_log with this frame's printer and
        // time (when the target knew the loss had ended); its own drop is not counted (lossSettle). Defined below.
        template<typename Mark>
        [[gnu::noinline,
          gnu::cold]] void
        reportLost(Mark mark);

        RecordFrame(RecordFrame const&)            = delete;
        RecordFrame& operator=(RecordFrame const&) = delete;
    };

    // Ticks is the type of the time stamp every line starts with (ticks() of the clock's now()),
    // Args the line's own arguments.
    template<typename Ticks, typename... Args>
    struct Log<std::tuple<Ticks, Args...>> {
        // The line's string as its call site's tag names it: the format string with the descriptors of the time
        // stamp and the arguments (remote_fmt::described), so the catalog says how to read the record's values.
        template<typename Fmt>
        static consteval auto described(Fmt) {
            return remote_fmt::described<Ticks, Args...>(Fmt{});
        }

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
                 char... Chars>
        [[gnu::always_inline]] static constexpr void log(remote_fmt::catalog_id       site,
                                                         sc::StringConstant<Chars...> fmt,
                                                         LogArgument_t<Args>... args) {
            // with the catalog the string is needed for this check only, which makes no code
            remote_fmt::Printer<ComBackend>::
              template checkCataloged<Ticks const&, decltype(normalizeLogArgument(args))...>(fmt);
            if constexpr(remote_fmt::useCatalog) {
                record<ComBackend, ClockTag>(site, args...);
            } else {
                RecordFrame<ComBackend, ClockTag> frame{
                  remote_fmt::described<Ticks const&, decltype(normalizeLogArgument(args))...>(
                    fmt)};
                frame.printer.formatArguments(normalizeLogArgument(args)...);
            }
        }
    };

}}   // namespace uc_log::detail
    #endif

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

    #ifdef USE_UC_LOG
    // Time goes out as a tick count, the header says "<count>[num/den]s" (numbers by desc::number: no fmt parsing).
    template<typename Tag>
    using LogTimePoint = decltype(LogClock<Tag>::now());

    template<typename Tag>
    consteval auto timeSuffix() {
        using namespace ::sc::literals;
        using Period = typename LogTimePoint<Tag>::period;
        return "["_sc + ::remote_fmt::desc::number<static_cast<std::uint64_t>(Period::num)>()
             + "/"_sc + ::remote_fmt::desc::number<static_cast<std::uint64_t>(Period::den)>()
             + "]s"_sc;
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
    #endif
}   // namespace detail
}   // namespace uc_log

    #ifdef USE_UC_LOG
        // The compile-time header string of a line in a scope with the environment `env` and the backend
        // tag `tag` (EnvTag_t<env>); expects the sc literal namespaces in scope. Names the macros introduce
        // carry the UC_LOG_DO_NOT_USE_ prefix: they expand into the caller's scope (-Wshadow).
        #define UC_LOG_DETAIL_FMT(tag, env, level, line, filename, fmt)                            \
            ("(\""_sc + SC_LIFT(::uc_log::detail::FileName{filename}) + "\", "_sc                  \
             + ::remote_fmt::desc::number<static_cast<std::uint32_t>(line)>() + ", "_sc            \
             + ::remote_fmt::desc::number<static_cast<std::uint8_t>(level)>() + ", {}"_sc          \
             + ::uc_log::detail::timeSuffix<tag>() + ", "_sc + ::uc_log::detail::headerTail<env>() \
             + "\"\"\")"_sc + SC_LIFT(fmt))

        // Only the filter file's module rules need the signature.
        #ifdef UC_LOG_HAS_FILTER_FILE
            #define UC_LOG_DETAIL_FILTER_SIGNATURE                                     \
                std::string_view{__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 1}
        #else
            #define UC_LOG_DETAIL_FILTER_SIGNATURE \
                std::string_view {}
        #endif

        // A log line at a given source position (UC_LOG passes __LINE__, __FILE_NAME__; a wrapper its
        // caller's). The arguments appear twice: unevaluated to harvest the types, then as the real call;
        // nothing is evaluated twice. uc_log_env_t is unqualified on purpose (LogEnv.hpp).
        #define UC_LOG_AT(level, line, filename, fmt, ...)                                         \
            do {                                                                                   \
                using UC_LOG_DO_NOT_USE_TAG = ::uc_log::detail::EnvTag_t<uc_log_env_t>;            \
                if constexpr(::uc_log::detail::lineEnabled<static_cast<::uc_log::LogLevel>(level), \
                                                           uc_log_env_t>(                          \
                               UC_LOG_DETAIL_FILTER_SIGNATURE))                                    \
                {                                                                                  \
                    if(!std::is_constant_evaluated()) {                                            \
                        using namespace ::remote_fmt::detail;                                      \
                        using namespace ::sc::literals;                                            \
                        using UC_LOG_DO_NOT_USE_LOG                                                \
                          = ::uc_log::detail::Log<decltype(::uc_log::detail::logArgumentTypes(     \
                            ::uc_log::detail::LogArgumentsPreferred{},                             \
                            ::uc_log::detail::ticks(                                               \
                              ::uc_log::LogClock<UC_LOG_DO_NOT_USE_TAG>::now()) __VA_OPT__(, )     \
                              __VA_ARGS__))>;                                                      \
                        constexpr typename UC_LOG_DO_NOT_USE_LOG::template Final<                  \
                          decltype(UC_LOG_DETAIL_FMT(UC_LOG_DO_NOT_USE_TAG,                        \
                                                     uc_log_env_t,                                 \
                                                     level,                                        \
                                                     line,                                         \
                                                     filename,                                     \
                                                     fmt))>                                        \
                          UC_LOG_DO_NOT_USE_FMT{};                                                 \
                        UC_LOG_DO_NOT_USE_LOG::template log<                                       \
                          ::uc_log::ResolveBackend<UC_LOG_DO_NOT_USE_TAG,                          \
                                                   static_cast<::uc_log::LogLevel>(level)>,        \
                          UC_LOG_DO_NOT_USE_TAG>(                                                  \
                          REMOTE_FMT_SITE()(                                                       \
                            UC_LOG_DO_NOT_USE_LOG::described(UC_LOG_DO_NOT_USE_FMT)),              \
                          UC_LOG_DO_NOT_USE_FMT __VA_OPT__(, ) __VA_ARGS__);                       \
                    }                                                                              \
                }                                                                                  \
            } while(false)

template<typename ComBackend,
         typename ClockTag>
template<typename Mark>
void uc_log::detail::RecordFrame<ComBackend,
                                 ClockTag>::reportLost(Mark const mark) {
    if constexpr(globalFloor() <= static_cast<std::uint8_t>(LogLevel::warn)) {
        using namespace ::remote_fmt::detail;
        using namespace ::sc::literals;
        using LostEnv
          = ExtendEnv_t<Env<>, setting::Backend<ClockTag>, setting::Module<decltype("uc_log"_sc)>>;
        namespace lost = ::uc_log::detail::
          lost_record;   // the text, shared with the printer: detail/LostRecord.hpp
        using Text = decltype(::sc::create([] { return lost::sign(remote_fmt::useCatalog); })
                              + " {} "_sc + ::sc::create([] { return lost::phrase; }) + " "_sc
                              + ::sc::create([] { return lost::cause; }));
        constexpr auto fmt
          = UC_LOG_DETAIL_FMT(ClockTag, LostEnv, LogLevel::warn, __LINE__, __FILE_NAME__, Text{});
        remote_fmt::Printer<ComBackend>::template checkCataloged<Ticks const&,
                                                                 std::uint32_t const&>(fmt);
        constexpr auto described = remote_fmt::described<Ticks, decltype(mark.pending)>(fmt);
        if constexpr(remote_fmt::useCatalog) {
            printer.beginCataloged(remote_fmt::SiteId{REMOTE_FMT_SITE()(described)});
        } else {
            printer.beginInline(described);
        }
        printer.formatArguments(time, mark.pending);
        if constexpr(remote_fmt::useCatalog) {
            printer.endCataloged();
        } else {
            printer.endInline();
        }
    }
    ComBackend::lossSettle(mark);
}
    #endif

    #ifndef USE_UC_LOG
        // Same arguments and literal scope as the real call, never evaluated (detail::touch).
        #define UC_LOG_AT(level, line, filename, fmt, ...)                                \
            do {                                                                          \
                using namespace ::sc::literals;                                           \
                static_cast<void>(                                                        \
                  false ? ::uc_log::detail::touch(static_cast<::uc_log::LogLevel>(level), \
                                                  fmt __VA_OPT__(, ) __VA_ARGS__)         \
                        : 0);                                                             \
            } while(false)
    #endif

    #define UC_LOG(level, fmt, ...)                                               \
        UC_LOG_AT(level, __LINE__, __FILE_NAME__, fmt __VA_OPT__(, ) __VA_ARGS__)

    #define UC_LOG_T(...) UC_LOG(::uc_log::LogLevel::trace, __VA_ARGS__)
    #define UC_LOG_D(...) UC_LOG(::uc_log::LogLevel::debug, __VA_ARGS__)
    #define UC_LOG_I(...) UC_LOG(::uc_log::LogLevel::info, __VA_ARGS__)
    #define UC_LOG_W(...) UC_LOG(::uc_log::LogLevel::warn, __VA_ARGS__)
    #define UC_LOG_E(...) UC_LOG(::uc_log::LogLevel::error, __VA_ARGS__)
    #define UC_LOG_C(...) UC_LOG(::uc_log::LogLevel::crit, __VA_ARGS__)

#endif   // UC_LOG_UC_LOG_HPP
