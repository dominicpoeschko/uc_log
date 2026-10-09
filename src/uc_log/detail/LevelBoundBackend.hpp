#pragma once

#include "../ComBackend.hpp"
#include "../LogLevel.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace uc_log::detail {

// The backend's record guard, forwarded for uc_log::detail::Log, which sees the backend only
// through LevelBoundBackend; a backend without one gets an empty guard.
template<typename Backend>
struct RecordGuardOf {
    struct RecordGuard {};
};

template<typename Backend>
    requires requires { typename Backend::RecordGuard; }
struct RecordGuardOf<Backend> {
    using RecordGuard = typename Backend::RecordGuard;
};

template<typename Backend, LogLevel Level>
struct LevelBoundBackend : RecordGuardOf<Backend> {
    static void write(std::span<std::byte const> span) {
        if constexpr(requires { Backend::template write<Level>(span); }) {
            Backend::template write<Level>(span);
        } else {
            Backend::write(span);
        }
    }

    // remote_fmt::Printer brackets one entry with these; forwarded so a backend that
    // assembles whole entries (a queue with a drop-whole-entry policy) can tell where
    // one starts and ends. Optional on the backend, level-templated when it wants it.
    static void initTransfer() {
        if constexpr(requires { Backend::template initTransfer<Level>(); }) {
            Backend::template initTransfer<Level>();
        } else if constexpr(requires { Backend::initTransfer(); }) {
            Backend::initTransfer();
        }
    }

    static void finalizeTransfer() {
        if constexpr(requires { Backend::template finalizeTransfer<Level>(); }) {
            Backend::template finalizeTransfer<Level>();
        } else if constexpr(requires { Backend::finalizeTransfer(); }) {
            Backend::finalizeTransfer();
        }
    }
};

template<typename Backend, LogLevel Level>
concept TakesLevel
  = requires(std::span<std::byte const> span) { Backend::template write<Level>(span); }
 || requires { Backend::template initTransfer<Level>(); }
 || requires { Backend::template finalizeTransfer<Level>(); };

// A backend that ignores the level gets one instantiation for all levels. Every level is asked:
// a backend may constrain its write<L> (requires L >= warn).
template<typename Backend,
         std::size_t... Ls>
consteval bool takesAnyLevel(std::index_sequence<Ls...>) {
    return (TakesLevel<Backend, static_cast<LogLevel>(Ls)> || ...);
}

template<typename Backend>
concept LevelAware = takesAnyLevel<Backend>(
  std::make_index_sequence<static_cast<std::size_t>(LogLevel::crit) + 1>{});

// A backend may declare `static constexpr LogLevel kLevelFloor`: every level from there up is
// handled alike (a backend that only compares the level against a minimum). Those levels then
// share one LevelBoundBackend -- and with it one Printer, record frame and formatter set,
// instead of a copy per level. The record's level is not the backend's: it travels in the
// format string, so nothing on the wire changes.
template<typename Backend,
         LogLevel Level>
consteval LogLevel canonicalLevel() {
    if constexpr(requires { Backend::kLevelFloor; }) {
        return Level >= Backend::kLevelFloor ? Backend::kLevelFloor : Level;
    } else {
        return Level;
    }
}

template<typename Tag, LogLevel Level>
using ResolveBackend
  = LevelBoundBackend<ComBackend<Tag>,
                      LevelAware<ComBackend<Tag>> ? canonicalLevel<ComBackend<Tag>, Level>()
                                                  : LogLevel::info>;

}   // namespace uc_log::detail
