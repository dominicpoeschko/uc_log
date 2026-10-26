#pragma once

// rtt_reader tests: frames decoded end to end, a quiet target, overflows and bad host offsets, reconnects and the end
// of a session. Included only by rtt_reader_tests.cpp (one binary for every rtt_reader case); the fake J-Link and the
// shared pieces: support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::decode {

inline void framesDecode() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // frames decode end to end
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");
}

inline void quietTargetOnlyWarns() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    reader.setNoLogTimeout(1);
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");

    // a quiet-but-alive target must not reconnect (the old code tore down every 15 s);
    // it must only warn
    CHECK(waitFor([&]() { return collected.anyMessageContains("no log messages"); }, 4000ms),
          "quiet warning emitted");
    CHECK(shared.constructions.load() == 1, "no reconnect while the link is alive");
}

inline void overflowReported() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // a running session first: what it read at its start is the baseline the change below is seen against
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");
    {
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.status.hostOverflowCount += 2;
        }
        CHECK(waitFor([&]() { return collected.anyErrorContains("RTT host overflow"); }, 3000ms),
              "overflow told as a status error");
        // the fake memory holds no "SEGGER RTT" id
        CHECK(
          waitFor([&]() { return collected.anyErrorContains("RTT control block at 0x20000100"); },
                  3000ms),
          "control block dump follows the data loss");
        CHECK(waitFor([&]() { return collected.anyErrorContains("CORRUPT"); }, 3000ms),
              "a block without the RTT id is corrupt");
        CHECK(!collected.anyEntryContains("RTT host overflow")
                && !collected.anyEntryContains("RTT control block"),
              "neither of them is an entry of the log");
        std::size_t before = 0;
        {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            before
              = static_cast<std::size_t>(std::ranges::count_if(collected.errors, [](auto const& e) {
                    return e.find("RTT host overflow") != std::string::npos;
                }));
        }
        for(int i = 0; i != 20; ++i) {
            {
                std::scoped_lock<std::mutex> const lock{shared.mutex};
                shared.status.hostOverflowCount += 1;
            }
            Clock::sleep_for(10ms);
        }
        {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            auto const                         after
              = static_cast<std::size_t>(std::ranges::count_if(collected.errors, [](auto const& e) {
                    return e.find("RTT host overflow") != std::string::npos;
                }));
            CHECK(after == before, "a running overflow is summarised, not repeated");
        }
    }
}

inline void hostOffsetOutOfRange() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // a running session first: what it read at its start is the baseline the change below is seen against
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");
    // the host's offset in the control block out of range (the channel would be dead for good): set
    // to the target's offset at the next loss
    {
        auto const block = [](std::uint32_t wr, std::uint32_t rd) {
            return std::array<std::uint32_t, 12>{0x47474553U,   // "SEGGER RTT\0..."
                                                 0x52205245U,
                                                 0x00005454U,
                                                 0U,
                                                 1U,   // up
                                                 0U,   // down
                                                 0U,   // name
                                                 0x20000400U,
                                                 1024U,
                                                 wr,
                                                 rd,
                                                 0U};
        };
        auto const loss = [&]() {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.status.hostOverflowCount += 1;
        };
        auto const word = [&](std::size_t i) {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            return (*shared.controlBlock)[i];
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.controlBlock = block(334, 3222675632U);
        }
        loss();
        CHECK(waitFor([&]() { return word(10) == 334; }, 3000ms),
              "an up buffer's read offset out of range is set to the write offset");
        // said after the write: waited for, not looked at once
        CHECK(waitFor(
                [&]() {
                    return collected.anyErrorContains(
                      "the host's read offset was 3222675632 in a 1024-byte ring");
                },
                3000ms),
              "and said so");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.controlBlock = block(5000, 7);   // the target's own offset: its RAM, not ours
        }
        loss();
        Clock::sleep_for(300ms);
        CHECK(word(9) == 5000 && word(10) == 7, "a target-owned offset out of range is left alone");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.controlBlock.reset();
        }
    }
}

