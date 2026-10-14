#pragma once

// Decoding on a thread of its own, so the reader keeps the probe busy. Read and decode in turn on one thread leave
// the probe idle while a pass's bytes are decoded: at full rate the decode took ~15 % of each pass and the printer
// read ~25 % less than the probe could carry, so a ring the target filled at the probe's rate overflowed.
//
// Items are decoded in the order they were pushed, one at a time. barrier() returns once every pushed item is decoded
// and the worker waits for the next: from then until the next push, the caller may touch what the decode touches.
// The destructor decodes what is still queued (those bytes are gone from the target) before it joins. An exception
// from the decode is rethrown on the caller's thread by the next push() or barrier().

#include "uc_log/detail/Clock.hpp"

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace uc_log::detail {

class DecodeWorker {
public:
    struct Item {
        std::size_t            slot{};   // which channel
        std::vector<std::byte> bytes{};
        bool                   haltedRecently{};
    };

    // `decode` returns whether the item gave a line; `maxQueuedBytes`: push() waits while more than that are queued -
    // small on purpose: what the decoder has not caught up with is better left in the target's rings (the printer's
    // own reader takes it from there later) than piled up here, where a stop must decode all of it before it ends
    // (4 MiB took 25 s behind a slow consumer)
    explicit DecodeWorker(std::function<bool(Item&)>&& decode,
                          std::size_t                  maxQueuedBytes = std::size_t{64} << 10U)
      : decode_{std::move(decode)}
      , maxQueuedBytes_{maxQueuedBytes}
      , thread_{[this]() { run(); }} {}

    DecodeWorker(DecodeWorker const&)            = delete;
    DecodeWorker& operator=(DecodeWorker const&) = delete;

    ~DecodeWorker() {
        {
            std::unique_lock<std::mutex> lock{m_};
            idle_.wait(lock, [this]() { return items_.empty() && !busy_; });
            stop_ = true;
        }
        work_.notify_all();
        thread_.join();
    }

    void push(Item item) {
        {
            std::unique_lock<std::mutex> lock{m_};
            rethrowLocked();
            idle_.wait(lock, [this]() { return queuedBytes_ <= maxQueuedBytes_ || error_; });
            rethrowLocked();
            queuedBytes_ += item.bytes.size();
            items_.push_back(std::move(item));
        }
        work_.notify_one();
    }

    void barrier() {
        std::unique_lock<std::mutex> lock{m_};
        idle_.wait(lock, [this]() { return (items_.empty() && !busy_) || error_; });
        rethrowLocked();
    }

    // whether an item gave a line since the last call
    bool takePrinted() {
        std::lock_guard<std::mutex> const lock{m_};
        return std::exchange(printed_, false);
    }

private:
    void rethrowLocked() {
        if(error_) { std::rethrow_exception(std::exchange(error_, nullptr)); }
    }

    void run() {
        std::unique_lock<std::mutex> lock{m_};
        while(true) {
            work_.wait(lock, [this]() { return stop_ || !items_.empty(); });
            if(items_.empty()) { return; }   // stop_, and nothing left
            Item item = std::move(items_.front());
            items_.pop_front();
            busy_ = true;
            lock.unlock();
            bool               printed = false;
            std::exception_ptr error;
            try {
                printed = decode_(item);
            } catch(...) { error = std::current_exception(); }
            lock.lock();
            busy_ = false;
            queuedBytes_ -= item.bytes.size();
            printed_ = printed_ || printed;
            if(error && !error_) { error_ = error; }
            idle_.notify_all();
        }
    }

    std::function<bool(Item&)> decode_;
    std::size_t                maxQueuedBytes_;
    std::mutex                 m_;
    Clock::ConditionVariable   work_;
    Clock::ConditionVariable   idle_;
    std::deque<Item>           items_;
    std::size_t                queuedBytes_{};
    bool                       busy_{};
    bool                       stop_{};
    bool                       printed_{};
    std::exception_ptr         error_;
    Clock::Thread              thread_;   // last: started once everything above exists
};

}   // namespace uc_log::detail
