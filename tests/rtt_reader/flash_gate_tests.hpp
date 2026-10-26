#pragma once

// rtt_reader tests: the flash-ready gate and the firmware check: the flash read only while mapped, resets in the check.
// Included only by rtt_reader_tests.cpp (one binary for every rtt_reader case); the fake J-Link and the shared pieces:
// support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::flash_gate {

// The firmware check reads the flash only while it is mapped (--flash_ready_signature): an unannounced reset lands
// the boot ROM in its plain-SPI loop, and an XIP read then hangs it (the boot ROM stall). The session start's read
// waits for the chip's words (here: SSI CTRLR0.TMOD EEPROM_READ), then reads; never mapped, it fails without
// touching the flash.
inline void flashReadyWaitsForMapping() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {
        put(FakeTransport::Shared::ssiWord + 4, 1);   // SSIENR: enabled
        ssi(0x0007'0000);   // the ROM's plain SPI transfer: SPI_FRF 0, DFS_32 7, TMOD 0
        shared.romHung    = false;
        shared.flashReads = 0;
        std::vector<std::byte> got;
        std::atomic<int>       results{0};
        std::atomic<bool>      readOk{false};
        auto                   r = start(got, results, readOk);
        Clock::sleep_for(600ms);
        CHECK(shared.flashReads.load() == 0 && results.load() == 0,
              "flash ready: no read while the boot ROM drives the flash, the check waits");
        ssi(0x005f'0300);   // boot2's XIP: SPI_FRF QUAD, DFS_32 31, TMOD EEPROM_READ
        bool const checked = waitFor([&]() { return results.load() >= 1; }, 8000ms);
        CHECK(checked, "flash ready: the check reads once mapped");
        if(!checked) { flashCheck.dump(); }
        CHECK(readOk.load() && !got.empty() && got[0] == std::byte{0x5a} && !shared.romHung.load(),
              "flash ready: the read sees the flash, the boot ROM never saw an XIP access");
    }
}

inline void flashNeverMapped() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {
        put(FakeTransport::Shared::ssiWord + 4,
            0);   // SSIENR 0: not mapped, though TMOD says so
        ssi(0x005f'0300);
        shared.romHung    = false;
        shared.flashReads = 0;
        std::vector<std::byte> got;
        std::atomic<int>       results{0};
        std::atomic<bool>      readOk{true};
        auto                   r = start(got, results, readOk);
        CHECK(waitFor([&]() { return results.load() >= 1; }, 6000ms),
              "flash ready: a check that waited ends");
        CHECK(waitFor(
                [&]() {
                    return !readOk.load() && flashCheck.anyToolContains("the flash is not mapped");
                },
                3000ms),
              "flash ready: never mapped - said");
        Clock::sleep_for(4000ms);   // the session reads its rings' names now (in flash)
        CHECK(shared.flashReads.load() == 0 && !shared.romHung.load(),
              "flash ready: never mapped - no flash read, the ring names' neither");
    }
}

inline void resetInTheCheck() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {   // a core-0-only reset in the middle of the check: the words still say "mapped" (the SSI keeps boot2's setup),
        // S_RESET_ST says it - the read it fell into is dropped, no XIP read after it
        put(FakeTransport::Shared::ssiWord + 4, 1);
        ssi(0x005f'0300);
        shared.flashReads           = 0;
        shared.resetAfterFlashReads = 1;
        std::atomic<int> okReads{0};
        std::atomic<int> done{0};
        std::atomic<int> readsAtEnd{-1};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = false;
        }
        auto const sessionStart = [&](uc_log::printer::DirectMemoryRead const& read) {
            if(done.load() != 0) { return; }
            for(int i = 0; i != 4; ++i) {
                std::vector<std::byte> b(16);
                if(read(FakeTransport::Shared::flashBase + 0x1000U * static_cast<std::uint32_t>(i),
                        b))
                {
                    ++okReads;
                }
            }
            readsAtEnd = shared.flashReads.load();
            ++done;
        };
        auto r = makeReader(
          flashCheck,
          {
            .block
            = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
            .catalogText = "hello direct",
            .image       = {.announcedReset = announceBlock, .onSessionStart = sessionStart},
            .options     = {.flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300}}}
        });
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.announce  = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return done.load() >= 1; }, 6000ms),
              "reset in the check: the check ends");
        CHECK(okReads.load() == 0 && readsAtEnd.load() == 1,
              "reset in the check: the read it hit dropped, no XIP read after it");
        shared.resetAfterFlashReads = -1;
        r.reset();
    }
}