inline void lostConnectionReconnects() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    CHECK(waitFor([&]() { return reader.sessionCount() >= 1; }, 3000ms), "a session runs first");
    auto const built = shared.constructions.load();
    // a lost connection must reconnect
    {
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = false;
        }
        Clock::sleep_for(100ms);
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return shared.constructions.load() >= built + 1; }, 5000ms),
              "reconnected after connection loss");
        // counted before the frame goes in: the reader may decode it before the next statement runs
        auto const before = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return collected.entries.size();
        }();
        shared.feed(makeFrame(0));
        CHECK(waitFor(
                [&]() {
                    std::scoped_lock<std::mutex> const lock{collected.mutex};
                    return collected.entries.size() > before;
                },
                3000ms),
              "decoding works again after reconnect");
    }
}

inline void restartedCountersAreNotAWrap() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // the DLL's counters above zero first: a frame read
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");
    // byte counters that went down (restarted by the DLL) are not a wrap to ~4 G
    {
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.status.numBytesTransferred = 0;
            shared.status.numBytesRead        = 0;
            shared.status.hostOverflowCount += 1;
        }
        CHECK(waitFor(
                [&]() {
                    return collected.anyErrorContains(
                      "(1 event; since the last report 0 bytes from the target, 0 read");
                },
                3000ms),
              "a restarted counter counts from zero");
        CHECK(!collected.anyErrorContains("42949"), "no wrapped byte count");
    }
}

inline void stopDecodesHeldLines() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    CHECK(waitFor([&]() { return reader.sessionCount() >= 1; }, 3000ms), "a session runs first");
    // The printer stops while the DLL holds lines it read from the target: they are decoded, not dropped (the
    // target's read offset has moved past them, so nobody else would ever see them). Without this ~10 ms of
    // every channel went missing at each printer restart.
    {
        auto const before = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return collected.entries.size();
        }();
        shared.holdUntilRttStop = true;
        shared.feed(makeFrame(0));
        Clock::sleep_for(50ms);   // passes of the loop that see nothing
        under.reader.reset();     // the printer stops
        std::scoped_lock<std::mutex> const lock{collected.mutex};
        CHECK(collected.entries.size() == before + 1,
              "the line the DLL held at the stop is decoded");
    }
}

// The printer's own RTT reader at the end of a session: one read per ring, up to the write offset - never a cut
// inside a record (the stopping session would drop its head, the next one start on its tail: "corrupted data"
// and an unannounced loss), and never a drain that goes on while the target floods (a stop that outlasted its
// caller's wait). A target that floods a 1 KiB ring with whole 4-byte records; every probe read 1 ms.
inline void directReaderStopsBetweenRecords() {
    Scenario s;
    auto&    shared = s.shared;
    {
        static constexpr std::uint32_t block = FakeTransport::Shared::ramBase;
        RamTarget                      target{shared, 0x800, oneKiBRing};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 1ms;
        }
        // the target fills the ring with every record that fits as soon as the printer frees room (its read offset
        // write) - a flood that does not depend on a thread getting the CPU
        std::uint64_t produced{0};                           // shared.mutex
        auto const    fill = [&, frame = makeFrame(0)]() {   // shared.mutex held
            produced += target.rtt.fillWith(0, frame);
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            fill();
            shared.afterRamWrite = fill;
        }
        Collected  direct;
        auto const startReader = [&]() {
            return makeReader(direct,
                              {.block = []() { return uc_log::printer::RttBlockInfo{block, 1}; },
                               .catalogText = "hello direct"});
        };
        auto const entries = [&]() {
            std::scoped_lock<std::mutex> const lock{direct.mutex};
            return direct.entries.size();
        };
        for(int session = 0; session != 5; ++session) {
            auto       r     = startReader();
            auto const start = entries();
            CHECK(waitFor([&]() { return entries() > start + 500; }, 5000ms),
                  "the printer's own reader decodes");
            auto const t0 = Clock::now();
            r.reset();   // the printer stops while the target floods on
            auto const took = Clock::now() - t0;
            CHECK(took < 100ms, "a stop under a flood ends after one read per ring");
            if(took >= 100ms) {
                std::printf("  stop took %lld ms\n",
                            static_cast<long long>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
            }
        }
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.afterRamWrite = nullptr;   // the flood ends
        }
        {
            auto r = startReader();   // the rest of the ring, then quiet
            Clock::sleep_for(300ms);
            r.reset();
        }
        CHECK(!direct.anyErrorContains("corrupted") && !direct.anyErrorContains("resync"),
              "no session started inside a record");
        CHECK(!direct.anyMessageContains("unfinished"), "no session ended inside a record");
        CHECK(waitFor([&]() { return direct.anyToolContains("RTT read by the printer"); }, 3000ms),
              "the printer's own reader was used");
        std::scoped_lock<std::mutex> const lock{direct.mutex};
        CHECK(direct.entries.size() == produced, "every record exactly once across the sessions");
        if(direct.entries.size() != produced) {
            std::printf("  decoded %zu, produced %llu\n",
                        direct.entries.size(),
                        static_cast<unsigned long long>(produced));
        }
    }
}

