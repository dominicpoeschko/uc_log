// detail/DecodeWorker.hpp: items decoded in push order, barrier() waits for all of them, the destructor decodes
// what is still queued, push() blocks at the byte bound, an exception reaches the pushing thread.
#include "uc_log/detail/Clock.hpp"
#include "uc_log/detail/DecodeWorker.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

using Worker = uc_log::detail::DecodeWorker;

Worker::Item item(std::size_t   slot,
                  std::size_t   n,
                  unsigned char fill) {
    return Worker::Item{slot, std::vector<std::byte>(n, std::byte{fill}), false};
}

void inOrder() {
    std::vector<std::size_t> seen;   // touched only by the worker until barrier()
    {
        Worker w{[&](Worker::Item& i) {
            seen.push_back(i.slot);
            uc_log::detail::Clock::sleep_for(std::chrono::microseconds{i.slot % 3 == 0 ? 200 : 0});
            return !i.bytes.empty();
        }};
        for(std::size_t k = 0; k != 500; ++k) { w.push(item(k, k % 2, 0)); }
        w.barrier();
        CHECK(seen.size() == 500, "every item decoded by barrier()");
        bool ordered = true;
        for(std::size_t k = 0; k != seen.size(); ++k) { ordered = ordered && seen[k] == k; }
        CHECK(ordered, "in push order");
        CHECK(w.takePrinted(), "an item gave a line");
        CHECK(!w.takePrinted(), "taken once");
        w.push(item(500, 0, 0));
        w.barrier();
        CHECK(!w.takePrinted(), "an empty item gives no line");
    }
}

void destructorDecodesTheRest() {
    std::atomic<int> n{0};
    {
        Worker w{[&](Worker::Item&) {
            uc_log::detail::Clock::sleep_for(std::chrono::microseconds{100});
            ++n;
            return true;
        }};
        for(int k = 0; k != 200; ++k) { w.push(item(0, 8, 1)); }
    }   // no barrier: the destructor
    CHECK(n == 200, "the destructor decodes what is queued");
}

void boundedQueue() {
    std::atomic<bool>             release{false};
    std::atomic<int>              decoded{0};
    Worker                        w{[&](Worker::Item&) {
                 while(!release) { uc_log::detail::Clock::sleep_for(std::chrono::milliseconds{1}); }
                 ++decoded;
                 return false;
                                    },
                                    1000};
    std::atomic<int>              pushed{0};
    uc_log::detail::Clock::Thread producer{[&]() {
        for(int k = 0; k != 10; ++k) {
            w.push(item(0, 400, 2));
            ++pushed;
        }
    }};
    uc_log::detail::Clock::sleep_for(std::chrono::milliseconds{50});
    // one item in the decode, at most 1000 bytes (+ the one that crossed the bound) queued: the producer waits
    CHECK(pushed < 10, "push waits at the byte bound");
    release = true;
    producer.join();
    w.barrier();
    CHECK(decoded == 10, "all decoded after the release");
}

void errorReachesThePusher() {
    Worker w{[](Worker::Item& i) -> bool {
        if(i.slot == 3) { throw std::runtime_error{"bad"}; }
        return false;
    }};
    for(std::size_t k = 0; k != 5; ++k) { w.push(item(k, 1, 0)); }
    bool thrown = false;
    try {
        w.barrier();
    } catch(std::runtime_error const&) { thrown = true; }
    CHECK(thrown, "barrier() rethrows the decode's exception");
    bool again = false;
    try {
        w.barrier();
    } catch(std::runtime_error const&) { again = true; }
    CHECK(!again, "once");
}
}   // namespace

int main() {
    // the decoder and the producers on the virtual clock: no check depends on how fast the machine runs
    uc_log::detail::VirtualTime const virtualTime;
    inOrder();
    destructorDecodesTheRest();
    boundedQueue();
    errorReachesThePusher();
    if(failures == 0) { std::printf("decode_worker_tests: ok\n"); }
    return failures == 0 ? 0 : 1;
}
