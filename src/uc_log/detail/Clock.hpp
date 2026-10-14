#pragma once

// The printer's clock: steady_clock, or - once a test constructs a VirtualTime - a virtual time that only moves when
// every thread taking part in it waits on it. No test then depends on how fast the machine runs: a wait of 4 s in a
// test is 4 s of the printer's own time, however long the sanitized build takes to get through them.
//
// What takes part: the thread that constructed the VirtualTime and every Clock::Thread started while it exists. A
// participant blocks only through this header (sleep_for/sleep_until, ConditionVariable, Thread::join); then, when all
// of them block, the time jumps to the earliest deadline and wakes whoever waits for it. A wake-up (a deadline reached,
// a notify, a thread's end) is counted by the thread that makes it, under one mutex, so the time never moves between a
// notify and the woken thread running again. A participant blocked any other way (a std::mutex held by a sleeping
// one, a socket) counts as running: the time stands still, and the test hangs instead of moving on too early. When
// every participant waits without a deadline nothing can wake them: that is reported and the program aborts.
//
// Other threads (std::thread, the asio threads) may use the clock too; they wait for the time the participants make
// and never hold it up.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace uc_log::detail {

class VirtualTime;

namespace clock_detail {
    inline std::atomic<VirtualTime*> installed{nullptr};
    inline thread_local bool         participant{false};
}   // namespace clock_detail

class VirtualTime {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration   = std::chrono::steady_clock::duration;

    // installs it; the constructing thread takes part
    VirtualTime() : now_{std::chrono::steady_clock::now()} {
        VirtualTime* expected = nullptr;
        if(!clock_detail::installed.compare_exchange_strong(expected, this)) {
            std::fputs("uc_log VirtualTime: a second one while one is installed\n", stderr);
            std::abort();
        }
        clock_detail::participant = true;
    }

    VirtualTime(VirtualTime const&)            = delete;
    VirtualTime& operator=(VirtualTime const&) = delete;

    // every Clock::Thread started under it has ended
    ~VirtualTime() {
        clock_detail::participant = false;
        clock_detail::installed   = nullptr;
    }

    time_point now() {
        std::lock_guard<std::mutex> const lock{mutex_};
        return now_;
    }

    // Blocks the calling thread until `deadline` or a notify on `channel` (nullptr: none) - `unlock` is called once it
    // is registered (a notify after it is not lost), `relock` before it returns.
    template<typename Unlock,
             typename Relock>
    void block(time_point  deadline,
               void const* channel,
               Unlock&&    unlock,
               Relock&&    relock) {
        std::unique_lock<std::mutex> lock{mutex_};
        Waiter                       w{deadline, channel, clock_detail::participant};
        if(deadline <= now_) {
            unlock();
            lock.unlock();
            relock();
            return;
        }
        auto const it = waiters_.insert(waiters_.end(), &w);
        if(w.counted) { ++blocked_; }
        unlock();
        advanceWhileIdle();
        cv_.wait(lock, [&w]() { return w.woken; });
        waiters_.erase(it);
        lock.unlock();
        relock();
    }

    void notify(void const* channel,
                bool        all) {
        std::lock_guard<std::mutex> const lock{mutex_};
        bool                              any = false;
        for(auto* w : waiters_) {
            if(w->woken || w->channel != channel) { continue; }
            wake(*w);
            any = true;
            if(!all) { break; }
        }
        if(any) { cv_.notify_all(); }
    }

    void addParticipant() {
        std::lock_guard<std::mutex> const lock{mutex_};
        ++participants_;
    }

    void removeParticipant() {
        std::lock_guard<std::mutex> const lock{mutex_};
        --participants_;
        advanceWhileIdle();
    }

private:
    struct Waiter {
        time_point  deadline;
        void const* channel;
        bool        counted;
        bool        woken{};
    };

    void wake(Waiter& w) {
        w.woken = true;
        if(w.counted) { --blocked_; }
    }

    // mutex_ held
    void advanceWhileIdle() {
        while(participants_ > 0 && blocked_ == participants_) {
            auto next = time_point::max();
            for(auto const* w : waiters_) {
                if(!w->woken && w->deadline < next) { next = w->deadline; }
            }
            if(next == time_point::max()) {
                std::fprintf(
                  stderr,
                  "uc_log VirtualTime: all %d threads wait without a deadline - a deadlock\n",
                  participants_);
                std::abort();
            }
            now_ = next;
            for(auto* w : waiters_) {
                if(!w->woken && w->deadline <= now_) { wake(*w); }
            }
            cv_.notify_all();
        }
    }

    std::mutex              mutex_;
    std::condition_variable cv_;
    time_point              now_;
    int                     participants_{1};   // the constructing thread
    int                     blocked_{};
    std::list<Waiter*>      waiters_;
};

struct Clock {
    using rep                       = std::chrono::steady_clock::rep;
    using period                    = std::chrono::steady_clock::period;
    using duration                  = std::chrono::steady_clock::duration;
    using time_point                = std::chrono::steady_clock::time_point;
    static constexpr bool is_steady = true;

