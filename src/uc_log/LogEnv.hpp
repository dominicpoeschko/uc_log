#pragma once

#include "LogLevel.hpp"
#include "Tag.hpp"
#include "detail/Signature.hpp"
#include "string_constant/string_constant.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

// Scoped configuration of the UC_LOG macros: each call names `uc_log_env_t` unqualified and gets
// the innermost one.
//
//   namespace kvasir::i2c { UC_LOG_SCOPE_MIN_LEVEL(warn); }
//   struct Motor { UC_LOG_SCOPE_BACKEND(MotorTag); ... };
//   UC_LOG_WITH_ENV(::uc_log::setting::MinLevel<::uc_log::LogLevel::info>) { UC_LOG_I("..."); }
//   namespace kvasir::usb { UC_LOG_SCOPE_MODULE("usb"); }
//
// Never below the global UC_LOG_MIN_LEVEL. One UC_LOG_ENV per scope (across headers too), not at
// global scope. Without UC_LOG_SCOPE_MODULE the printer derives the module from the signature.

namespace uc_log {
template<typename... Settings>
struct Env {};

namespace setting {
    template<LogLevel Level>
    struct MinLevel {};

    template<typename Tag>
    struct Backend {};

    template<typename Name>
    struct Module {};
}   // namespace setting

namespace detail {
    template<typename>
    inline constexpr bool isEnvSetting = false;
    template<LogLevel Level>
    inline constexpr bool isEnvSetting<setting::MinLevel<Level>> = true;
    template<typename Tag>
    inline constexpr bool isEnvSetting<setting::Backend<Tag>> = true;
    template<char... chars>
    inline constexpr bool isEnvSetting<setting::Module<sc::StringConstant<chars...>>> = [] {
        static_assert(validModuleName(sc::StringConstant<chars...>::stringView),
                      "a log module name is 1-63 characters of a-z 0-9 _ . : / -");
        return true;
    }();
}   // namespace detail

template<typename E, typename... Settings>
struct ExtendEnv;

template<typename... Current, typename... Settings>
struct ExtendEnv<Env<Current...>, Settings...> {
    static_assert((detail::isEnvSetting<Settings> && ...),
                  "UC_LOG_ENV takes uc_log::setting::MinLevel<...>, uc_log::setting::Backend<...> "
                  "and uc_log::setting::Module<...>");
    using type = Env<Current..., Settings...>;
};

template<typename E, typename... Settings>
using ExtendEnv_t = typename ExtendEnv<E, Settings...>::type;

namespace detail {
    template<typename Setting>
    struct LevelOf {
        static constexpr LogLevel apply(LogLevel current) { return current; }
    };

    template<LogLevel Level>
    struct LevelOf<setting::MinLevel<Level>> {
        static constexpr LogLevel apply(LogLevel) { return Level; }
    };

    template<typename... Settings>
    consteval LogLevel scopeMinLevel(Env<Settings...>) {
        LogLevel level = LogLevel::trace;
        ((level = LevelOf<Settings>::apply(level)), ...);
        return level;
    }

    template<typename Current, typename Setting>
    struct TagOf {
        using type = Current;
    };

    template<typename Current, typename Tag>
    struct TagOf<Current, setting::Backend<Tag>> {
        using type = Tag;
    };

    template<typename Current, typename... Settings>
    struct LastTag {
        using type = Current;
    };

    template<typename Current, typename First, typename... Rest>
    struct LastTag<Current, First, Rest...>
      : LastTag<typename TagOf<Current, First>::type, Rest...> {};

    template<typename E>
    struct EnvTag;

    template<typename... Settings>
    struct EnvTag<Env<Settings...>> : LastTag<Tag::User, Settings...> {};

    template<typename E>
    using EnvTag_t = typename EnvTag<E>::type;

    struct NoModule {};

    template<typename Current, typename Setting>
    struct ModuleOf {
        using type = Current;
    };

    template<typename Current, typename Name>
    struct ModuleOf<Current, setting::Module<Name>> {
        using type = Name;
    };

    template<typename Current, typename... Settings>
    struct LastModule {
        using type = Current;
    };

    template<typename Current, typename First, typename... Rest>
    struct LastModule<Current, First, Rest...>
      : LastModule<typename ModuleOf<Current, First>::type, Rest...> {};

    template<typename E>
    struct EnvModule;

    template<typename... Settings>
    struct EnvModule<Env<Settings...>> : LastModule<NoModule, Settings...> {};

    template<typename E>
    using EnvModule_t = typename EnvModule<E>::type;

    // Only an explicit module goes into the header; the printer derives the rest.
    template<typename E>
    consteval auto moduleFieldFor() {
        using Name = EnvModule_t<E>;
        if constexpr(!std::is_same_v<Name, NoModule>) {
            return sc::StringConstant<'"'>{} + Name{} + sc::StringConstant<'"', ',', ' '>{};
        } else {
            return sc::StringConstant<>{};
        }
    }

    // No function name here: the call-site tag names it (UC_LOG_IMPL), which costs no
    // __PRETTY_FUNCTION__ read (Signature.hpp scanSignature).
    template<typename E>
    consteval auto headerTail() {
        return moduleFieldFor<E>() + sc::StringConstant<'"', '"', '"'>{};
    }

}   // namespace detail
}   // namespace uc_log

using uc_log_env_t = ::uc_log::Env<>;

// The lambda keeps a class-scope redeclaration from "changing the meaning" of uc_log_env_t.
#define UC_LOG_DETAIL_ENV_DECL(...)                                \
    [[maybe_unused]] typedef decltype([] {                         \
        return ::uc_log::ExtendEnv_t<uc_log_env_t, __VA_ARGS__>{}; \
    }()) uc_log_env_t

// Adds settings (uc_log::setting::...) to the current scope's uc_log_env_t. Takes a `;`.
#define UC_LOG_ENV(...)                                                           \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") \
      UC_LOG_DETAIL_ENV_DECL(__VA_ARGS__);                                        \
    _Pragma("GCC diagnostic pop") static_assert(true)

// UC_LOG_SCOPE_MIN_LEVEL(warn);
#define UC_LOG_SCOPE_MIN_LEVEL(level)                                  \
    UC_LOG_ENV(::uc_log::setting::MinLevel<::uc_log::LogLevel::level>)

// ComBackend<Tag> and LogClock<Tag>.
#define UC_LOG_SCOPE_BACKEND(...) UC_LOG_ENV(::uc_log::setting::Backend<__VA_ARGS__>)

// UC_LOG_SCOPE_MODULE("usb");
#define UC_LOG_MODULE(name)       ::uc_log::setting::Module<decltype(SC_LIFT(name))>
#define UC_LOG_SCOPE_MODULE(name) UC_LOG_ENV(UC_LOG_MODULE(name))

// Settings for the following statement. The body is the else branch so a caller's `else` cannot
// bind to the macro's if.
#define UC_LOG_WITH_ENV(...)                                                                   \
    _Pragma("GCC diagnostic push") _Pragma(                                                    \
      "GCC diagnostic ignored \"-Wshadow\"") if constexpr(UC_LOG_DETAIL_ENV_DECL(__VA_ARGS__); \
                                                          false) {}                            \
    else _Pragma("GCC diagnostic pop")
