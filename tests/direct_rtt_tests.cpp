// detail/DirectRtt.hpp against a simulated target: a control block in a byte array, a writer thread that fills an up
// ring as rtt.hpp does (bytes first, then the write offset), the printer's reads in pieces, and sessions that end and
// start again in the middle of the stream. Every byte arrives once, in order: a session that ends leaves what it did
// not read in the ring (the DLL's RTT took it and dropped it).
#include "uc_log/detail/Clock.hpp"
#include "uc_log/detail/DirectRtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__clang__)
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {
int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

// target RAM from 0x20000000; the block at 0x20000000, one up ring "Terminal" (size 97: not a power of two, wraps at
// odd places), one down ring "duplex" (size 31)
struct Target {
    static constexpr std::uint32_t Base     = 0x2000'0000;
    static constexpr std::uint32_t UpBuf    = Base + 0x100;
    static constexpr std::uint32_t UpSize   = 97;
    static constexpr std::uint32_t DownBuf  = Base + 0x200;
    static constexpr std::uint32_t DownSize = 31;
    static constexpr std::uint32_t UpName   = Base + 0x300;
    static constexpr std::uint32_t DownName = Base + 0x310;
    static constexpr std::uint32_t Epoch    = Base + 0x380;   // detail/RttEpoch.hpp's word
    static constexpr std::uint32_t UpDesc   = Base + 24;
    static constexpr std::uint32_t DownDesc = Base + 48;

    std::mutex                     m;   // one bus access at a time, as the probe's
    std::array<std::uint8_t, 1024> ram{};

    void put(std::uint32_t a,
             std::uint32_t v) {
        for(int i = 0; i != 4; ++i) {
            ram[a - Base + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }

    std::uint32_t get(std::uint32_t a) {
        std::uint32_t v{};
        for(int i = 0; i != 4; ++i) {
            v |= std::uint32_t{ram[a - Base + static_cast<std::uint32_t>(i)]} << (8 * i);
        }
        return v;
    }

    Target() {
        std::memcpy(ram.data(), "SEGGER RTT", 10);
        put(Base + 16, 1);
        put(Base + 20, 1);
        put(UpDesc, UpName);
        put(UpDesc + 4, UpBuf);
        put(UpDesc + 8, UpSize);
        put(DownDesc, DownName);
        put(DownDesc + 4, DownBuf);
        put(DownDesc + 8, DownSize);
        std::memcpy(&ram[UpName - Base], "Terminal", 9);
        std::memcpy(&ram[DownName - Base], "duplex", 7);
    }

    // the firmware's side of the up ring: all of `bytes` or nothing (skip mode), the offset after the data
    bool produce(std::span<std::uint8_t const> bytes) {
        std::lock_guard<std::mutex> const lock{m};
        auto const                        wr   = get(UpDesc + 12);
        auto const                        rd   = get(UpDesc + 16);
        auto const                        free = rd > wr ? rd - wr - 1 : UpSize - 1 - (wr - rd);
        if(free < bytes.size()) { return false; }
        auto pos = wr;
        for(auto const b : bytes) {
            ram[UpBuf - Base + pos] = b;
            pos                     = (pos + 1) % UpSize;
        }
        put(UpDesc + 12, pos);
        return true;
    }

    // the firmware's side of the down ring
    std::vector<std::uint8_t> consume() {
        std::lock_guard<std::mutex> const lock{m};
        auto const                        wr = get(DownDesc + 12);
        auto                              rd = get(DownDesc + 16);
        std::vector<std::uint8_t>         out;
        while(rd != wr) {
            out.push_back(ram[DownBuf - Base + rd]);
            rd = (rd + 1) % DownSize;
        }
        put(DownDesc + 16, rd);
        return out;
    }
};

struct SimTransport {
    struct Status {
        std::uint32_t numBytesTransferred{};
        std::uint32_t numBytesRead{};
        int           hostOverflowCount{};
        int           isRunning{};
        int           numUpBuffers{};
        int           numDownBuffers{};
        std::uint32_t overflowMask{};
    };

    struct BufferDesc {
        std::string name;
    };

    Target& t;
    // A boot that re-initialises the block at one step of the printer's ring access: 0 after the offsets are read,
    // 1 after the bytes are read, 2 just before the read offset is written back, 3 just after it.
    int                   rebootAtStep = -1;
    std::function<void()> reboot{};
    // the target logging after the read offset was written back (step 3), once - with the boot above: in the window
    // where the old boot's read offset sits in the new block until the pass reads the epoch
    std::function<void()> afterReadOffset{};

    void maybeReboot(int step) {
        if(step == rebootAtStep && reboot) {
            auto f = std::move(reboot);
            reboot = nullptr;
            f();
        }
    }

    void readMemory(std::uint32_t        a,
                    std::span<std::byte> out) {
        {
            std::lock_guard<std::mutex> const lock{t.m};
            if(a < Target::Base || a + out.size() > Target::Base + t.ram.size()) {
                throw std::runtime_error{"bus"};
            }
            std::memcpy(out.data(), &t.ram[a - Target::Base], out.size());
        }
        if(a <= Target::UpDesc + 12 && a + out.size() > Target::UpDesc + 12) {
            maybeReboot(0);
        }   // the offsets
        if(a >= Target::UpBuf && a < Target::UpBuf + Target::UpSize) { maybeReboot(1); }
    }

    void writeWord(std::uint32_t a,
                   std::uint32_t v) {
        bool const readOffset = a == Target::UpDesc + 16;
        if(readOffset) { maybeReboot(2); }
        {
            std::lock_guard<std::mutex> const lock{t.m};
            t.put(a, v);
        }
        if(readOffset) {
            maybeReboot(3);
            if(afterReadOffset) {
                auto f          = std::move(afterReadOffset);
                afterReadOffset = nullptr;
                f();
            }
        }
    }

    void writeMemory(std::uint32_t              a,
                     std::span<std::byte const> data) {
        std::lock_guard<std::mutex> const lock{t.m};
        std::memcpy(&t.ram[a - Target::Base], data.data(), data.size());
    }
};

using Rtt = uc_log::detail::DirectRtt<SimTransport>;

void stream() {
    Target       target;
    SimTransport bus{target};

    constexpr std::uint32_t    Total = 200'000;
    std::atomic<std::uint32_t> produced{0};
    std::thread                writer{[&] {
        std::uint32_t n = 0;
        while(n < Total) {
            // records of 1..13 bytes, each byte its running number's low byte
            std::uint32_t const       len = 1 + n % 13;
            std::vector<std::uint8_t> rec;
            for(std::uint32_t i = 0; i != len && n + i < Total; ++i) {
                rec.push_back(static_cast<std::uint8_t>(n + i));
            }
            if(target.produce(rec)) { n += static_cast<std::uint32_t>(rec.size()); }
            produced = n;
        }
    }};

    std::uint32_t             got      = 0;
    std::uint32_t             wrong    = 0;
    int                       sessions = 0;
    std::array<std::byte, 40> chunk{};
    while(got < Total) {
        // a session: open, read some passes, end (no stop call, no drain: what is not read stays)
        auto rtt = Rtt::open(bus, Target::Base, 2);
        CHECK(rtt.has_value(), "the block opens");
        if(!rtt) { break; }
        ++sessions;
        for(int pass = 0; pass != 1 + sessions % 50 && got < Total; ++pass) {
            // every other session reads as a drain does (no cap), the others as a live pass
            std::array<std::span<std::byte>, 1> const outs{std::span{chunk}};
            auto const                                data
              = rtt->readAll(bus,
                             outs,
                             sessions % 2 == 0 ? Rtt::Cap::halfWhenShared : Rtt::Cap::none)[0];
            for(auto const b : data) {
                if(std::to_integer<std::uint8_t>(b) != static_cast<std::uint8_t>(got)) { ++wrong; }
                ++got;
            }
        }
    }
    writer.join();
    CHECK(wrong == 0, "every byte in order, none twice, none missing");
    CHECK(got == Total, "all of them");
    CHECK(sessions > 100, "across many sessions that ended in the middle of the stream");
}

void namesAndDown() {
    Target       target;
    SimTransport bus{target};
    auto         rtt = Rtt::open(bus, Target::Base, 2);
    CHECK(rtt.has_value(), "opens");
    if(!rtt) { return; }
    auto const up   = rtt->bufferDesc(bus, false, 0);
    auto const down = rtt->bufferDesc(bus, true, 0);
    CHECK(up && up->name == "Terminal" && down && down->name == "duplex", "names from the target");
    std::array<std::byte, 8>                  chunk{};
    std::array<std::span<std::byte>, 1> const outs{std::span{chunk}};
    static_cast<void>(rtt->readAll(bus, outs));
    auto const s = rtt->status();
    CHECK(s.isRunning == 1 && s.numUpBuffers == 1 && s.numDownBuffers == 1,
          "status from the block");

    // the down ring takes at most size - 1, and wraps
    std::vector<std::uint8_t> sent;
    std::vector<std::uint8_t> received;
    for(int round = 0; round != 50; ++round) {
        std::array<std::byte, 20> data{};
        for(std::size_t i = 0; i != data.size(); ++i) {
            data[i] = static_cast<std::byte>(round * 20 + static_cast<int>(i));
        }
        auto const n = rtt->write(bus, 0, data);
        CHECK(n <= Target::DownSize - 1, "at most the free room");
        for(std::size_t i = 0; i != n; ++i) {
            sent.push_back(std::to_integer<std::uint8_t>(data[i]));
        }
        auto const r = target.consume();
        received.insert(received.end(), r.begin(), r.end());
    }
    CHECK(sent == received, "down bytes arrive in order");

    // a block that went (a reset cleared it): not running
    {
        std::lock_guard<std::mutex> const lock{target.m};
        target.ram[0] = 0;
    }
    static_cast<void>(rtt->readAll(bus, outs));
    CHECK(rtt->status().isRunning == 0, "a cleared block is not running");
    CHECK(!Rtt::open(bus, Target::Base, 2).has_value(), "and does not open");
    CHECK(!Rtt::open(bus, 0, 2).has_value(), "no address, no direct RTT");
}

void corruptOffsets() {
    Target       target;
    SimTransport bus{target};
    auto         rtt = Rtt::open(bus, Target::Base, 2);
    target.put(Target::UpDesc + 12,
               500);   // a write offset past the ring: a wrong block, a bus error
    bool threw = false;
    try {
        std::array<std::byte, 8>                  chunk{};
        std::array<std::span<std::byte>, 1> const outs{std::span{chunk}};
        static_cast<void>(rtt->readAll(bus, outs));
    } catch(std::runtime_error const&) { threw = true; }
    CHECK(threw, "offsets out of range end the session instead of reading outside the ring");
}

// A reset nobody announced, between the printer's read of a ring and its write of the read offset: the new boot
// re-initialises the block (offsets 0) and logs. Without the epoch the old read offset lands in the new block and the
// next read goes through the old boot's bytes again; with it the access is dropped, the offset put back to 0, and the
// new session reads the new boot from its first byte - every old byte at most once, every new byte exactly once.
void reinitInsideAnAccess() {
    for(int step = 0; step != 4; ++step) {
        for(bool const withEpoch : {false, true}) {
            Target       target;
            SimTransport bus{target};
            target.put(Target::Epoch, 6);
            // the old boot: 60 bytes 0x00..0x3B, of which the printer has read the first 20
            std::vector<std::uint8_t> old(60);
            for(std::size_t i = 0; i != old.size(); ++i) { old[i] = static_cast<std::uint8_t>(i); }
            target.produce(old);
            auto epochAt = withEpoch ? std::optional<std::uint32_t>{Target::Epoch} : std::nullopt;
            auto rtt     = Rtt::open(bus, Target::Base, 2, epochAt);
            std::vector<std::uint8_t>                 seen;
            std::array<std::byte, 20>                 chunk{};
            std::array<std::span<std::byte>, 1> const first{std::span{chunk}};
            for(auto const b : rtt->readAll(bus, first)[0]) {
                seen.push_back(std::to_integer<std::uint8_t>(b));
            }
            // the next access: the boot happens at `step` of it, and logs 30 bytes 0x80..
            bus.rebootAtStep = step;
            bus.reboot       = [&] {
                {
                    std::lock_guard<std::mutex> const lock{target.m};
                    target.put(Target::Epoch, 7);   // preMemoryInit, before .data: odd
                    target.put(Target::UpDesc + 12, 0);
                    target.put(Target::UpDesc + 16, 0);
                    target.put(Target::Epoch,
                               8);   // postMemoryInit, the block is this boot's: even
                }
                std::vector<std::uint8_t> fresh(30);
                for(std::size_t i = 0; i != fresh.size(); ++i) {
                    fresh[i] = static_cast<std::uint8_t>(0x80 + i);
                }
                target.produce(fresh);
            };
            std::array<std::byte, 40>                 big{};
            std::array<std::span<std::byte>, 1> const outs{std::span{big}};
            auto const readOnce = [&](Rtt& r) { return r.readAll(bus, outs)[0]; };
            for(auto const b : readOnce(*rtt)) { seen.push_back(std::to_integer<std::uint8_t>(b)); }
            // a new session, as the reader starts one
            auto next = Rtt::open(bus, Target::Base, 2, epochAt);
            for(int pass = 0; pass != 4; ++pass) {
                for(auto const b : readOnce(*next)) {
                    seen.push_back(std::to_integer<std::uint8_t>(b));
                }
            }
            std::size_t          oldTwice = 0, newOnce = 0;
            std::array<int, 256> count{};
            for(auto const b : seen) { ++count[b]; }
            for(std::size_t v = 0; v != 60; ++v) { oldTwice += count[v] > 1 ? 1U : 0U; }
            for(std::size_t v = 0x80; v != 0x80 + 30; ++v) { newOnce += count[v] == 1 ? 1U : 0U; }
            if(withEpoch) {
                CHECK(rtt->reinitSeen(), "the re-init is seen");
                CHECK(oldTwice == 0, "with the epoch: no old byte twice");
                CHECK(newOnce == 30, "with the epoch: every new byte exactly once");
                if(oldTwice != 0 || newOnce != 30) {
                    std::printf("  (re-init at step %d)\n", step);
                }
            } else if(step == 2) {
                CHECK(oldTwice != 0 || newOnce != 30,
                      "without the epoch, a re-init before the write-back replays");
            }
        }
    }
}

// The new boot logs while the old boot's read offset S sits in its block (written back after the boot, before the
// epoch read). Without a wrap - its write offset ends at or above S, or it only had too little room - nothing it took
// goes missing: the offset goes back to 0 and the next session reads the new boot whole. (A wrap inside that window
// loses the new boot's first bytes unnoticed; not covered.)
void logsInTheWindow() {
    struct Case {
        std::uint32_t oldRead;   // the old boot's bytes the printer read before the pass
        std::uint32_t
          oldTotal;   // the old boot's bytes in all (the pass reads the rest and writes its end back)
        std::uint32_t freshBefore;   // records the new boot logged before the write-back
        std::uint32_t inWindow;      // records it tries to log in the window
    };

    for(Case const c :
        {
          Case{ 0, 10, 10,  2},
          Case{30, 50, 14,  3},
          Case{50, 96,  1, 30}
    })
    {
        Target       target;
        SimTransport bus{target};
        target.put(Target::Epoch, 6);
        std::vector<std::uint8_t> old(c.oldTotal);
        for(std::size_t i = 0; i != old.size(); ++i) {
            old[i] = static_cast<std::uint8_t>(i % 0x70);
        }
        // the old boot's bytes, read up to oldRead (fed in pieces the ring can hold)
        auto const                epochAt = std::optional<std::uint32_t>{Target::Epoch};
        auto                      rtt     = Rtt::open(bus, Target::Base, 2, epochAt);
        std::array<std::byte, 96> chunk{};
        std::array<std::span<std::byte>, 1> const outs{std::span{chunk}};
        if(c.oldRead != 0) {
            target.produce(std::span{old}.first(c.oldRead));
            static_cast<void>(rtt->readAll(bus, outs));
        }
        target.produce(std::span{old}.subspan(c.oldRead));
        std::uint32_t taken = 0, dropped = 0, value = 0x80;
        auto const    log = [&](std::uint32_t n) {
            for(std::uint32_t i = 0; i != n; ++i, ++value) {
                std::array<std::uint8_t, 4> const rec{static_cast<std::uint8_t>(value),
                                                      static_cast<std::uint8_t>(value),
                                                      static_cast<std::uint8_t>(value),
                                                      static_cast<std::uint8_t>(value)};
                if(target.produce(rec)) {
                    ++taken;
                } else {
                    ++dropped;
                }
            }
        };
        bus.rebootAtStep
          = 1;   // after the pass read the old bytes, before it writes their end back
        bus.reboot = [&] {
            {
                std::lock_guard<std::mutex> const lock{target.m};
                target.put(Target::Epoch, 7);
                target.put(Target::UpDesc + 12, 0);
                target.put(Target::UpDesc + 16, 0);
                target.put(Target::Epoch, 8);
            }
            log(c.freshBefore);
        };
        bus.afterReadOffset = [&] { log(c.inWindow); };
        static_cast<void>(rtt->readAll(bus, outs));
        CHECK(rtt->reinitSeen(), "window: the re-init is seen");
        auto                      next = Rtt::open(bus, Target::Base, 2, epochAt);
        std::vector<std::uint8_t> seen;
        for(int pass = 0; pass != 6; ++pass) {
            for(auto const b : next->readAll(bus, outs)[0]) {
                seen.push_back(std::to_integer<std::uint8_t>(b));
            }
        }
        // the records as the target stored them: runs of 4 equal bytes from 0x80 on
        std::array<int, 256> count{};
        bool                 torn = seen.size() % 4 != 0;
        for(std::size_t i = 0; i + 3 < seen.size(); i += 4) {
            if(seen[i] != seen[i + 1] || seen[i] != seen[i + 2] || seen[i] != seen[i + 3]
               || seen[i] < 0x80)
            {
                torn = true;
            }
            ++count[seen[i]];
        }
        std::uint32_t arrived = 0, twice = 0;
        for(std::uint32_t v = 0x80; v != value; ++v) {
            arrived += count[v] >= 1 ? 1U : 0U;
            twice += count[v] > 1 ? 1U : 0U;
        }
        bool const lost = torn || twice != 0 || arrived != taken;
        if(lost) {
            std::printf(
              "  window case old %u/%u fresh %u+%u: taken %u dropped %u arrived %u twice %u torn "
              "%d\n",
              c.oldRead,
              c.oldTotal,
              c.freshBefore,
              c.inWindow,
              taken,
              dropped,
              arrived,
              twice,
              torn ? 1 : 0);
        }
        CHECK(!lost, "window, no wrap: every record the target took arrives once, intact");
    }
}

// A reset nobody announced BETWEEN two passes of readAll (it reads the RTT epoch only after a pass): the pass after the
// boot reads the new boot's bytes from the re-initialised block. None of them may come out of the old session,
// its read offset goes back to 0, and the next session reads the new boot from its first byte - every old byte at most
// once, every new byte exactly once. A new boot that has written nothing yet: the pass reads nothing, ends nothing.
void reinitBetweenPasses() {
    for(std::size_t const fresh : {std::size_t{0}, std::size_t{30}}) {
        Target       target;
        SimTransport bus{target};
        target.put(Target::Epoch, 6);
        std::vector<std::uint8_t> old(60);
        for(std::size_t i = 0; i != old.size(); ++i) { old[i] = static_cast<std::uint8_t>(i); }
        target.produce(old);
        auto const                epochAt = std::optional<std::uint32_t>{Target::Epoch};
        auto                      rtt     = Rtt::open(bus, Target::Base, 2, epochAt);
        std::array<std::byte, 20> small{};
        std::array<std::byte, 40> big{};
        std::array<std::span<std::byte>, 1> const first{std::span{small}};
        std::array<std::span<std::byte>, 1> const outs{std::span{big}};
        std::vector<std::uint8_t>                 seen;
        for(auto const b : rtt->readAll(bus, first)[0]) {
            seen.push_back(std::to_integer<std::uint8_t>(b));
        }
        CHECK(seen.size() == 20 && !rtt->reinitSeen(), "the old boot's first 20 bytes");
        {   // the boot, between the passes: the epoch odd (preMemoryInit), the block (.data), the epoch even, its lines
            std::lock_guard<std::mutex> const lock{target.m};
            target.put(Target::Epoch, 7);
            target.put(Target::UpDesc + 12, 0);
            target.put(Target::UpDesc + 16, 0);
            target.put(Target::Epoch, 8);
        }
        std::vector<std::uint8_t> lines(fresh);
        for(std::size_t i = 0; i != lines.size(); ++i) {
            lines[i] = static_cast<std::uint8_t>(0x80 + i);
        }
        target.produce(lines);
        CHECK(rtt->readAll(bus, outs)[0].empty(),
              "no byte of the new boot comes out of the old session");
        CHECK(rtt->reinitSeen() == (fresh != 0),
              "the boot is seen by the pass that read its bytes, by none before");
        CHECK(target.get(Target::UpDesc + 16) == 0,
              "the read offset is back at the new block's first byte");
        auto next = Rtt::open(bus, Target::Base, 2, epochAt);
        for(int pass = 0; pass != 4; ++pass) {
            for(auto const b : next->readAll(bus, outs)[0]) {
                seen.push_back(std::to_integer<std::uint8_t>(b));
            }
        }
        std::array<int, 256> count{};
        for(auto const b : seen) { ++count[b]; }
        std::size_t oldTwice = 0, newOnce = 0;
        for(std::size_t v = 0; v != 60; ++v) { oldTwice += count[v] > 1 ? 1U : 0U; }
        for(std::size_t v = 0x80; v != 0x80 + fresh; ++v) { newOnce += count[v] == 1 ? 1U : 0U; }
        CHECK(oldTwice == 0, "between passes: no old byte twice");
        CHECK(newOnce == fresh, "between passes: every new byte exactly once");
        CHECK(!next->reinitSeen(), "the next session reads the new boot as its own");
    }
}

// An odd epoch: a boot is re-initialising the block (RttEpoch.hpp). open() waits for it to turn even (a .data copy is
// short) and reads the new block from its first byte; odd for good (an image without the init fence, a boot that died
// in its init) - after a bounded wait it skips what the rings hold, never reading from the middle of a record, says how
// much, and reads what comes after.
void oddEpoch() {
    {   // a boot in its init: odd for a few ms, then the new block
        Target       target;
        SimTransport bus{target};
        target.put(Target::Epoch, 7);
        std::vector<std::uint8_t> old(20, 0x11);
        target.produce(old);   // the ended boot's tail, still in the old block
        uc_log::detail::Clock::Thread boot{[&] {
            uc_log::detail::Clock::sleep_for(std::chrono::milliseconds{30});
            std::lock_guard<std::mutex> const lock{target.m};
            target.put(Target::UpDesc + 12, 0);
            target.put(Target::UpDesc + 16, 0);
            target.put(Target::Epoch, 8);
        }};
        auto rtt = Rtt::open(bus, Target::Base, 2, std::optional<std::uint32_t>{Target::Epoch});
        boot.join();
        CHECK(rtt && rtt->sessionEpoch() == 8 && !rtt->staleSkipped(),
              "odd epoch: open waits for the new block");
        std::vector<std::uint8_t> fresh(10, 0x22);
        target.produce(fresh);
        std::array<std::byte, 40>                 big{};
        std::array<std::span<std::byte>, 1> const outs{std::span{big}};
        auto const                                got = rtt->readAll(bus, outs)[0];
        CHECK(got.size() == 10
                && std::ranges::all_of(got, [](std::byte b) { return b == std::byte{0x22}; }),
              "odd epoch: the new boot's bytes, none of the old tail");
    }
    {   // odd for good
        Target       target;
        SimTransport bus{target};
        target.put(Target::Epoch, 9);
        std::vector<std::uint8_t> old(20, 0x11);
        target.produce(old);
        auto const start = uc_log::detail::Clock::now();
        auto rtt = Rtt::open(bus, Target::Base, 2, std::optional<std::uint32_t>{Target::Epoch});
        auto const took = uc_log::detail::Clock::now() - start;
        CHECK(rtt && took < std::chrono::seconds{1}, "odd for good: open does not hang");
        CHECK(rtt && rtt->staleSkipped() == 20U,
              "odd for good: the rings' 20 bytes skipped and counted");
        CHECK(target.get(Target::UpDesc + 16) == target.get(Target::UpDesc + 12),
              "odd for good: read offset at write offset");
        std::vector<std::uint8_t> fresh(10, 0x22);
        target.produce(fresh);
        std::array<std::byte, 40>                 big{};
        std::array<std::span<std::byte>, 1> const outs{std::span{big}};
        auto const                                got = rtt->readAll(bus, outs)[0];
        CHECK(got.size() == 10
                && std::ranges::all_of(got, [](std::byte b) { return b == std::byte{0x22}; }),
              "odd for good: what comes after is read whole");
    }
}

// detail/RttControlBlock.hpp + Le32.hpp: a block written word by word as rtt.hpp lays it out, read back field by field;
// the ring arithmetic at its edges; an id that is damaged or cut short.
void controlBlockLayout() {
    namespace rb = uc_log::detail::rtt_block;
    std::vector<std::byte> raw(rb::bytesFor(2));
    auto const             put = [&](std::size_t at, std::uint32_t v) {
        for(std::size_t i = 0; i != 4; ++i) { raw[at + i] = static_cast<std::byte>(v >> (8U * i)); }
    };
    std::memcpy(raw.data(), "SEGGER RTT", 10);
    put(16, 1);   // up
    put(20, 1);   // down
    for(std::uint32_t b = 0; b != 2; ++b) {
        auto const at = rb::descriptorAt(b);
        put(at, 0x100 + b);       // name
        put(at + 4, 0x200 + b);   // buffer
        put(at + 8, 0x300 + b);   // size
        put(at + 12, 0x10 + b);   // write offset
        put(at + 16, 0x20 + b);   // read offset
        put(at + 20, 0x30 + b);   // flags
    }
    rb::View const block{raw};
    CHECK(block.hasId() && block.printableId() == "SEGGER RTT", "the id");
    CHECK(block.numUp() == 1 && block.numDown() == 1, "the buffer counts");
    CHECK(block.descriptorsRead() == 2, "two descriptors read");
    auto const d = block.descriptor(1);
    CHECK(d.name == 0x101 && d.buffer == 0x201 && d.size == 0x301 && d.writeOffset == 0x11
            && d.readOffset == 0x21 && d.flags == 0x31,
          "every field of the second descriptor in its place");
    CHECK(rb::descriptorAt(1) + rb::ReadOffsetField == 24 + 24 + 16, "the read offset's address");
    CHECK(uc_log::detail::le32(raw, rb::descriptorAt(0) + rb::WriteOffsetField) == 0x10,
          "the write offset's place");
    rb::Descriptor const writeAtSize{.name        = 0,
                                     .buffer      = 0,
                                     .size        = 16,
                                     .writeOffset = 16,
                                     .readOffset  = 0,
                                     .flags       = 0};
    rb::Descriptor const noSize{.name        = 0,
                                .buffer      = 0,
                                .size        = 0,
                                .writeOffset = 0,
                                .readOffset  = 0,
                                .flags       = 0};
    CHECK(d.offsetsInRange() && !writeAtSize.offsetsInRange() && !noSize.offsetsInRange(),
          "offsets in range: both below the size, and a size");

    CHECK(rb::filled(16, 5, 5) == 0 && rb::filled(16, 7, 5) == 2 && rb::filled(16, 3, 5) == 14,
          "filled");
    CHECK(rb::space(16, 5, 5) == 15 && rb::space(16, 4, 5) == 0 && rb::space(16, 15, 0) == 0
            && rb::space(16, 0, 15) == 14,
          "space keeps one slot free");

    raw[3] = std::byte{0x01};
    CHECK(!rb::View{raw}.hasId() && rb::View{raw}.printableId() == "SEG?ER RTT", "a damaged id");
    std::memcpy(raw.data(), "SEGGER RTT", 10);
    CHECK(rb::View{std::span{raw}.first(10)}.hasId(),
          "the id's bytes alone are enough for hasId()");
    CHECK(!rb::View{std::span{raw}.first(9)}.hasId(), "nine bytes are not an id");
    CHECK(rb::View{std::span{raw}.first(24 + 23)}.descriptorsRead() == 0,
          "a cut descriptor is not read");
}

// Every place the ring's end can fall: each start offset of the 97-byte ring x each record length 1..96 x read chunks
// of 1, 7, 40, 96 and 97 bytes x both caps x two writer habits (fill the ring, then read it in passes; one record a
// pass). Deterministic, one thread: the bytes of every record the target took arrive once, in order, the read offset
// ends where the reads did, and a ring the printer emptied always takes the next record.
void wrapSweep() {
    constexpr std::array<std::size_t, 5> Chunks{1, 7, 40, 96, 97};
    std::size_t                          cases = 0, bad = 0;
    for(std::uint32_t start = 0; start != Target::UpSize; ++start) {
        for(std::uint32_t len = 1; len != Target::UpSize; ++len) {
            for(auto const chunkSize : Chunks) {
                for(auto const cap : {Rtt::Cap::halfWhenShared, Rtt::Cap::none}) {
                    for(int habit = 0; habit != 2; ++habit) {
                        Target       target;
                        SimTransport bus{target};
                        target.put(Target::UpDesc + 12, start);
                        target.put(Target::UpDesc + 16, start);
                        auto rtt = Rtt::open(bus, Target::Base, 2);
                        if(!rtt) {
                            ++bad;
                            continue;
                        }
                        // records whose bytes count on from 0; 3 ring laps of them
                        std::uint32_t const    total = 3 * Target::UpSize;
                        std::uint32_t          next = 0, got = 0, wrong = 0;
                        std::vector<std::byte> chunk(chunkSize);
                        bool                   refusedWhileEmpty = false;
                        std::uint32_t          passes            = 0;
                        auto const             produceOne        = [&]() {
                            std::vector<std::uint8_t> rec(len);
                            for(std::uint32_t i = 0; i != len; ++i) {
                                rec[i] = static_cast<std::uint8_t>(next + i);
                            }
                            bool const empty
                              = target.get(Target::UpDesc + 12) == target.get(Target::UpDesc + 16);
                            if(target.produce(rec)) {
                                next += len;
                                return true;
                            }
                            refusedWhileEmpty = refusedWhileEmpty || empty;
                            return false;
                        };
                        auto const readPass = [&]() {
                            std::array<std::span<std::byte>, 1> const outs{std::span{chunk}};
                            auto const data = rtt->readAll(bus, outs, cap)[0];
                            for(auto const b : data) {
                                if(std::to_integer<std::uint8_t>(b)
                                   != static_cast<std::uint8_t>(got))
                                {
                                    ++wrong;
                                }
                                ++got;
                            }
                            ++passes;
                            return data.size();
                        };
                        while(next < total && passes < 10 * total) {
                            if(habit == 0) {
                                while(next < total && produceOne()) {}
                                while(readPass() != 0) {}
                            } else {
                                produceOne();
                                readPass();
                            }
                        }
                        while(readPass() != 0) {}
                        ++cases;
                        auto const wr = target.get(Target::UpDesc + 12);
                        auto const rd = target.get(Target::UpDesc + 16);
                        bool const ok = wrong == 0 && got == next && next >= total && rd == wr
                                     && rd == (start + next) % Target::UpSize && !refusedWhileEmpty;
                        if(!ok && bad++ < 3) {
                            std::printf(
                              "  wrap: start %u len %u chunk %zu cap %d habit %d: got %u of %u, "
                              "wrong %u, rd %u wr %u, refused while empty %d\n",
                              start,
                              len,
                              chunkSize,
                              cap == Rtt::Cap::none ? 1 : 0,
                              habit,
                              got,
                              next,
                              wrong,
                              rd,
                              wr,
                              refusedWhileEmpty ? 1 : 0);
                        }
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "wrap sweep: every byte once, in order, the read offset where the reads ended");
    std::printf("wrap sweep: %zu cases\n", cases);
}
}   // namespace

int main() {
    // EpochSettle and the boot below on the virtual clock (the stream's writer is a target: it never waits on it)
    uc_log::detail::VirtualTime const virtualTime;
    wrapSweep();
    controlBlockLayout();
    reinitInsideAnAccess();
    reinitBetweenPasses();
    logsInTheWindow();
    oddEpoch();
    stream();
    namesAndDown();
    corruptOffsets();
    std::printf(failures == 0 ? "ALL OK\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
