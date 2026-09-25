// TimeDelayedQueue, the merge between the RTT reader and the .rttlog / control socket / gui: a
// channel's lines never change order, nothing is lost or doubled, channels merge by target time
// within the delay, nothing waits past delay + maxHold, and the destructor hands out the rest.
#include "uc_log/TimeDelayedQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

namespace {
using Clock = std::chrono::steady_clock;

struct Line {
    int               channel{};
    std::int64_t      time{};   // the target's time: per channel, restarts at a reboot
    int               seq{};    // per channel, in the order the reader appended it
    Clock::time_point appended{};
};

struct Out {
    Line              line;
    Clock::time_point handedOut;
};

struct Collector {
    std::mutex       mutex;
    std::vector<Out> out;
};

// a queue that records what it hands out, with test-sized windows
struct Harness {
    Collector collector;
    std::optional<
      TimeDelayedQueue<Line,
                       std::int64_t (*)(Line const&),
                       int (*)(Line const&),
                       std::function<void(std::chrono::system_clock::time_point, Line const&)>>>
      queue;

    Harness(std::chrono::milliseconds delay,
            std::chrono::milliseconds maxHold) {
        queue.emplace(
          +[](Line const& l) { return l.time; },
          +[](Line const& l) { return l.channel; },
          std::function<void(std::chrono::system_clock::time_point, Line const&)>{
            [this](std::chrono::system_clock::time_point, Line const& l) {
                std::lock_guard<std::mutex> const lock{collector.mutex};
                collector.out.push_back({l, Clock::now()});
            }},
          delay,
          maxHold);
    }

    void append(Line l) {
        l.appended = Clock::now();
        queue->append(l);
    }

