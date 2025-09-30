#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <ranges>
#include <thread>
#include <vector>

template<typename Entry, typename Projection>
struct TimeDelayedQueue {
private:
    using Clock = std::chrono::steady_clock;

    struct QEntry {
        Clock::time_point                     entryTime;
        std::chrono::system_clock::time_point sys_entryTime;
        Entry                                 entry;
    };

    std::vector<QEntry>                                                      q{};
    std::function<void(std::chrono::system_clock::time_point, Entry const&)> f;

    std::condition_variable cv;
    std::mutex              m;
    std::jthread            thread{std::bind_front(&TimeDelayedQueue::run, this)};

    void run(std::stop_token const& stoken) {
        std::vector<QEntry> toHandle{};
        while(!stoken.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock{m};
                cv.wait_for(lock, std::chrono::milliseconds{50});
                std::ranges::sort(q, std::ranges::greater{}, Projection{});
                auto const deadline = Clock::now() - std::chrono::milliseconds{200};
                auto const pos      = std::ranges::partition_point(
                  q,
                  [&](auto const& entryTime) { return deadline < entryTime; },
                  [](auto const& entry) { return entry.entryTime; });
                toHandle.insert(toHandle.begin(), pos, q.end());
                q.erase(pos, q.end());
            }

            for(auto const& entry : std::views::reverse(toHandle)) {
                f(entry.sys_entryTime, entry.entry);
                if(stoken.stop_requested()) {
                    return;
                }
            }
            toHandle.clear();
        }
    }

public:
    template<typename F>
    explicit TimeDelayedQueue(F func) : f{std::move(func)} {}

    template<typename E>
    void append(E&& entry) {
        {
            std::lock_guard<std::mutex> const lock{m};
            q.emplace_back(Clock::now(), std::chrono::system_clock::now(), std::forward<E>(entry));
        }
        cv.notify_one();
    }
};
