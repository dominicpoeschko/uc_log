#pragma once
// The signals that end the printer as its own quit does: SIGINT (Ctrl-C), SIGTERM (kill, a service manager) and SIGHUP
// (its terminal or tmux window closed). A printer such a signal killed outright left its session unended: the
// firmware's announced-reset block still armed (Kvasir_SDK Runtime/AnnouncedReset.hpp), so the board's next reset
// waited for an ack that never came - long enough for a short watchdog to reset the chip instead (an A/B update then
// failed: the watchdog's reset was a normal boot, not the update's) - and the J-Link not closed.
//
// So they are blocked, in the thread that makes a StopSignals and every thread started after it (make it first in
// main, before any other thread), and asked for: poll() takes one that is pending, without waiting. The main loop then
// returns, and the reader's stop ends the session as for SIGINT before: the block disarmed, RTT stopped, the J-Link
// closed, "the session ends ..." and "stopped jlink" said. A blocked signal is never delivered to a handler, so no
// other handler (RTXUI's terminal restore re-raises SIGHUP, SIGINT and SIGTERM) ends the process before that; the
// window, quit by the same request, restores the terminal on its way out.
//
// A signal mask survives fork and exec: a program the printer starts (the build, printer/BuildRunner.cpp) unblocks
// them between the two (unblockInChild()), or it would let Ctrl-C, a kill and its pty's hang-up go by and outlive the
// printer.
#include <array>
#include <csignal>
#include <optional>
#include <pthread.h>

namespace uc_log::printer {

class StopSignals {
public:
    static constexpr std::array<int, 3> signals{SIGINT, SIGTERM, SIGHUP};

    StopSignals() {
        sigemptyset(&set_);
        for(int const s : signals) { sigaddset(&set_, s); }
        pthread_sigmask(SIG_BLOCK, &set_, &previous_);
    }

    StopSignals(StopSignals const&)            = delete;
    StopSignals& operator=(StopSignals const&) = delete;

    /// The previous mask back: what was still pending is delivered as it would have been.
    ~StopSignals() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }

    /// A stop signal that is pending, taken (the first of several, in `signals`' order); nullopt: none.
    [[nodiscard]] std::optional<int> poll() {
        sigset_t pending;
        sigemptyset(&pending);
        if(sigpending(&pending) != 0) { return std::nullopt; }
        for(int const s : signals) {
            if(sigismember(&pending, s) == 1) {
                sigset_t one;
                sigemptyset(&one);
                sigaddset(&one, s);
                int taken{};
                if(sigwait(&one, &taken) == 0) { return taken; }
            }
        }
        return std::nullopt;
    }

    /// In a child between fork and exec: the stop signals unblocked, so the program it becomes takes them as any
    /// program does. Async-signal-safe (sigemptyset, sigaddset, sigprocmask), as code after a fork must be.
    static void unblockInChild() noexcept {
        sigset_t set;
        sigemptyset(&set);
        for(int const s : signals) { sigaddset(&set, s); }
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }

private:
    sigset_t set_{};
    sigset_t previous_{};
};

}   // namespace uc_log::printer
