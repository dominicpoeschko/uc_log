#pragma once

// rtt_reader tests: resets and flashes, outside resets, announced resets and their late answers. Included only by
// rtt_reader_tests.cpp (one binary for every rtt_reader case); the fake J-Link and the shared pieces:
// support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::resets {

inline void resetsAndFlashes() {
    Scenario                              s;
    auto&                                 shared = s.shared;
    std::vector<JLink::MemoryWrite> const commands{
      JLink::MemoryWrite{0x4001A004, 0x01000000},
      JLink::MemoryWrite{0x4001B004, 0x01000000}
    };
    {
        std::atomic<int>      sessionStarts{0};
        std::atomic<unsigned> sessionByte{0};
        auto const            sessionStart = [&](uc_log::printer::DirectMemoryRead const& read) {
            std::array<std::byte, 1> b{};
            if(read(0x20000042, b)) { sessionByte = std::to_integer<unsigned>(b[0]); }
            ++sessionStarts;
        };
        ReaderUnderTest under{
          {.image = {.onSessionStart = sessionStart}, .options = {.preResetCommands = commands}}
        };
        [[maybe_unused]] Reader&    reader    = *under.reader;
        [[maybe_unused]] Collected& collected = under.said;
        std::atomic<std::uint64_t>  sessionAtReset{0};
        shared.onReset    = [&]() { sessionAtReset = reader.sessionCount(); };
        auto const before = reader.sessionCount();
        auto const r      = reader.resetAndWait(5000ms);
        CHECK(r.has_value() && shared.resets.load() >= 1 && reader.sessionCount() > before,
              "resetAndWait: a new session");
        CHECK(reader.sessionCount() > sessionAtReset.load(),
              "resetAndWait returns with a session begun after the reset");

        // a reset requested while a session is starting: that session is not the reset's
        shared.holdSessionStart = true;
        auto const built        = shared.constructions.load();
        reader.resetJLink();
        CHECK(waitFor([&]() { return shared.constructions.load() > built; }, 3000ms),
              "a session is starting");
        auto const                       resetsBefore = shared.resets.load();
        std::expected<void, std::string> during{std::unexpected{std::string{"not run"}}};
        Clock::Thread                    requester{[&]() { during = reader.resetAndWait(5000ms); }};
        Clock::sleep_for(50ms);
        shared.holdSessionStart = false;
        requester.join();
        CHECK(during.has_value() && shared.resets.load() > resetsBefore,
              "resetAndWait during a session start waits for its reset");
        CHECK(reader.sessionCount() > sessionAtReset.load(), "and for a session begun after it");

        // through the reset line ("reset" with "line": true): that one reset by one pulse of the pin (not J-Link
        // reset type 2, whose strategy pulses an RP2350's RUN three times), the next ordinary reset by the configured
        // type again
        auto const lineBefore  = shared.resetLineResets.load();
        auto const pulseBefore = shared.resetLinePulses.load();
        auto const viaLine     = reader.resetAndWait(5000ms, {}, true);
        CHECK(viaLine.has_value() && shared.resetLineResets.load() == lineBefore + 1
                && shared.resetLinePulses.load() == pulseBefore + 1,
              "resetAndWait through the reset line: one pulse of the pin");
        auto const plain = reader.resetAndWait(5000ms);
        CHECK(plain.has_value() && shared.resetLineResets.load() == lineBefore + 1,
              "the reset after it is the ordinary one again");
        shared.onReset = nullptr;
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            CHECK(shared.preResetAtLastReset == commands,
                  "the pre-reset commands reach the transport before its reset");
        }
        auto const f = reader.flashAndWait(8000ms);
        CHECK(f.has_value() && shared.flashes.load() == 1, "flashAndWait flashes once");
        shared.flashFailures = 1;
        auto const again     = reader.flashAndWait(12000ms);
        CHECK(again.has_value() && shared.flashes.load() == 2, "one failed download is retried");
        shared.flashFailures  = 3;
        auto const beforeFail = reader.sessionCount();
        auto const failed     = reader.flashAndWait(12000ms);
        CHECK(waitFor([&]() { return reader.sessionCount() > beforeFail; }, 5000ms),
              "after a failed flash the log comes back");
        CHECK(!failed.has_value() && failed.error().contains("fake flash failure"),
              "three failed downloads are reported, not retried for ever");
        CHECK(shared.flashes.load() == 2 && !reader.isFlashing(),
              "and the flash request is dropped");
        CHECK(sessionStarts.load() >= 2 && sessionByte.load() == 0x42, "session start callback");
        CHECK(!reader.isRamImage(), "an ordinary image by default");
        CHECK(!collected.anyMessageContains("reset from outside"),
              "the printer's own resets and flashes are not taken for one from outside");
    }

    // a RAM image: flash and reset load it and start it, and never reset after that (a reader of its own: the image
    // source is fixed for a reader's life)
    {
        JLink::RamImageStart const start{0x2000'0000, 0x2008'2000, 0x2000'0199};
        std::atomic<bool>          mapped{true};   // false: where it starts is unknown
        auto const                 ramStart = [&]() {
            return mapped
                   ? uc_log::printer::RamImageStartResult{start}
                   : uc_log::printer::RamImageStartResult{std::unexpected{std::string{"no map"}}};
        };
        ReaderUnderTest under{
          {.image = {.ramImage = ramStart}, .options = {.preResetCommands = commands}}
        };
        [[maybe_unused]] Reader&    reader    = *under.reader;
        [[maybe_unused]] Collected& collected = under.said;
        CHECK(reader.isRamImage(), "a RAM image once told");
        CHECK(waitFor([&]() { return reader.getStatus().isRunning != 0; }, 5000ms),
              "a RAM image's session runs");
        auto const ramResetsBefore = shared.resets.load();
        auto const flashesBefore   = shared.flashes.load();
        auto const sessionsRam     = reader.sessionCount();
        auto const ram             = reader.flashAndWait(8000ms);
        CHECK(ram.has_value() && shared.ramFlashes.load() == 1,
              "a RAM image is loaded and started");
        CHECK(reader.sessionCount() > sessionsRam, "and its log comes back");
        CHECK(shared.resets.load() == ramResetsBefore && shared.flashes.load() == flashesBefore,
              "no reset after it (that would boot flash), no plain download");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            CHECK(shared.lastRamStart == start, "started at its vector table");
            CHECK(shared.preResetAtLastReset == commands, "the pre-reset commands are there too");
        }
        auto const ramReset = reader.resetAndWait(5000ms);
        CHECK(ramReset.has_value() && shared.ramFlashes.load() == 2
                && shared.resets.load() == ramResetsBefore,
              "a reset of a RAM image loads it again instead of a reset");

        // where it starts is unknown (no map symbol, no table in the hex): told, not tried
        mapped              = false;
        auto const t0       = Clock::now();
        auto const noReset  = reader.resetAndWait(5000ms);
        auto const tookLong = Clock::now() - t0 > 3000ms;
        CHECK(!noReset && noReset.error() == "not reset: no map" && !tookLong,
              "a reset says why at once");
        auto const noFlash = reader.flashAndWait(12000ms);
        CHECK(!noFlash && noFlash.error() == "no map", "so does a flash");
        CHECK(shared.ramFlashes.load() == 2 && shared.resets.load() == ramResetsBefore,
              "and neither touches the target");
        // the failed download ended the session
        CHECK(waitFor([&]() { return reader.getStatus().isRunning != 0; }, 5000ms),
              "the log comes back after the refused flash");
        CHECK(!collected.anyMessageContains("reset from outside"),
              "a RAM image's loads are not taken for a reset from outside");
    }
}

