#pragma once

// The log sink protocol: what uc_log asks of a ComBackend<Tag> (a "sink"), in one place.
//
// A sink is a type with static members only. Every member but write() is optional; uc_log asks for each with
// `requires`, so a sink declares only what it does. "Level" marks a hook that may also be level-templated
// (`template<LogLevel L> static ...`): a sink that takes the level gets the record's level, any other gets one code
// path for every level (detail::LevelBoundBackend).
//
//   member                                     asked by                 kind
//   write(std::span<std::byte const>)          Printer                  required, Level
//   initTransfer() / finalizeTransfer()        Printer                  optional, Level (bracket one record)
//   RecordGuard                                RecordFrame              optional type, held for a whole record
//   noteRecord()                               RecordFrame              optional (RTT: the boot logged, RttEpoch.hpp)
//   lossMark() / lossSettle(mark)              RecordFrame              optional, lossMark Level (LossPolicy::Counted)
//   enterPanicMode() / drain()                 uc_log::panicMode/drain  optional (Runtime.hpp)
//   bootsThatLogged()                          uc_log::bootsThatLogged  optional (RTT: RttEpoch.hpp's count); without
//                                                                       it the call does not compile
//   kLevelFloor                                ResolveBackend           optional LogLevel: levels from it up alike
//   numDuplexChannels, DuplexChannel<I>        the application          optional (DuplexChannel.hpp)
//   isStartupEntry                             Kvasir Startup           listed with nothing to run
//   preMemoryInit() / postMemoryInit()         Kvasir Startup           optional, around .data/.bss init
//   isrPriorityLevels, singleIsrPriorityLevel,
//   silentIsrPriorityLevels, perCoreIsrContexts Kvasir Startup          optional plain std::array / bool members:
//                                                                       the ISR contract (IsrPolicy.hpp)
//
// FanOutBackend.hpp forwards every row to its sinks (bootsThatLogged: the primary's); the Startup rows need nothing
// from Kvasir (plain members).

#include "LogLevel.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace uc_log {

// The sink's RecordGuard, void when it has none.
template<typename Sink>
struct RecordGuardOf {
    using type = void;
};

template<typename Sink>
    requires requires { typename Sink::RecordGuard; }
struct RecordGuardOf<Sink> {
    using type = typename Sink::RecordGuard;
};

namespace detail {
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
}   // namespace detail

}   // namespace uc_log
