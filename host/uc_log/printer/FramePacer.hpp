#pragma once

// How often a window draws, for any front end: the producers (the decoder with every line, the build thread, the
// messages) ask for a frame from any thread, as often as they like; the window draws at most once per interval. A
// request while a frame is scheduled is the same frame; a request during a frame (after begin()) is the next one, so
// nothing asked for is lost. Input is not paced - the front end draws a key press at once - and a frame drawn for
// input counts as a frame here too (begin()).
//
// The decision is a function of the time it is handed, never of a clock it reads: tests give the time points.
//
//   any thread:  if(pacer.request()) post_delayed(wake, pacer.delay(now));   // the first request since the last
//                                                                             // begin() posts it, for when it is due
//   UI thread:   wake: if(auto d = pacer.delay(now); d > 0) post_delayed(wake, d); else { pacer.begin(now); draw(); }
//
// The requester's own delay makes a flood's frame one wake-up of the UI thread, not one that only finds it early.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <optional>

namespace uc_log::printer {

class FramePacer {
public:
    using Clock     = std::chrono::steady_clock;
    using Duration  = Clock::duration;
    using TimePoint = Clock::time_point;

    /// 33 ms: 30 frames a second - a scrolling log looks the same at 60, for twice the frames.
    static constexpr Duration defaultInterval = std::chrono::milliseconds{33};

    explicit FramePacer(Duration interval = defaultInterval) : interval_{interval} {}

    /// A frame is wanted. From any thread. True: the caller posts the wake-up (nothing is scheduled yet).
    bool request() { return !scheduled_.exchange(true, std::memory_order_acq_rel); }

    /// How long to wait before the frame (zero: draw now). Any thread: the requester posts its wake-up for then, the
    /// wake-up asks again.
    [[nodiscard]] Duration delay(TimePoint now) const {
        auto const last = last_.load(std::memory_order_acquire);
        if(last == never) { return Duration::zero(); }
        auto const due = TimePoint{Duration{last}} + interval_;
        return now >= due ? Duration::zero() : due - now;
    }

    /// UI thread, as a frame starts (before it reads what it shows): a request from now on is a new frame.
    void begin(TimePoint now) {
        last_.store(now.time_since_epoch().count(), std::memory_order_release);
        scheduled_.store(false, std::memory_order_release);
    }

    /// UI thread: a frame drawn for input; the next paced one waits an interval from it.
    void drewFor(TimePoint now) {
        last_.store(now.time_since_epoch().count(), std::memory_order_release);
    }

    [[nodiscard]] Duration interval() const { return interval_; }

    void setInterval(Duration interval) { interval_ = std::max(interval, Duration::zero()); }

private:
    static constexpr Duration::rep never = std::numeric_limits<Duration::rep>::min();

    Duration                   interval_;
    std::atomic<bool>          scheduled_{false};
    std::atomic<Duration::rep> last_{
      never};   // the last frame's start (since the epoch): read by requesters
};

}   // namespace uc_log::printer