inline void outsideResetStartsOver() {
    Scenario s;
    auto&    shared = s.shared;
    // where the map says the control block is (a rebuild changes it)
    std::atomic<std::uint32_t>  blockAddress{0x20000100};
    ReaderUnderTest             under{{.block = [&blockAddress]() {
        return uc_log::printer::RttBlockInfo{blockAddress.load(), 1};
    }}};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // a running session first: what it read at its start is the baseline the change below is seen against
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");
    // a reset from outside (picotool, a watchdog): SEGGER's reader would read an old lap of a
    // ring once the new image rewrites the control block, so the session starts over - after the
    // lines already read, without the rest of a line the reset cut off
    {
        CHECK(!collected.anyMessageContains("reset from outside"),
              "the printer's own resets and flashes are not taken for one from outside");
        auto const sessions = reader.sessionCount();
        auto const built    = shared.constructions.load();
        auto const hellos   = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.entries, [](auto const& e) {
                return e.contains("hello from target");
            });
        };
        auto const before = hellos();
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            auto const                         frame = makeFrame(0);
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.end());
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.begin() + 2);
            shared.coreWasReset = true;
        }
        CHECK(waitFor([&]() { return collected.anyMessageContains("reset from outside"); }, 3000ms),
              "a reset from outside is told as a status message");
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions; }, 3000ms)
                && shared.constructions.load() > built,
              "and a new session starts");
        CHECK(hellos() == before + 1, "the line read before the reset is kept, once");
        CHECK(waitFor(
                [&]() {
                    return collected.anyMessageContains(
                      "2 bytes of a line cut off by the reset dropped");
                },
                3000ms),
              "the cut-off line is dropped and counted");
        CHECK(!collected.anyErrorContains("resync"), "and does not show up as a stuck frame");
        auto const after = reader.sessionCount();
        Clock::sleep_for(200ms);
        CHECK(reader.sessionCount() == after, "one new session, not one per pass");

        // into a boot ROM that clears the block: the next session waits for it, quietly
        shared.blockGone    = true;
        shared.coreWasReset = true;
        CHECK(waitFor(
                [&]() { return collected.anyMessageContains("waiting for the RTT control block"); },
                3000ms),
              "a session after a reset from outside waits for the control block");
        auto const waiting = reader.sessionCount();
        Clock::sleep_for(300ms);
        CHECK(reader.sessionCount() == waiting, "and starts no session while it is gone");
        {
            std::array<uc_log::printer::TargetOp, 1> const piece{
              uc_log::printer::target_op::Read{0x20000010, 4}
            };
            CHECK(reader.accessTarget(piece, 2000ms).has_value(),
                  "memory reads are served meanwhile");
        }
        shared.blockGone = false;
        CHECK(waitFor([&]() { return reader.sessionCount() > waiting; }, 3000ms),
              "the session starts once the block is back");

        // the files changed to another image (its map: another block address) and a reset: the wait is at that
        // address, where nothing is; the files back - the wait follows the map and the session starts
        blockAddress        = 0x20000300;
        shared.coreWasReset = true;
        CHECK(waitFor([&]() { return collected.anyMessageContains("control block at 0x20000300"); },
                      3000ms),
              "the wait is where the map says");
        auto const atOther = reader.sessionCount();
        Clock::sleep_for(300ms);
        CHECK(reader.sessionCount() == atOther,
              "no session while the map points where no block is");
        blockAddress = 0x20000100;
        CHECK(
          waitFor([&]() { return reader.sessionCount() > atOther; }, 3000ms),
          "the map back at the block: the session starts (it waited for good at the old address)");
    }
}

