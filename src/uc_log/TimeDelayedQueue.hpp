#pragma once

#include <algorithm>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <ranges>
#include <set>
#include <thread>
#include <vector>

template<typename Entry, typename Projection, typename Function>
struct TimeDelayedQueue {
private:
    using Clock = std::chrono::steady_clock;

    struct QEntry {
        Clock::time_point                     entryTime;
        std::chrono::system_clock::time_point sys_entryTime;
        Entry                                 entry;
    };

    std::vector<QEntry>              q{};
    [[no_unique_address]] Projection proj;
    [[no_unique_address]] Function   f;

    std::condition_variable cv;
    std::mutex              m;
    std::jthread            thread{std::bind_front(&TimeDelayedQueue::run, this)};

    void run(std::stop_token const& stoken) {
        std::vector<QEntry> toHandle{};
        while(!stoken.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock{m};
                cv.wait_for(lock, std::chrono::milliseconds{50});
                std::ranges::sort(q, std::ranges::less{}, proj);
                auto const deadline = Clock::now() - std::chrono::milliseconds{200};
                auto const pos      = std::ranges::partition_point(
                  q,
                  [&](auto const& entryTime) { return entryTime >= deadline; },
                  [](auto const& entry) { return entry.entryTime; });
                toHandle.reserve(static_cast<std::size_t>(std::distance(pos, q.end())));
                toHandle.assign(std::make_move_iterator(pos), std::make_move_iterator(q.end()));

                q.erase(pos, q.end());
            }

            for(auto const& entry : toHandle) {
                f(entry.sys_entryTime, entry.entry);
                if(stoken.stop_requested()) { return; }
            }
            toHandle.clear();
        }
    }

public:
    TimeDelayedQueue(Projection&& projection,
                     Function&&   func)
      : proj{std::move(projection)}
      , f{std::move(func)} {}

    template<typename E>
    void append(E&& entry) {
        {
            std::lock_guard<std::mutex> const lock{m};
            q.emplace_back(Clock::now(), std::chrono::system_clock::now(), std::forward<E>(entry));
        }
        cv.notify_one();
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
template<typename P,
         typename F>
TimeDelayedQueue(P&&,
                 F&&) -> TimeDelayedQueue<detail::entry_type_t<std::remove_cvref_t<F>>,
                                          std::remove_cvref_t<P>,
                                          std::remove_cvref_t<F>>;
