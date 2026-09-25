#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Merges the log lines of several channels into one time line, handed to `f` from its own thread:
//  1. a channel's lines come out in the order they were appended;
//  2. across channels the smallest target time goes first, after waiting `delay`;
//  3. no line waits longer than `delay + maxHold` (channel clocks are not comparable);
//  4. the destructor hands out everything still queued.
template<typename Entry, typename TimeProjection, typename ChannelProjection, typename Function>
struct TimeDelayedQueue {
private:
    using Clock = std::chrono::steady_clock;

    struct QEntry {
        Clock::time_point                     arrival;
        std::chrono::system_clock::time_point sysArrival;
        Entry                                 entry;
    };

    using ChannelKey = std::remove_cvref_t<std::invoke_result_t<ChannelProjection&, Entry const&>>;

    std::map<ChannelKey, std::deque<QEntry>> channels{};
    [[no_unique_address]] TimeProjection     time;
    [[no_unique_address]] ChannelProjection  channelOf;
    [[no_unique_address]] Function           f;
    std::chrono::milliseconds                delay;
    std::chrono::milliseconds                maxHold;
    std::condition_variable_any              cv;
    std::mutex                               m;
    std::jthread                             thread;

    // `all` ignores the waiting times
    std::deque<QEntry>* next(Clock::time_point now,
                             bool              all) {
        std::deque<QEntry>* earliestTime    = nullptr;
        std::deque<QEntry>* earliestArrival = nullptr;
        auto const          before          = [this](QEntry const& a, QEntry const& b) {
            auto const ta = std::invoke(time, a.entry);
            auto const tb = std::invoke(time, b.entry);
            return ta < tb || (!(tb < ta) && a.arrival < b.arrival);
        };
        for(auto& [key, q] : channels) {
            if(q.empty()) { continue; }
            if(earliestTime == nullptr || before(q.front(), earliestTime->front())) {
                earliestTime = &q;
            }
            if(earliestArrival == nullptr || q.front().arrival < earliestArrival->front().arrival) {
                earliestArrival = &q;
            }
        }
        if(earliestTime == nullptr || all) { return earliestTime; }
        if(earliestTime->front().arrival + delay <= now) { return earliestTime; }
        if(earliestArrival->front().arrival + delay + maxHold <= now) { return earliestArrival; }
        return nullptr;
    }

    void handOut(bool all) {
        std::vector<QEntry> batch;
        {
            std::lock_guard<std::mutex> const lock{m};
            auto const                        now = Clock::now();
            while(auto* const q = next(now, all)) {
                batch.push_back(std::move(q->front()));
                q->pop_front();
            }
        }
        for(auto const& e : batch) { f(e.sysArrival, e.entry); }
    }

    void run(std::stop_token const& stoken) {
        while(!stoken.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock{m};
                cv.wait_for(lock, stoken, std::chrono::milliseconds{50}, [] { return false; });
            }
            handOut(false);
        }
    }

public:
    TimeDelayedQueue(TimeProjection&&          timeProjection,
                     ChannelProjection&&       channelProjection,
                     Function&&                func,
                     std::chrono::milliseconds delay_   = std::chrono::milliseconds{200},
                     std::chrono::milliseconds maxHold_ = std::chrono::milliseconds{1000})
      : time{std::move(timeProjection)}
      , channelOf{std::move(channelProjection)}
      , f{std::move(func)}
      , delay{delay_}
      , maxHold{maxHold_}
      , thread{std::bind_front(&TimeDelayedQueue::run,
                               this)} {}

    TimeDelayedQueue(TimeDelayedQueue const&)            = delete;
    TimeDelayedQueue& operator=(TimeDelayedQueue const&) = delete;

    ~TimeDelayedQueue() {
        thread.request_stop();
        thread.join();
        handOut(true);   // rule 4
    }

    template<typename E>
    void append(E&& entry) {
        std::lock_guard<std::mutex> const lock{m};
        auto const                        key = std::invoke(channelOf, entry);
        channels[key].push_back(
          QEntry{Clock::now(), std::chrono::system_clock::now(), Entry(std::forward<E>(entry))});
    }
};

// Deduction guide helper to extract Entry type from function signature
namespace detail {
template<typename F>
struct function_traits;

template<typename R, typename T1, typename T2>
struct function_traits<R(T1, T2)> {
    using entry_type = std::remove_cvref_t<T2>;
};

template<typename R, typename T1, typename T2>
struct function_traits<R (*)(T1, T2)> {
    using entry_type = std::remove_cvref_t<T2>;
};

template<typename C, typename R, typename T1, typename T2>
struct function_traits<R (C::*)(T1, T2) const> {
    using entry_type = std::remove_cvref_t<T2>;
};

template<typename C, typename R, typename T1, typename T2>
struct function_traits<R (C::*)(T1, T2)> {
    using entry_type = std::remove_cvref_t<T2>;
};

template<typename Lambda>
struct function_traits : function_traits<decltype(&Lambda::operator())> {};

template<typename F>
using entry_type_t = typename function_traits<F>::entry_type;
}   // namespace detail

// Deduction guide: deduce Entry from Function's second parameter type
template<typename T,
         typename C,
         typename F,
         typename... Durations>
TimeDelayedQueue(T&&,
                 C&&,
                 F&&,
                 Durations...) -> TimeDelayedQueue<detail::entry_type_t<std::remove_cvref_t<F>>,
                                                   std::remove_cvref_t<T>,
                                                   std::remove_cvref_t<C>,
                                                   std::remove_cvref_t<F>>;