inline void announcedReset() {
    Scenario s;
    auto&    shared = s.shared;
    {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.announce = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
    }
    ReaderUnderTest             under{{.image = {.announcedReset = announceBlock}}};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // an announced reset (Kvasir_SDK Runtime/AnnouncedReset.hpp): the printer arms the block, and on
    // a request drains the log, stops RTT, clears the control block id, writes the ack and closes
    // the session - and touches nothing until the pause is over, then connects again
    {
        using namespace uc_log::printer::announced_reset;
        auto const sessions = reader.sessionCount();
        reader.resetJLink();
        auto const armed = [&]() {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == hostArmed;
        };
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions && armed(); }, 3000ms),
              "a session start arms the block");
        auto const hellos = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.entries, [](auto const& e) {
                return e.find("hello from target") != std::string::npos;
            });
        };
        Clock::sleep_for(100ms);
        CHECK(!reader.isPausedForReset(), "no pause without a request");
        auto const before = hellos();
        auto const built  = shared.constructions.load();
        {
            // the firmware's last line, then its request
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            auto const                         frame = makeFrame(0);
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.end());
            shared.announce[2] = 1;
        }
        CHECK(waitFor([&]() { return shared.resetting.load(); }, 3000ms), "the ack is written");
        auto const ackedAt = shared.ackedAt.load();   // the write itself, not when we noticed it
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            CHECK(shared.announce[3] == 1, "ack = request");
            using Write           = std::pair<std::uint32_t, std::uint32_t>;
            auto const  idCleared = std::ranges::find(shared.writes, Write{0x20000100U, 0U});
            Write const ackWrite  = {FakeTransport::Shared::announceAddress + 12, 1U};
            CHECK(idCleared != shared.writes.end(), "the control block's id is cleared");
            CHECK(!shared.writes.empty() && shared.writes.back() == ackWrite,
                  "the ack is the last write");
        }
        CHECK(shared.rttStops.load() >= 1, "RTT is stopped before it");
        CHECK(hellos() == before + 1, "the last line before the reset is in the log");
        CHECK(
          waitFor([&]() { return collected.anyMessageContains("the target announced a reset"); },
                  3000ms),
          "told as a status message");
        CHECK(waitFor([&]() { return shared.open.load() == 0; }, 1000ms), "the session is closed");
        CHECK(reader.isPausedForReset(), "the reader is paused");
        {
            std::array<uc_log::printer::TargetOp, 1> const piece{
              uc_log::printer::target_op::Read{0x20000010, 4}
            };
            auto const t0     = Clock::now();
            auto const result = reader.accessTarget(piece, 2000ms);
            CHECK(!result && result.error().contains("announced reset")
                    && Clock::now() - t0 < 100ms,
                  "a memory access during the pause fails at once");
        }
        // the chip reboots for ~1 s; then the new boot's .data: a fresh block and the RTT id
        Clock::sleep_until(ackedAt + 1000ms);
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce = {magicValue, 0, 0, 0};
        }
        shared.resetting = false;
        shared.blockGone = false;
        CHECK(shared.touchedWhileResetting.load() == 0, "no DLL call touched the resetting target");
        CHECK(shared.constructions.load() == built, "no reconnect during the pause");
        CHECK(waitFor([&]() { return shared.constructions.load() > built && armed(); }, 3000ms),
              "connected again after the pause, and armed again");
        CHECK(Clock::now() - ackedAt >= uc_log::printer::announced_reset::printerPause,
              "not before the pause is over");
        CHECK(!reader.isPausedForReset(), "the pause is over");
        shared.feed(makeFrame(0));
        CHECK(waitFor([&]() { return hellos() == before + 2; }, 3000ms), "the log runs again");
    }
}

