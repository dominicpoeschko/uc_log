#pragma once

#include "../ComBackend.hpp"
#include "../LogLevel.hpp"
#include "../Sink.hpp"

#include <cstddef>
#include <span>

namespace uc_log::detail {

// The backend's record guard, forwarded for uc_log::detail::Log, which sees the backend only
// through LevelBoundBackend; a backend without one gets an empty guard.
template<typename Guard>
struct LevelBoundGuard {
    using RecordGuard = Guard;
};

template<>
struct LevelBoundGuard<void> {
    struct RecordGuard {};
};

template<typename Backend, LogLevel Level>
struct LevelBoundBackend : LevelBoundGuard<typename RecordGuardOf<Backend>::type> {
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

    // An RTT backend counts the boots that logged (detail/RttEpoch.hpp); RecordFrame calls this before each record.
    static void noteRecord()
        requires requires { Backend::noteRecord(); }
    {
        Backend::noteRecord();
    }

    // LossPolicy::Counted (LossPolicy.hpp): RecordFrame asks before a record whether its ring
    // dropped records nobody announced yet. Only a counting backend has them; for any other this
    // makes no code (RecordFrame checks with requires).
    static auto lossMark()
        requires(
          requires { Backend::template lossMark<Level>(); } || requires { Backend::lossMark(); })
    {
        if constexpr(requires { Backend::template lossMark<Level>(); }) {
            return Backend::template lossMark<Level>();
        } else {
            return Backend::lossMark();
        }
    }

    template<typename Mark>
    static void lossSettle(Mark const mark)
        requires requires { Backend::lossSettle(mark); }
    {
        Backend::lossSettle(mark);
    }
};

}   // namespace uc_log::detail

namespace uc_log {
/// The backend a line of `Level` on `Tag` writes through: ComBackend<Tag> bound to its canonical level. What the log
/// macros use, and what a backend that wraps another one (a fan-out to a ring and RTT) names for the inner one.
template<typename Tag, LogLevel Level>
using ResolveBackend
  = detail::LevelBoundBackend<ComBackend<Tag>,
                              detail::LevelAware<ComBackend<Tag>>
                                ? detail::canonicalLevel<ComBackend<Tag>, Level>()
                                : LogLevel::info>;
}   // namespace uc_log