inline void silentAcrossConnections() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    for(bool const lineFrees : {true, false}) {
        {
            std::scoped_lock<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        shared.resetLineResets    = 0;
        shared.resetLineFrees     = lineFrees;
        shared.deadAcrossConnects = true;
        shared.apDead             = true;
        shared.blockGone          = true;   // a wait for the block, as after a reset
        auto r                    = start(0);
        CHECK(waitFor([&]() { return quiet.anyErrorContains("no answer from the target for 3 s"); },
                      8000ms),
              "silent: said within seconds");
        CHECK(waitFor([&]() { return shared.resetLineResets.load() >= 1; }, 20000ms),
              "silent: the reset line after reconnects");
        if(lineFrees) {
            CHECK(waitFor([&]() { return !shared.apDead.load(); }, 2000ms),
                  "silent: the reset line freed it");
        } else {
            CHECK(waitFor([&]() { return quiet.anyErrorContains("power-cycle it"); }, 12000ms),
                  "silent: a power cycle asked for, and it keeps trying");
        }
        shared.deadAcrossConnects = false;
        shared.apDead             = false;
        shared.blockGone          = false;
    }
}

// a stop while the decoder lags behind the reader (a slow consumer of the decoded lines, a flood) - every
// record already read is still decoded, and the reader still ends promptly. Before, the decoder honoured the stop:
// the rest piled up undecoded and the oversized-buffer resync ground through it for minutes.
inline void stopWithDecoderBacklog() {
    Scenario s;
    auto&    shared = s.shared;
    {
        constexpr std::uint32_t ringSize = 1024;
        constexpr std::uint32_t desc     = FakeTransport::Shared::ramBase + rb::descriptorAt(0);
        RamTarget               target{shared, 0x800, oneKiBRing};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
        }
        // the target refills the ring whenever the printer frees room (as in the flood above)
        std::uint64_t produced{0};                           // shared.mutex
        auto const    fill = [&, frame = makeFrame(0)]() {   // shared.mutex held
            produced += target.rtt.fillWith(0, frame);
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            fill();
            shared.afterRamWrite = fill;
        }
        std::atomic<std::uint64_t> decoded{0};
        Collected                  said;
        auto                       r = makeReader(
          said,
          {.block
           = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
           .catalogText = "hello direct",
           .onEntry =
             [&decoded]() {
                 Clock::sleep_for(20us);   // a slow consumer: the decoder lags behind
                 ++decoded;
             }});
        Clock::sleep_for(500ms);
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.afterRamWrite = nullptr;   // the flood ends
        }
        auto const t0 = Clock::now();
        r.reset();   // the stop
        auto const took = Clock::now() - t0;
        CHECK(took < 3000ms, "the reader ends promptly with a backlog");
        CHECK(decoded.load() != 0, "the flood was read and decoded: the backlog case is not empty");
        if(took >= 3000ms) {
            std::printf("  the stop took %lld ms\n",
                        static_cast<long long>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
        }
        // what the stopped session read was decoded whole; the rest is still in the ring (read by the next session)
        std::uint64_t left{};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            auto const                         wr = shared.getRam(desc + 12);
            auto const                         rd = shared.getRam(desc + 16);
            left                                  = (wr >= rd ? wr - rd : ringSize - rd + wr) / 4;
        }
        CHECK(decoded + left == produced,
              "every record read was decoded, every other one is still in the ring");
        if(decoded + left != produced) {
            std::printf("  decoded %llu + left %llu != produced %llu\n",
                        static_cast<unsigned long long>(decoded.load()),
                        static_cast<unsigned long long>(left),
                        static_cast<unsigned long long>(produced));
        }
    }
}