inline void lateAnswerDoesNotWriteNewBoot() {
    Scenario s;
    auto&    shared = s.shared;
    {   // armed by the first session, as the announced reset before it left the block
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.announce = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
    }
    ReaderUnderTest             under{{.image = {.announcedReset = announceBlock}}};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    // a request the printer answers too late - it was busy (a halt it caught, a slow probe) and
    // the firmware's bounded wait ran out: the chip has reset already. The printer must not write
    // into the new boot: a cleared id there would hide its control block from every session
    {
        using namespace uc_log::printer::announced_reset;
        auto const armed = [&]() {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == hostArmed;
        };
        CHECK(waitFor(armed, 3000ms), "armed");
        std::size_t writesBefore{};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            writesBefore           = shared.writes.size();
            shared.gaveUpBeforeAck = true;
            shared.announce[2]     = 2;
        }
        CHECK(
          waitFor(
            [&]() { return collected.anyMessageContains("did it before the printer answered"); },
            3000ms),
          "told that the reset came first");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            using Write      = std::pair<std::uint32_t, std::uint32_t>;
            auto const after = std::ranges::subrange(shared.writes.begin()
                                                       + static_cast<std::ptrdiff_t>(writesBefore),
                                                     shared.writes.end());
            CHECK(std::ranges::find(after, Write{0x20000100U, 0U}) == after.end(),
                  "the new boot's control block keeps its id");
            CHECK(std::ranges::find_if(after,
                                       [](Write const& w) {
                                           return w.first
                                               == FakeTransport::Shared::announceAddress + 12;
                                       })
                    == after.end(),
                  "no ack into the new boot's block");
        }
        CHECK(!shared.blockGone.load(), "the block is still there for the next session");
        CHECK(waitFor(armed, 3000ms), "the next session arms the new boot's block");
    }
}

