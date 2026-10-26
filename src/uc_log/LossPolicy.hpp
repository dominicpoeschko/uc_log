#pragma once

// What the RTT backend does about a log record it cannot write: a ring in skip mode that is full,
// or DebuggerPresentFunction saying no. An RttLog option (RttLog.hpp), RttComBackend's last parameter.
//
//   LossPolicy::Silent                  the record is gone, nothing says so (the default: no ledger,
//                                       no staging, nothing of the policy in the image)
//   LossPolicy::Counted<CpuHzCeiling>   every dropped record is counted per ring (::lossLedger, read
//                                       by the printer), and the next record that fits on that ring
//                                       is preceded by "⚠ N log records lost on this channel"; a
//                                       record is dropped whole, never a piece of it. From
//                                       uc_log::panicMode() on (Runtime.hpp), a record that does
//                                       not fit waits while the host keeps reading
//                                       (Kvasir::Panic::beginFatal() calls it before a fatal log
//                                       line)
//   LossPolicy::Counted<CpuHzCeiling, 0> counted and reported, never waits
//
// CpuHzCeiling: the fastest the image's CPU runs, in Hz - required, no default: uc_log knows no chip.
// A Kvasir image passes its chip's `Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User>::value` (the
// chip's WaitBounds.hpp, included before the backend), any other image the clock of its part. 0 is
// refused (what that hook's primary holds when no chip specialised it).
//
// PanicStallUs: how long the host may leave its read offset unchanged before a waiting write gives
// up (and every later wait ends at once until the host reads again). Timed by the chip package's
// microsecond counter where it has one (kvasir_announced_reset_time_us(), a register read: safe
// with interrupts masked), as Kvasir's AnnouncedReset does; without one counted in polls at
// CpuHzCeiling and the fewest cycles a poll can take, which waits longer - up to ~40 times on an M0+
// (detail::StallWaiter) - and never shorter. A watchdog timeout shorter than PanicStallUs may fire
// during a panic's wait: keep the stall below the image's watchdog timeout.

#include <concepts>
#include <cstdint>

namespace uc_log::LossPolicy {

struct Silent {};

template<std::uint64_t CpuHzCeiling, std::uint32_t PanicStallUs = 250'000>
struct Counted {
    static_assert(
      CpuHzCeiling != 0,
      "LossPolicy::Counted<CpuHzCeiling>: the fastest CPU clock of the image in Hz, not 0 - "
      "a Kvasir image: Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User>::value with the "
      "chip's WaitBounds.hpp included before the log backend");
    static constexpr std::uint64_t cpuHzCeiling = CpuHzCeiling;
    static constexpr std::uint32_t panicStallUs = PanicStallUs;
    // a poll is a volatile load, a compare and a count: 4 cycles at the very least
    static constexpr std::uint32_t minCyclesPerPoll = 4;
    static constexpr std::uint32_t panicStallPolls
      = static_cast<std::uint32_t>(CpuHzCeiling * PanicStallUs / 1'000'000 / minCyclesPerPoll);
};

template<typename T>
inline constexpr bool isCounted = false;
template<std::uint64_t CpuHzCeiling, std::uint32_t PanicStallUs>
inline constexpr bool isCounted<Counted<CpuHzCeiling, PanicStallUs>> = true;

// Reading a Counted's ceiling instantiates it: a 0 stops at its static_assert where the backend names the policy,
// not only where a wait first reads the polls.
template<typename T>
concept Policy = std::same_as<T, Silent> || (isCounted<T> && T::cpuHzCeiling != 0);

}   // namespace uc_log::LossPolicy