inline void resetBeforeTheCheck() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {   // a reset before the check's first read: that read is not made at all
        // the flash mapped (SSIENR 1, boot2's XIP), as the case before this one left it when they were one
        put(FakeTransport::Shared::ssiWord + 4, 1);
        ssi(0x005f'0300);
        shared.flashReads = 0;
        std::atomic<int> readsAtEnd{-1};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = false;
        }
        auto const sessionStart = [&](uc_log::printer::DirectMemoryRead const& read) {
            if(readsAtEnd.load() >= 0) { return; }
            shared.coreWasReset = true;   // the reset lands right before the check reads
            std::vector<std::byte> b(16);
            static_cast<void>(read(FakeTransport::Shared::flashBase, b));
            readsAtEnd = shared.flashReads.load();
        };
        auto r = makeReader(
          flashCheck,
          {.block
           = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
           .catalogText = "hello direct",
           .image       = {.onSessionStart = sessionStart},
           .options     = {.flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300}}}});
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return readsAtEnd.load() >= 0; }, 6000ms) && readsAtEnd.load() == 0,
              "reset before the check: no XIP read at all");
        r.reset();
    }
}

inline void armedAfterFlashReads() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {   // the announce block is armed only after the session start's flash reads (an announced reset waits for them)
        // the flash mapped (SSIENR 1, boot2's XIP), as the case before this one left it when they were one
        put(FakeTransport::Shared::ssiWord + 4, 1);
        ssi(0x005f'0300);
        shared.flashReads      = 0;
        shared.flashReadsAtArm = -1;
        std::atomic<int> done{0};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = false;
            shared.announce  = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
        }
        auto const sessionStart = [&](uc_log::printer::DirectMemoryRead const& read) {
            std::vector<std::byte> b(16);
            static_cast<void>(read(FakeTransport::Shared::flashBase, b));
            static_cast<void>(read(FakeTransport::Shared::flashBase + 0x1000, b));
            ++done;
        };
        auto r = makeReader(
          flashCheck,
          {
            .block
            = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
            .catalogText = "hello direct",
            .image       = {.announcedReset = announceBlock, .onSessionStart = sessionStart},
            .options     = {.flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300}}}
        });
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return shared.flashReadsAtArm.load() >= 0; }, 6000ms),
              "armed: the block is armed");
        CHECK(shared.flashReadsAtArm.load() >= 2,
              "armed: only after the session start's two flash reads");
        r.reset();
    }
}

inline void flashWindowGate() {
    Scenario                    s;
    auto&                       shared = s.shared;
    FlashGate                   gate{shared};
    [[maybe_unused]] Collected& flashCheck = gate.flashCheck;
    [[maybe_unused]] auto const put
      = [&](std::uint32_t address, std::uint32_t v) { gate.put(address, v); };
    [[maybe_unused]] auto const ssi = [&](std::uint32_t v) { gate.ssi(v); };
    [[maybe_unused]] auto const start
      = [&](std::vector<std::byte>& got, std::atomic<int>& results, std::atomic<bool>& readOk) {
            return gate.start(got, results, readOk);
        };
    {                       // a control-socket read in the flash window goes through the same gate
        ssi(0x0007'0000);   // the boot ROM drives the flash
        shared.flashReads = 0;
        shared.romHung    = false;
        auto r            = makeReader(
          flashCheck,
          {
            .block
            = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
            .catalogText = "hello direct",
            .options = {.flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300}},
                    .flashWindow
                        = std::pair<std::uint32_t, std::uint32_t>{FakeTransport::Shared::flashBase,
                                                                  0x10'0000}}
        });
        std::array<uc_log::printer::TargetOp, 1> const flashOp{
          uc_log::printer::target_op::Read{FakeTransport::Shared::flashBase + 0x40, 16}
        };
        auto const refused = r->accessTarget(flashOp, 8000ms);
        CHECK(!refused.has_value() && shared.flashReads.load() == 0 && !shared.romHung.load(),
              "window: not read while the boot ROM drives the flash");
        std::array<uc_log::printer::TargetOp, 1> const ramOp{
          uc_log::printer::target_op::Read{FakeTransport::Shared::ramBase, 16}
        };
        CHECK(r->accessTarget(ramOp, 4000ms).has_value(), "window: RAM is read as before");
        ssi(0x005f'0300);
        CHECK(r->accessTarget(flashOp, 8000ms).has_value() && shared.flashReads.load() == 1,
              "window: read once mapped");
        r.reset();
    }
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{"flashReadyWaitsForMapping", flashReadyWaitsForMapping},
  uc_log::test::Case{         "flashNeverMapped",          flashNeverMapped},
  uc_log::test::Case{          "resetInTheCheck",           resetInTheCheck},
  uc_log::test::Case{      "resetBeforeTheCheck",       resetBeforeTheCheck},
  uc_log::test::Case{     "armedAfterFlashReads",      armedAfterFlashReads},
  uc_log::test::Case{          "flashWindowGate",           flashWindowGate},
};

}   // namespace uc_log_test_rtt_reader::flash_gate