    std::vector<Out> finish() {
        queue.reset();   // the destructor hands out the rest
        return collector.out;
    }
};

// every appended line exactly once, and each channel's lines in the order they were appended
bool perChannelIntact(std::vector<Out> const& out,
                      std::map<int,
                               int> const&    appendedPerChannel,
                      char const*             what) {
    std::map<int, int> next;
    bool               ok = true;
    for(auto const& o : out) {
        int& expected = next[o.line.channel];
        if(o.line.seq != expected) {
            std::printf("  %s: channel %d gave #%d where #%d was next\n",
                        what,
                        o.line.channel,
                        o.line.seq,
                        expected);
            ok = false;
        }
        expected = o.line.seq + 1;
    }
    for(auto const& [channel, count] : appendedPerChannel) {
        if(next[channel] != count) {
            std::printf("  %s: channel %d handed out %d of %d\n",
                        what,
                        channel,
                        next[channel],
                        count);
            ok = false;
        }
    }
    return ok && out.size() == [&] {
        std::size_t n = 0;
        for(auto const& [c, count] : appendedPerChannel) { n += static_cast<std::size_t>(count); }
        return n;
    }();
}

// A printer start: the rings' backlog in several reads, one channel far ahead of the others.
void backlogBurst() {
    Harness            h{200ms, 1000ms};
    std::map<int, int> count;
    std::int64_t       t = 1'000'000'000;
    for(int chunk = 0; chunk != 6; ++chunk) {
        for(int channel = 0; channel != 4; ++channel) {
            for(int i = 0; i != 150; ++i) {
                // channel clocks seconds apart: target time and arrival disagree
                h.append({channel,
                          t + (3 - channel) * 10'000'000'000LL + i * 1000 + chunk * 1'000'000,
                          count[channel]++});
            }
        }
        // part of the queue is due, part not
        std::this_thread::sleep_for(60ms);
    }
    // handed out by the queue's own thread, not the destructor
    std::this_thread::sleep_for(400ms);
    auto const out = h.finish();
    CHECK(perChannelIntact(out, count, "backlog burst"),
          "backlog burst: per channel in order, all");
}

// Random reads: chunks of any size, pauses, per-channel clocks with offsets and reboots (the time
// falls back to 0), several seeds.
void randomReads() {
    for(unsigned seed = 1; seed != 9; ++seed) {
        std::mt19937                       random{seed};
        Harness                            h{20ms, 100ms};
        std::map<int, int>                 count;
        std::map<int, std::int64_t>        clock;
        std::uniform_int_distribution<int> channelOf{0, 3};
        std::uniform_int_distribution<int> chunk{1, 300};
        std::uniform_int_distribution<int> pause{0, 12};
        std::uniform_int_distribution<int> step{0, 50'000};
        std::uniform_int_distribution<int> reboot{0, 40};
        for(int c = 0; c != 4; ++c) { clock[c] = c * 5'000'000'000LL; }
        for(int read = 0; read != 120; ++read) {
            int const channel = channelOf(random);
            if(reboot(random) == 0) { clock[channel] = 0; }
            for(int i = chunk(random); i != 0; --i) {
                clock[channel] += step(random);
                h.append({channel, clock[channel], count[channel]++});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{pause(random)});
        }
        auto const out = h.finish();
        CHECK(perChannelIntact(out, count, "random reads"),
              "random reads: per channel in order, all");
    }
}

// Lines read out of target-time order across channels, all within the delay: handed out merged.
void mergesByTime() {
    Harness            h{100ms, 1000ms};
    std::map<int, int> count;
    std::mt19937       random{42};
    std::int64_t       t = 0;
    for(int round = 0; round != 10; ++round) {
        // one round: each channel's next lines, the channels read in random order
        std::vector<std::vector<Line>> perChannel(3);
        for(int i = 0; i != 60; ++i) {
            int const channel = i % 3;
            perChannel[static_cast<std::size_t>(channel)].push_back(
              {channel, t++, count[channel]++});
        }
        std::vector<int> order{0, 1, 2};
        std::ranges::shuffle(order, random);
        for(int const channel : order) {
            for(auto const& l : perChannel[static_cast<std::size_t>(channel)]) { h.append(l); }
        }
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(300ms);
    auto const out = h.finish();
    CHECK(perChannelIntact(out, count, "merge"), "merge: per channel in order, all");
    CHECK(std::ranges::is_sorted(out, {}, [](Out const& o) { return o.line.time; }),
          "merge: target time never goes back across channels");
}

// A channel whose clock is far behind (a rebooted core) must not hold another's lines forever.
void noStarvation() {
    Harness            h{20ms, 100ms};
    std::map<int, int> count;
    h.append({0, 900'000'000'000, count[0]++});   // far ahead in target time
    auto const   start = Clock::now();
    std::int64_t t     = 0;
    while(Clock::now() - start < 600ms) {   // the other channel: always a fresh, earlier line
        h.append({1, t += 1000, count[1]++});
        std::this_thread::sleep_for(2ms);
    }
    auto const out = h.finish();
    CHECK(perChannelIntact(out, count, "starvation"), "starvation: per channel in order, all");
    auto const ahead = std::ranges::find_if(out, [](Out const& o) { return o.line.channel == 0; });
    CHECK(ahead != out.end() && ahead->handedOut - ahead->line.appended < 400ms,
          "starvation: waits delay + maxHold, not until the other channel is quiet");
}

// No line waits longer than delay + maxHold + the loop's 50 ms (and some scheduling).
void boundedLatency() {
    Harness            h{20ms, 100ms};
    std::map<int, int> count;
    std::mt19937       random{7};
    for(int i = 0; i != 2000; ++i) {
        int const channel = static_cast<int>(random() % 3);
        h.append({channel, static_cast<std::int64_t>(random() % 1'000'000), count[channel]++});
        if(i % 50 == 0) { std::this_thread::sleep_for(5ms); }
    }
    std::this_thread::sleep_for(400ms);
    auto const out = h.finish();
    auto const worst
      = std::ranges::max(out, {}, [](Out const& o) { return o.handedOut - o.line.appended; });
    CHECK(perChannelIntact(out, count, "latency"), "latency: per channel in order, all");
    CHECK(worst.handedOut - worst.line.appended < 350ms, "latency: bounded");
}

// The printer stopping: whatever is queued is handed out, not dropped.
void destructorHandsOutTheRest() {
    Harness            h{200ms, 1000ms};
    std::map<int, int> count;
    for(int i = 0; i != 500; ++i) { h.append({i % 2, 1000 - i, count[i % 2]++}); }
    auto const out = h.finish();   // at once: nothing can be due yet
    CHECK(out.size() == 500, "shutdown: all 500 handed out");
    CHECK(perChannelIntact(out, count, "shutdown"), "shutdown: per channel in order");
}
}   // namespace

int main() {
    backlogBurst();
    randomReads();
    mergesByTime();
    noStarvation();
    boundedLatency();
    destructorHandsOutTheRest();
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