// the probe is lost in the middle of a pass (a J-Link that drops off USB): readAll() has read ring 0 and moved
// its read offset in the target, then the read of ring 1 throws. What the dying session read must still be
// decoded, and nothing may be left damaged for the next session - before, ring 0's bytes died with the exception
// (an unannounced loss of up to a ring) and the next session began inside a record ("corrupted data").
inline void probeLostMidPass() {
    Scenario s;
    auto&    shared = s.shared;
    {
        // 1030 bytes: the per-pass cap (half a ring while both rings hold data) is 515 - reads end inside 4-byte records,
        // so the dying session also holds the head of a cut record
        constexpr std::uint32_t           ringSize = 1030;
        constexpr std::uint32_t           block    = FakeTransport::Shared::ramBase;
        constexpr std::uint32_t           desc0    = block + 24;
        constexpr std::uint32_t           desc1    = desc0 + 24;
        constexpr std::uint32_t           ring0    = block + 0x100;
        constexpr std::uint32_t           ring1    = ring0 + ringSize;
        constexpr std::array<RingSpec, 2> rings{
          RingSpec{.buffer = ring0, .size = ringSize},
          RingSpec{.buffer = ring1, .size = ringSize}
        };
        RamTarget target{shared, 0x100 + 2 * ringSize, rings};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
            shared.putRam(
              block + 0xF0,
              8);   // the image's RTT epoch: constant (even: the block is ready), no boot here
            shared.connected = false;   // no session before the epoch source is set (below)
        }
        // the target refills both rings whenever the printer frees room (as in the flood above); a read is a probe
        // round trip, so the reader's passes take time while the rings never empty
        std::uint64_t produced{0};                           // shared.mutex
        auto const    fill = [&, frame = makeFrame(0)]() {   // shared.mutex held
            produced += target.rtt.fillWith(0, frame);
            produced += target.rtt.fillWith(1, frame);
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 100us;
            fill();
            shared.afterRamWrite = fill;
        }
        std::atomic<std::uint64_t> decoded{0};
        Collected                  lost;
        auto                       r = makeReader(
          lost,
          {.block       = []() { return uc_log::printer::RttBlockInfo{block, 2}; },
           .catalogText = "hello direct",
           .onEntry     = [&decoded]() { ++decoded; },
           .image = {.rttEpoch = []() { return std::optional<std::uint32_t>{block + 0xF0}; }}});
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        Clock::sleep_for(1500ms);   // the first session up (a connect is retried every second)
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.failReadFrom = ring1;   // the probe goes while ring 1 is read, after ring 0 was
            shared.failReadTo   = ring1 + ringSize;
        }
        Clock::sleep_for(800ms);   // a new session
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.afterRamWrite = nullptr;   // the flood ends
        }
        waitFor(
          [&]() {
              std::scoped_lock<std::mutex> const lock{shared.mutex};
              return shared.getRam(desc0 + 12) == shared.getRam(desc0 + 16)
                  && shared.getRam(desc1 + 12) == shared.getRam(desc1 + 16);
          },
          3000ms);
        Clock::sleep_for(200ms);
        r.reset();
        bool failed{};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            failed = shared.failReadTo == 0;
        }
        CHECK(failed, "the probe failure happened");
        CHECK(!lost.anyErrorContains("corrupted"), "no damaged frame after the probe came back");
        CHECK(decoded == produced,
              "every record of both rings is decoded - none died with the session");
        if(decoded != produced) {
            std::printf("  decoded %llu != produced %llu\n",
                        static_cast<unsigned long long>(decoded.load()),
                        static_cast<unsigned long long>(produced));
        }
    }
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{                   "framesDecode",                    framesDecode},
  uc_log::test::Case{           "quietTargetOnlyWarns",            quietTargetOnlyWarns},
  uc_log::test::Case{               "overflowReported",                overflowReported},
  uc_log::test::Case{           "hostOffsetOutOfRange",            hostOffsetOutOfRange},
  uc_log::test::Case{       "lostConnectionReconnects",        lostConnectionReconnects},
  uc_log::test::Case{   "restartedCountersAreNotAWrap",    restartedCountersAreNotAWrap},
  uc_log::test::Case{           "stopDecodesHeldLines",            stopDecodesHeldLines},
  uc_log::test::Case{"directReaderStopsBetweenRecords", directReaderStopsBetweenRecords},
  uc_log::test::Case{        "silentAcrossConnections",         silentAcrossConnections},
  uc_log::test::Case{         "stopWithDecoderBacklog",          stopWithDecoderBacklog},
  uc_log::test::Case{               "probeLostMidPass",                probeLostMidPass},
};

}   // namespace uc_log_test_rtt_reader::decode
