#pragma once

// What an RTT backend does about a log record it cannot write: a ring in skip mode that is full,
// or DebuggerPresentFunction saying no. The last template parameter of the three RTT backends.
//
//   LossPolicy::Silent                  the record is gone, nothing says so (the default: the image
//                                       is byte-identical to one without this parameter)
//   LossPolicy::Counted<>               every dropped record is counted per ring (::lossLedger, read
//                                       by the printer), and the next record that fits on that ring
//                                       is preceded by "⚠ N log records lost on this channel"; a
//                                       record is dropped whole, never a piece of it. From
//                                       uc_log::panicMode() on, a record that does not fit waits
//                                       while the host keeps reading (Kvasir::Panic::beginFatal()
//                                       calls it before a fatal log line)
//   LossPolicy::Counted<0>              counted and reported, never waits
//
// PanicStallUs: how long the host may leave its read offset unchanged before a waiting write gives
// up (and every later wait ends at once until the host reads again). Timed by the chip package's
// microsecond counter where it has one (kvasir_announced_reset_time_us(), a register read: safe
// with interrupts masked), as Kvasir's AnnouncedReset does; without one counted in polls at
// MaxCoreHz and the fewest cycles a poll can take, which waits longer - up to ~40 times on an M0+
// (detail::StallWaiter). 250 ms is below every watchdog timeout of the firmwares here; a shorter
// watchdog may fire during a panic's wait.

#include "ComBackend.hpp"
#include "Tag.hpp"

#include <concepts>
#include <cstdint>

namespace uc_log::LossPolicy {

struct Silent {};

template<std::uint32_t PanicStallUs = 250'000, std::uint64_t MaxCoreHz = 200'000'000>
struct Counted {
    static constexpr std::uint32_t panicStallUs = PanicStallUs;
    // a poll is a volatile load, a compare and a count: 4 cycles at the very least
    static constexpr std::uint32_t MinCyclesPerPoll = 4;
    static constexpr std::uint32_t panicStallPolls
      = static_cast<std::uint32_t>(MaxCoreHz * PanicStallUs / 1'000'000 / MinCyclesPerPoll);
};

template<typename T>
inline constexpr bool isCounted = false;
template<std::uint32_t PanicStallUs, std::uint64_t MaxCoreHz>
inline constexpr bool isCounted<Counted<PanicStallUs, MaxCoreHz>> = true;

template<typename T>
concept Policy = std::same_as<T, Silent> || isCounted<T>;

}   // namespace uc_log::LossPolicy

namespace uc_log {
namespace detail {
    // the tag's backend, if it is complete (a host test has none)
    template<typename T>
    concept HasPanicMode
      = requires { sizeof(ComBackend<T>); } && requires { ComBackend<T>::enterPanicMode(); };
    template<typename T>
    concept HasDrain = requires { sizeof(ComBackend<T>); } && requires { ComBackend<T>::drain(); };
}   // namespace detail

/// Zephyr's log_panic(): from here to the reset, a record that does not fit waits while the host
/// keeps reading (LossPolicy::Counted with PanicStallUs != 0). Nothing for any other backend.
/// Kvasir::Panic::beginFatal() calls it before every fatal log line.
template<typename T = Tag::User>
inline void panicMode() {
    if constexpr(detail::HasPanicMode<T>) { ComBackend<T>::enterPanicMode(); }
}

/// Wait until the host has read every log ring, or has not read for the stall time. For a reset no
/// printer was told about (SYSRESETREQ, a panic handler's reset); AnnouncedReset::announce() has
/// the printer drain by itself. Nothing for a backend that does not count.
template<typename T = Tag::User>
inline void drain() {
    if constexpr(detail::HasDrain<T>) { ComBackend<T>::drain(); }
}
}   // namespace uc_log