inline void lateArming() {
    Scenario s;
    auto&    shared = s.shared;
    {   // armed by the first session, as the announced reset before it left the block
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.announce = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
    }
    ReaderUnderTest             under{{.image = {.announcedReset = announceBlock}}};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    CHECK(waitFor(
            [&]() {
                std::scoped_lock<std::mutex> const lock{shared.mutex};
                return shared.announce[1] == uc_log::printer::announced_reset::hostArmed;
            },
            3000ms),
          "late arming: the first session arms the block");
    // a session that starts before the boot has set the block up (right after a flash, .data not
    // copied yet) looks for it again and arms it once it is there - in the same session, and a
    // request afterwards is served as usual (on the RP2040 a reset the printer does not know of
    // ends in the Rescue DP, so a session must never stay unarmed)
    {
        using namespace uc_log::printer::announced_reset;
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce = {0, 0, 0, 0};
        }
        auto const sessions = reader.sessionCount();
        auto const notYet   = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.tools, [](auto const& m) {
                return m.contains("announced resets: no block at");
            });
        };
        auto const notYetBefore = notYet();
        reader.resetJLink();
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions; }, 3000ms),
              "a session starts without the block");
        auto const armed = [&]() {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == hostArmed;
        };
        Clock::sleep_for(150ms);
        CHECK(!armed(), "nothing armed while the block is not there");
        CHECK(notYet() == notYetBefore + 1, "said once, not once per look");
        auto const session = reader.sessionCount();
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce = {magicValue, 0, 0, 0};
        }
        CHECK(waitFor(armed, 500ms), "armed once the block is set up");
        CHECK(reader.sessionCount() == session, "in the same session");
        CHECK(notYet() == notYetBefore + 1, "no further message");
        auto const built = shared.constructions.load();
        {
            // a watchdog left to run out: the request asks the printer to stay away 3 s
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce[2] = (30U << 16) | 1U;
        }
        CHECK(waitFor([&]() { return shared.resetting.load(); }, 3000ms),
              "a request after the late arming is acked");
        auto const ackedAt = shared.ackedAt.load();
        CHECK(shared.announce[3] == ((30U << 16) | 1U), "the whole request word is the ack");
        CHECK(
          waitFor(
            [&]() { return collected.anyMessageContains("the probe stays off it for 3000 ms"); },
            3000ms),
          "the time the firmware asked for is told");
        Clock::sleep_for(1000ms);
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce = {magicValue, 0, 0, 0};
        }
        shared.resetting = false;
        shared.blockGone = false;
        CHECK(waitFor([&]() { return shared.constructions.load() > built && armed(); }, 5000ms),
              "and the session after the pause is armed again");
        CHECK(Clock::now() - ackedAt >= 3000ms, "not before the 3 s the firmware asked for");
    }
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{             "resetsAndFlashes",              resetsAndFlashes},
  uc_log::test::Case{       "outsideResetStartsOver",        outsideResetStartsOver},
  uc_log::test::Case{               "announcedReset",                announcedReset},
  uc_log::test::Case{"lateAnswerDoesNotWriteNewBoot", lateAnswerDoesNotWriteNewBoot},
  uc_log::test::Case{                   "lateArming",                    lateArming},
};

}   // namespace uc_log_test_rtt_reader::resets
