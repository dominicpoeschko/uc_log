#pragma once

// rtt_reader tests: boot counts: unread boots and a torn tail across a reboot. Included only by rtt_reader_tests.cpp
// (one binary for every rtt_reader case); the fake J-Link and the shared pieces: support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::boots {

inline void unreadBoots() {
    Scenario s;
    auto&    shared = s.shared;
    {
        // boots nobody read, counted exactly by the image's count of boots that logged (printer/BootLedger.hpp; the RTT
        // epoch moves at every start, also one the J-Link DLL parks before main - it may not be counted)
        constexpr std::uint32_t        block    = FakeTransport::Shared::ramBase;
        constexpr std::uint32_t        ringSize = 256;
        constexpr std::uint32_t        desc0    = block + 24;
        constexpr std::uint32_t        ring0    = block + 0x100;
        static constexpr std::uint32_t epochAt  = block + 0xF0;
        static constexpr std::uint32_t loggedAt = block + 0xF4;
        // a start of the target: its epoch, its count of boots that logged, and its ring (two records if it logged, or
        // `bytes` of them: 2 is half a record)
        auto const start
          = [&](std::uint32_t epoch, std::uint32_t logged, bool logs, std::uint32_t bytes = 8) {
                std::scoped_lock<std::mutex> const lock{shared.mutex};
                shared.putRam(epochAt, epoch);
                shared.putRam(loggedAt,
                              logged << 16U
                                | (epoch & 0xFFFFU));   // the count in 31:16 (detail/RttEpoch.hpp)
                shared.putRam(loggedAt + 4,
                              ~(logged << 16U | (epoch & 0xFFFFU)));   // and its complement
                shared.putRam(desc0 + 12, logs ? bytes : 0U);
                shared.putRam(desc0 + 16, 0);
                shared.coreWasReset = true;
            };
        constexpr std::array<RingSpec, 1> ring{
          RingSpec{.buffer = ring0, .size = ringSize}
        };
        RamTarget target{shared, 0x100 + ringSize, ring};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
            shared.putRam(epochAt, 8);
            shared.putRam(loggedAt, 7U << 16U | 8U);
            shared.putRam(loggedAt + 4, ~(7U << 16U | 8U));
            auto const frame
              = makeFrame(0);   // what a boot that logged left: two records ("hello direct")
            for(std::size_t i = 0; i != 8; ++i) { shared.ram[0x100 + i] = frame[i % frame.size()]; }
            shared.putRam(desc0 + 12, 8);   // boot 7 logged 8 bytes: read by the first session
            shared.connected = false;
        }
        Collected                                 said;
        std::mutex                                unreadMutex;
        std::vector<std::array<std::uint32_t, 3>> unread;
        auto                                      r = makeReader(
          said,
          {
            .block       = []() { return uc_log::printer::RttBlockInfo{block, 1}; },
            .catalogText = "hello direct",
            .unreadBoots =
              [&](std::uint32_t n, std::uint32_t from, std::uint32_t to) {
                  std::scoped_lock<std::mutex> const lock{unreadMutex};
                  unread.push_back({n, from, to});
                                 },
            .image = {.rttEpoch    = []() { return std::optional<std::uint32_t>{epochAt}; },
                                 .loggedBoots = []() { return std::optional<std::uint32_t>{loggedAt}; }}
        });
        auto const unreadCount = [&] {
            std::scoped_lock<std::mutex> const lock{unreadMutex};
            return unread.size();
        };
        // the session has read its boot's bytes - and handled them: the read offset is written inside the read, the
        // boot is noted after it (decodeRead), so a start right after the offset alone could overwrite the count first
        auto const read = [&] {
            bool const done = waitFor(
              [&]() {
                  std::scoped_lock<std::mutex> const lock{shared.mutex};
                  return shared.getRam(desc0 + 16) == shared.getRam(desc0 + 12);
              },
              4000ms);
            Clock::sleep_for(100ms);
            return done;
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(read(), "boots: boot 7 read");

        // (b) boot 8 logged and nobody read it, boot 9 logged and is read: exactly 1
        start(10, 9, true);
        CHECK(read() && waitFor([&]() { return unreadCount() == 1; }, 4000ms),
              "boots: one that logged unread is noticed");
        {
            std::scoped_lock<std::mutex> const lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 7, 8};
            CHECK(unread.size() == 1 && unread.back() == want, "boots: exactly 1 (counts 7 -> 8)");
        }
        CHECK(waitFor(
                [&]() {
                    return said.anyErrorContains("1 boot passed unread (boots that logged 7 -> 8)");
                },
                3000ms),
              "boots: and said");

        // (a) a start parked before main (the epoch moves, nothing logged), then the printer's reset: nothing unread
        start(12, 9, false);
        Clock::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(), "boots: the printer's reset");
        Clock::sleep_for(500ms);
        CHECK(unreadCount() == 1, "boots: a start that never logged is no unread boot");

        // (c) two starts, one parked before main and one that logged, then the printer's flash: exactly 1
        start(
          14,
          10,
          false);   // boot 12 logged (its count moved) - its line was not kept (a full ring), nothing read
        Clock::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(), "boots: the printer's reset again");
        CHECK(waitFor([&]() { return unreadCount() == 2; }, 4000ms),
              "boots: the one that logged is counted");
        {
            std::scoped_lock<std::mutex> const lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 9, 10};
            CHECK(unread.size() == 2 && unread.back() == want,
                  "boots: exactly 1 of the two starts (counts 9 -> 10)");
        }

        // (d) boot 11 logged, a session read its first bytes - half a record - and ended on a probe failure: no line of it,
        // so not read after all; the printer's reset then counts it
        start(16, 11, true, 2);
        CHECK(read(), "boots: half a record of boot 11 read");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.failReadFrom
              = desc0;   // the next read of the ring's offsets: the J-Link drops off USB
            shared.failReadTo = desc0 + 24;
        }
        Clock::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(),
              "boots: the printer's reset after the probe failure");
        CHECK(waitFor([&]() { return unreadCount() == 3; }, 4000ms),
              "boots: the boot whose bytes gave no line is counted");
        {
            std::scoped_lock<std::mutex> const lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 10, 11};
            CHECK(unread.size() == 3 && unread.back() == want,
                  "boots: exactly that one (counts 10 -> 11)");
        }

        // (e) a reset from outside while the boot ROM runs: RAM still holds the old boot's block and epoch - no session (no
        // firmware check reading a flash the ROM drives) until the new boot moved the epoch
        {
            auto const before = r->sessionCount();
            {
                std::scoped_lock<std::mutex> const lock{shared.mutex};
                shared.coreWasReset = true;
            }
            Clock::sleep_for(1500ms);
            CHECK(r->sessionCount() == before,
                  "boots: the old boot's block in RAM starts no session");
            start(18, 12, true);
            CHECK(waitFor([&]() { return r->sessionCount() > before; }, 4000ms),
                  "boots: the new epoch's does");
            // and it read the new boot's bytes before the next start overwrites the counts (each effect waited for)
            CHECK(read(), "boots: the new epoch's session reads its boot");
        }

        // a count that went back (a power-on, a download over RAM): said, no number
        start(20, 3, true);
        CHECK(waitFor([&]() { return said.anyErrorContains("how many is unknown"); }, 4000ms),
              "boots: a count that went back is said as not countable");
        CHECK(unreadCount() == 3, "boots: with no number handed over");
        if(unreadCount() != 3) {
            std::scoped_lock<std::mutex> const lock{unreadMutex};
            for(auto const& u : unread) {
                std::printf("  unread %u (%u -> %u)\n", u[0], u[1], u[2]);
            }
        }

        // (g) the count not kept (its complement does not match: a watchdog reboot that reset the SRAM flipped its bits)
        // - no number claimed, said, the restart row; the next kept count is a new baseline
        {
            auto const restartsBefore = [&] {
                std::scoped_lock<std::mutex> const lock{said.mutex};
                return std::ranges::count_if(said.messages, [](auto const& m) {
                    return m.find("was not kept") != std::string::npos;
                });
            }();
            auto const unknownCount = [&] {
                std::scoped_lock<std::mutex> const lock{said.mutex};
                return std::ranges::count_if(said.errors, [](auto const& e) {
                    return e.find("how many is unknown") != std::string::npos;
                });
            };
            auto const unknownsBefore = unknownCount();
            {
                std::scoped_lock<std::mutex> const lock{shared.mutex};
                shared.putRam(epochAt, 22);
                shared.putRam(loggedAt,
                              1U << 16U | 22U);   // the boot found junk and started the count again
                shared.putRam(loggedAt + 4,
                              ~(1U << 16U | 22U) ^ 1U);   // its complement, marked: restarted here
                shared.putRam(desc0 + 12, 8);
                shared.putRam(desc0 + 16, 0);
                shared.coreWasReset = true;
            }
            CHECK(read()
                    && waitFor(
                      [&]() {
                          std::scoped_lock<std::mutex> const lock{said.mutex};
                          return std::ranges::count_if(said.messages,
                                                       [](auto const& m) {
                                                           return m.find("was not kept")
                                                               != std::string::npos;
                                                       })
                               > restartsBefore;
                      },
                      4000ms),
                  "boots: a count not kept is said");
            CHECK(unreadCount() == 3, "boots: and no number claimed for it");
            // the restarted count is the new baseline: the session start compares it with nothing from before (the
            // last settled count, 3, would read as a count that went back)
            CHECK(unknownCount() == unknownsBefore, "boots: and the count starts again from it");
        }

        // (f) the printer's own flash: its download wrote over the target's RAM (J-Link's flash loader; the count read
        // 9723 before, 51937 after) - the count starts again, said, never compared across it
        start(24, 2, true);   // the boot after the restarted one (1)
        CHECK(read(), "boots: a boot read before the flash");
        auto const unknownBefore = [&] {
            std::scoped_lock<std::mutex> const lock{said.mutex};
            return std::ranges::count_if(said.errors, [](auto const& e) {
                return e.find("how many is unknown") != std::string::npos;
            });
        };
        auto const before = unknownBefore();
        shared.onReset    = [&]() {
            start(26, 51937, true);
        };   // the new image's first boot: a count from RAM junk
        CHECK(r->flashAndWait(8000ms).has_value(), "boots: the printer's flash");
        CHECK(read()
                && waitFor(
                  [&]() {
                      return said.anyMessageContains("the count of boots that logged starts again");
                  },
                  4000ms),
              "boots: the restart is said");
        Clock::sleep_for(500ms);
        CHECK(unknownBefore() == before && unreadCount() == 3,
              "boots: no unknown, no number across the download");
        shared.onReset = {};
        r.reset();
    }
}

