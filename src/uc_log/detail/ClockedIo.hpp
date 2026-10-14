#pragma once

// asio on the printer's clock (Clock.hpp): timers that expire on Clock's time, and an io_context loop that takes part
// in a test's virtual time. Without a VirtualTime both are plain asio: steady_clock timers, io_context::run().

#include "uc_log/detail/Clock.hpp"

#include <boost/asio/basic_waitable_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>

namespace uc_log::detail {

// How long asio's reactor may block for a timer (its timerfd or epoll timeout): the time left, on the real clock - or,
// under a VirtualTime, nothing: the time left is virtual, the reactor must look at the timers on every poll (a timerfd
// armed for the virtual time left fired on the real one).
struct ClockWaitTraits {
    static Clock::duration to_wait_duration(Clock::duration const& d) {
        return Clock::isVirtual() ? Clock::duration::zero() : d;
    }

    static Clock::duration to_wait_duration(Clock::time_point const& t) {
        return to_wait_duration(t - Clock::now());
    }
};

using ClockTimer = boost::asio::basic_waitable_timer<Clock, ClockWaitTraits>;

// Runs `ioc` until it is stopped or out of work, on the calling thread (a Clock::Thread). Under a VirtualTime the
// thread cannot block in epoll - the time would move on while it works: it runs what is ready, and waits a millisecond
// on the clock when nothing is. Its clients use unix sockets there: their data is in the socket when the write returns.
inline void runIo(boost::asio::io_context& ioc) {
    if(!Clock::isVirtual()) {
        ioc.run();
        return;
    }
    while(!ioc.stopped()) {
        if(ioc.poll() == 0 && !ioc.stopped()) { Clock::sleep_for(std::chrono::milliseconds{1}); }
    }
}

}   // namespace uc_log::detail
