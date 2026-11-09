#pragma once

// The RTXUI window's worker: a thread for what may block (the probe), its jobs in order.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace uc_log::printer::rtx {

/// Jobs that may block (a core op waits up to TargetPort::defaultTimeout, a reconnect longer), one after the other on
/// a thread of their own: the window never waits for the probe. Destroyed: the job running ends, the rest go.
class Worker {
public:
    Worker() = default;

    void post(std::function<void()> job) {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

private:
    std::mutex                        mutex_;
    std::condition_variable_any       wake_;
    std::deque<std::function<void()>> jobs_;
    std::jthread                      thread_{[this](std::stop_token const& stop) {
        while(true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock{mutex_};
                if(!wake_.wait(lock, stop, [this] { return !jobs_.empty(); })) { return; }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }};   // last: started once the members it uses are there
};

}   // namespace uc_log::printer::rtx