inline void tornTailAcrossReboot() {
    Scenario s;
    auto&    shared = s.shared;
    {
        // A torn tail across a reboot ("channel 0 corrupted data removed"): a session
        // reads one and a half records, a probe read fails (the half is carried), the target reboots - its epoch moves in
        // preMemoryInit, but the .data copy re-initialises the block only after that. A session opened in between must
        // not read on from the OLD block's read offset with the carry dropped: that starts mid-record. With the epoch
        // odd while a boot initialises (RttEpoch.hpp) it waits for the new block.
        constexpr std::uint32_t           block    = FakeTransport::Shared::ramBase;
        constexpr std::uint32_t           ringSize = 256;
        constexpr std::uint32_t           desc0    = block + 24;
        constexpr std::uint32_t           ring0    = block + 0x100;
        static constexpr std::uint32_t    epochAt  = block + 0xF0;
        auto const                        frame    = makeFrame(0);
        constexpr std::array<RingSpec, 1> ring{
          RingSpec{.buffer = ring0, .size = ringSize}
        };
        RamTarget target{shared, 0x100 + ringSize, ring};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
            shared.putRam(epochAt, 20);
            for(std::size_t i = 0; i != 12; ++i) {
                shared.ram[0x100 + i] = frame[i % frame.size()];
            }
            shared.putRam(desc0 + 12, 6);   // one record and half of the next
            shared.putRam(desc0 + 16, 0);
            shared.connected = false;
        }
        Collected torn;
        auto      r = makeReader(
          torn,
          {.block       = []() { return uc_log::printer::RttBlockInfo{block, 1}; },
           .catalogText = "hello direct",
           .image       = {.rttEpoch = []() { return std::optional<std::uint32_t>{epochAt}; }}});
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return torn.anyEntryContains("hello direct"); }, 4000ms),
              "torn tail: the first record");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(
              desc0 + 12,
              12);   // the old boot wrote the rest, then rebooted: the epoch moved (odd: its
            shared.putRam(epochAt, 21);   // init runs), the block is still the old one
            shared.failReadFrom
              = desc0;   // and the probe drops a read: the session ends holding half a record
            shared.failReadTo = desc0 + 24;
        }
        Clock::sleep_for(1500ms);
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(desc0 + 12,
                          4);   // the new boot's block: re-initialised, one record of its own
            shared.putRam(desc0 + 16, 0);
            shared.putRam(epochAt, 22);
        }
        CHECK(waitFor(
                [&]() {
                    std::scoped_lock<std::mutex> const lock{torn.mutex};
                    return std::ranges::count_if(torn.entries,
                                                 [](auto const& e) {
                                                     return e.find("hello direct")
                                                         != std::string::npos;
                                                 })
                        >= 2;
                },
                4000ms),
              "torn tail: the new boot's record");
        CHECK(waitFor([&]() { return torn.anyMessageContains("the RTT epoch stays odd"); }, 3000ms),
              "torn tail: an epoch odd for long is said");
        bool const clean
          = !torn.anyErrorContains("corrupted data") && !torn.anyErrorContains("damaged frame");
        CHECK(clean, "torn tail: no read from the middle of a record");
        if(!clean) { torn.dump(); }
        r.reset();
    }
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{         "unreadBoots",          unreadBoots},
  uc_log::test::Case{"tornTailAcrossReboot", tornTailAcrossReboot},
};

}   // namespace uc_log_test_rtt_reader::boots