    // a test's VirtualTime is installed
    static bool isVirtual() {
        return clock_detail::installed.load(std::memory_order_acquire) != nullptr;
    }

    static time_point now() {
        if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
            return v->now();
        }
        return std::chrono::steady_clock::now();
    }

    static void sleep_until(time_point t) {
        if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
            v->block(t, nullptr, []() {}, []() {});
            return;
        }
        std::this_thread::sleep_until(t);
    }

    template<typename Rep,
             typename Period>
    static void sleep_for(std::chrono::duration<Rep,
                                                Period> d) {
        if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
            v->block(v->now() + std::chrono::ceil<duration>(d), nullptr, []() {}, []() {});
            return;
        }
        std::this_thread::sleep_for(d);
    }

    // std::condition_variable_any on the Clock's time
    class ConditionVariable {
    public:
        void notify_one() {
            if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
                v->notify(this, false);
            }
            cv_.notify_one();
        }

        void notify_all() {
            if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
                v->notify(this, true);
            }
            cv_.notify_all();
        }

        template<typename Lock,
                 typename Predicate>
        void wait(Lock&     lock,
                  Predicate pred) {
            while(!pred()) {
                if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
                    v->block(
                      time_point::max(),
                      this,
                      [&]() { lock.unlock(); },
                      [&]() { lock.lock(); });
                } else {
                    cv_.wait(lock);
                }
            }
        }

        template<typename Lock,
                 typename Predicate>
        bool wait_until(Lock&      lock,
                        time_point t,
                        Predicate  pred) {
            while(!pred()) {
                if(auto* const v = clock_detail::installed.load(std::memory_order_acquire)) {
                    if(v->now() >= t) { return pred(); }
                    v->block(t, this, [&]() { lock.unlock(); }, [&]() { lock.lock(); });
                } else if(cv_.wait_until(lock, t) == std::cv_status::timeout) {
                    return pred();
                }
            }
            return true;
        }

        template<typename Lock,
                 typename Rep,
                 typename Period,
                 typename Predicate>
        bool wait_for(Lock&                         lock,
                      std::chrono::duration<Rep,
                                            Period> d,
                      Predicate                     pred) {
            return wait_until(lock, now() + std::chrono::ceil<duration>(d), std::move(pred));
        }

        // returns early (pred() as it stands) when a stop is requested
        template<typename Lock,
                 typename Rep,
                 typename Period,
                 typename Predicate>
        bool wait_for(Lock&                         lock,
                      std::stop_token const&        stoken,
                      std::chrono::duration<Rep,
                                            Period> d,
                      Predicate                     pred) {
            std::stop_callback const onStop{stoken, [this]() { notify_all(); }};
            return wait_until(lock,
                              now() + std::chrono::ceil<duration>(d),
                              [&]() { return stoken.stop_requested() || pred(); })
                && pred();
        }

    private:
        std::condition_variable_any cv_;
    };

    // std::jthread taking part in the virtual time; the destructor asks it to stop and joins it
    class Thread {
    public:
        Thread() = default;

        template<typename F>
        explicit Thread(F&& body) : exit_{std::make_shared<Exit>()} {
            auto* const v = clock_detail::installed.load(std::memory_order_acquire);
            if(v != nullptr) {
                v->addParticipant();
            }   // before it runs: the time waits for its first step
            thread_ = std::jthread{[v, exit = exit_, f = std::forward<F>(body)](
                                     std::stop_token stoken) mutable {
                clock_detail::participant = v != nullptr;
                struct Leave {
                    VirtualTime*          v;
                    std::shared_ptr<Exit> exit;

                    ~Leave() {
                        {
                            std::lock_guard<std::mutex> const lock{exit->mutex};
                            exit->done = true;
                        }
                        exit->cv
                          .notify_all();   // the joiner runs again first: the time does not move here
                        if(v != nullptr) { v->removeParticipant(); }
                    }
                } const leave{v, exit};
                if constexpr(std::is_invocable_v<F&, std::stop_token>) {
                    f(std::move(stoken));
                } else {
                    f();
                }
            }};
        }

        Thread(Thread&&) noexcept = default;

        Thread& operator=(Thread&& other) noexcept {
            if(this != &other) {
                stopAndJoin();
                exit_   = std::move(other.exit_);
                thread_ = std::move(other.thread_);
            }
            return *this;
        }

        ~Thread() { stopAndJoin(); }

        bool joinable() const noexcept { return thread_.joinable(); }

        bool request_stop() noexcept { return thread_.request_stop(); }

        std::stop_token get_stop_token() const noexcept { return thread_.get_stop_token(); }

        void join() {
            {
                std::unique_lock<std::mutex> lock{exit_->mutex};
                exit_->cv.wait(lock, [this]() { return exit_->done; });
            }
            thread_.join();
        }

    private:
        void stopAndJoin() {
            if(joinable()) {
                request_stop();
                join();
            }
        }

        struct Exit {
            std::mutex        mutex;
            ConditionVariable cv;
            bool              done{};
        };

        std::shared_ptr<Exit> exit_;
        std::jthread          thread_;   // last: started once exit_ exists
    };
};

}   // namespace uc_log::detail
