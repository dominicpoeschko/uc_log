#pragma once

// rtt_reader tests: stall, lockup and park recovery. Included only by rtt_reader_tests.cpp (one binary for every
// rtt_reader case); the fake J-Link and the shared pieces: support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::recovery {

// A quiet target whose chip package can tell a stall from one word (the RP2350's QMI stuck BUSY): said as an
// error, the package's recovery writes run, and a session ends for the new boot. A word that matches only now and
// then is not a stall.
inline void stallSaidAndRecovered() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {
        auto r = start(0x0301'0803);   // EN and BUSY: stuck
        CHECK(waitFor([&]() { return quiet.anyErrorContains("the target is stalled"); }, 6000ms),
              "a stall is said");
        CHECK(waitFor(
                [&]() {
                    std::scoped_lock<std::mutex> const lock{shared.mutex};
                    return std::ranges::count(
                             shared.writes,
                             std::pair<std::uint32_t, std::uint32_t>{0x400d'8000, 0x8000'0000})
                        >= 1;
                },
                3000ms),
              "and the package's recovery runs");
        // a download that fails because the flash interface is stalled: recovered and flashed again, the
        // recovery not counted as one of the attempts (an RP2350 QMI left busy: "RAMCode did not respond")
        auto const flashesBefore = shared.flashes.load();
        shared.flashFailures
          = 3;   // as many as flashAttempts: without the recovery path the flash fails
        auto const flashed = r->flashAndWait(20000ms);
        CHECK(waitFor(
                [&]() {
                    return flashed.has_value() && shared.flashes.load() == flashesBefore + 1
                        && quiet.anyErrorContains("the flash failed on a stalled target");
                },
                3000ms),
              "a flash on a stalled target: the recovery, then the download again");
        shared.flashFailures = 0;
    }
}

inline void stallWedgedFlash() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {   // a download wedged where no register shows it (an RP2350: "Error while determining flash info", the QMI
        // idle) until the chip booted once: before the next attempt the package's recovery runs and the core is let
        // run after a reset - inside the attempts; a wedge that outlives that still ends there
        auto       r             = start(0x0000'0000);
        auto const flashesBefore = shared.flashes.load();
        shared.wedged            = true;
        auto const flashed       = r->flashAndWait(20000ms);
        CHECK(waitFor(
                [&]() {
                    return flashed.has_value() && shared.flashes.load() == flashesBefore + 1
                        && quiet.anyErrorContains(
                          "running the chip package's stall recovery before the next attempt");
                },
                3000ms),
              "a flash wedged until the chip boots: the recovery, a reset that runs, then the "
              "download again");
        shared.wedgeOutlivesBoot = true;
        shared.wedged            = true;
        auto const failed        = r->flashAndWait(20000ms);
        CHECK(!failed.has_value() && shared.flashes.load() == flashesBefore + 1,
              "a wedge nothing frees: reported after the attempts, not retried for ever");
        shared.wedgeOutlivesBoot = false;
        shared.wedged            = false;
    }
}

inline void stallWordSetNowAndThen() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {
        auto              r = start(0x0000'0000);
        std::atomic<bool> toggling{true};
        Clock::Thread     toggler{[&]() {   // set now and then: a running image's busy flag
            for(std::uint32_t i = 0; toggling; ++i) {
                {
                    std::scoped_lock<std::mutex> const lock{shared.mutex};
                    shared.putRam(word, (i % 4) == 0 ? 0x3U : 0x0U);
                }
                Clock::sleep_for(3ms);
            }
        }};
        Clock::sleep_for(5000ms);
        toggling = false;
        toggler.join();
        CHECK(!quiet.anyErrorContains("the target is stalled"),
              "a word set only now and then is no stall");
    }
}

inline void stallNeedsEveryCondition() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {
        auto r = start(0x0301'0803,
                       0x005f'0300);   // the first word stuck, the second not as the signature says
        Clock::sleep_for(4000ms);
        CHECK(!quiet.anyErrorContains("the target is stalled"),
              "every condition must hold, not one of them");
    }
}

// The stall ladder goes on across reconnects: the package's recovery, then SYSRESETREQ in the live session - which frees this one - and the
// connect after a recovery waits off the probe first.
inline void stallLadderAcrossReconnects() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {
        {
            std::scoped_lock<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
            quiet.tools.clear();
        }
        shared.stuckWord     = word;
        shared.sysresetFrees = true;
        shared.sysresets     = 0;
        shared.recoveryAtUs  = 0;
        auto r = start(0x0301'0803);   // EN and BUSY: stuck - the recovery writes do not free it
        CHECK(waitFor([&]() { return quiet.anyErrorContains("(2 of 3)"); }, 20000ms),
              "ladder: the second step comes, across the reconnect");
        CHECK(waitFor([&]() { return shared.sysresets.load() >= 1; }, 3000ms)
                && quiet.anyErrorContains("SYSRESETREQ"),
              "ladder: step 2 is a SYSRESETREQ written in the live session");
        auto const pause = shared.connectAfterRecoveryUs.load() - shared.recoveryAtUs.load();
        CHECK(shared.recoveryAtUs.load() != 0 && pause >= 1'400'000,
              "ladder: off the probe ~1.5 s after the recovery");
        if(pause < 1'400'000) { std::printf("  connect %lld us after the recovery\n", pause); }
        shared.sysresetFrees = false;
        shared.stuckWord     = 0;
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.putRam(word, 0);
    }
}

inline void stallFirstStep() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    for(int const first : {2, 3}) {
        {
            std::scoped_lock<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        auto const recoveryWrites = [&]() {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            return std::ranges::count(
              shared.writes,
              std::pair<std::uint32_t, std::uint32_t>{0x400d'8000, 0x8000'0000});
        };
        auto const writesBefore = recoveryWrites();
        shared.stuckWord        = word;
        shared.sysresets        = 0;
        shared.resetLineResets  = 0;
        auto r
          = stallReader(shared, quiet, 0x0301'0803, 0x0007'0000, first);   // EN and BUSY: stuck
        auto const said = fmt::format("({} of 3)", first);
        CHECK(waitFor([&]() { return quiet.anyErrorContains(said); }, 20000ms),
              "first step: the ladder starts at the step asked for");
        CHECK(!quiet.anyErrorContains("(1 of 3)") && recoveryWrites() <= writesBefore,
              "first step: the steps before it do not run");
        if(quiet.anyErrorContains("(1 of 3)") || recoveryWrites() > writesBefore) {
            std::printf("  first step %d: recovery writes %lld -> %lld\n",
                        first,
                        static_cast<long long>(writesBefore),
                        static_cast<long long>(recoveryWrites()));
            std::scoped_lock<std::mutex> const lock{quiet.mutex};
            for(auto const& e : quiet.errors) { std::printf("    %s\n", e.c_str()); }
        }
        if(first == 2) {
            CHECK(waitFor([&]() { return shared.sysresets.load() >= 1; }, 3000ms),
                  "first step 2: a SYSRESETREQ");
        } else {
            CHECK(waitFor([&]() { return shared.resetLineResets.load() >= 1; }, 5000ms)
                    && shared.sysresets.load() == 0,
                  "first step 3: the reset line, no SYSRESETREQ before it");
        }
        r.reset();
        shared.stuckWord = 0;
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.putRam(word, 0);
    }
}

// After the stall recovery the DLL's connection may not see the target again (every call -1: the RP2040's
// watchdog reset under a connected DLL, for minutes): the printer connects again at once and finds it.
inline void stallWhileWaitingForBlock() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    {   // the stall found while waiting for a new boot's block (a reset from outside, the block gone)
        auto r = start(0);
        CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 4000ms),
              "dead after recovery: a session first");
        shared.deadOnRecoveryWrite = true;
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, 0x0301'0803);   // EN and BUSY: stuck
        }
        shared.blockGone    = true;
        shared.coreWasReset = true;
        CHECK(waitFor([&]() { return quiet.anyErrorContains("the target is stalled"); }, 8000ms),
              "dead after recovery: stall said");
        auto const constructionsAtStall = shared.constructions.load();
        // at once - not only after the silence bound (targetSilentLimit, 3 s): the recovery itself reconnects
        CHECK(waitFor(
                [&]() {
                    return shared.constructions.load() > constructionsAtStall
                        && !shared.apDead.load();
                },
                2500ms),
              "dead after recovery: the probe connects again at once and sees the target");
        shared.deadOnRecoveryWrite = false;
        shared.blockGone           = false;
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, 0);
        }
    }
}

inline void lockupDetection() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    // a core in lockup: DHCSR.S_LOCKUP, or halted at the lockup address (a debugger's connect halts a locked-up core
    // and that clears S_LOCKUP) - said and reset; a core halted anywhere else (someone halted it) is left alone
    auto const lockupSaid = [&]() { return quiet.anyErrorContains("in lockup"); };
    {
        std::scoped_lock<std::mutex> const lock{quiet.mutex};
        quiet.errors.clear();
    }
    {
        shared.lockup = true;
        auto r        = start(0);
        CHECK(waitFor(lockupSaid, 6000ms), "DHCSR.S_LOCKUP: said");
    }
    {
        std::scoped_lock<std::mutex> const lock{quiet.mutex};
        quiet.errors.clear();
    }
    {
        shared.lockup = true;
        shared.halted = true;
        auto r        = start(0);
        CHECK(waitFor(lockupSaid, 6000ms), "halted at the lockup address: said too");
    }
    {
        std::scoped_lock<std::mutex> const lock{quiet.mutex};
        quiet.errors.clear();
    }
    {
        shared.lockup   = true;
        shared.halted   = true;
        shared.lockupPc = 0xEFFF'FFFEU;
        auto r          = start(0);
        CHECK(waitFor(lockupSaid, 6000ms), "halted at the Armv8-M lockup address: said too");
        shared.lockupPc = 0xFFFF'FFFEU;
    }
    {
        std::scoped_lock<std::mutex> const lock{quiet.mutex};
        quiet.errors.clear();
    }
    {
        shared.lockup = false;
        shared.halted = true;
        auto r        = start(0);
        Clock::sleep_for(4000ms);
        CHECK(!lockupSaid(), "halted at an ordinary PC: no lockup");
    }
    shared.halted = false;
}

inline void lockupRecoveryKeepsAnswering() {
    Scenario                                 s;
    auto&                                    shared = s.shared;
    RamTarget                                target{shared, 0x800, oneKiBRing};
    [[maybe_unused]] constexpr std::uint32_t word = stallWord;
    Collected                                quiet;
    [[maybe_unused]] auto const              start
      = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            return stallReader(shared, quiet, wordValue, secondValue);
        };
    // After a lockup recovery the printer must go on answering control requests (once, after such a recovery,
    // the requests timed out for good while the log was still decoded). Both places a lockup is
    // recovered: a quiet session, and a closed decode gate (the firmware check failing - the flash unreadable).
    for(bool const viaGate : {false, true}) {
        {
            std::scoped_lock<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        std::atomic<bool> gateOpen{!viaGate};
        auto const        resetsBefore = shared.resets.load();
        shared.onReset = [&]() {   // the reset ends the lockup; the image then runs again
            shared.lockup = false;
            shared.halted = false;
            gateOpen      = true;
        };
        shared.lockup = true;
        shared.halted = true;
        auto r        = stallReader(
          shared,
          quiet,
          0,
          0x0007'0000,
          1,
          viaGate ? std::function<bool(uc_log::printer::DirectMemoryRead const&)>{[&gateOpen](
                                                                                    auto const&) {
              return gateOpen.load();
          }}
                  : std::function<bool(uc_log::printer::DirectMemoryRead const&)>{});
        CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 8000ms),
              viaGate ? "lockup behind a closed gate: reset" : "lockup in a quiet session: reset");
        int answered = 0;
        int failed   = 0;
        for(auto const until = Clock::now() + 4000ms; Clock::now() < until;) {
            std::array<uc_log::printer::TargetOp, 1> const piece{
              uc_log::printer::target_op::Read{FakeTransport::Shared::ramBase, 4}
            };
            if(r->accessTarget(piece, 2000ms).has_value()) {
                ++answered;
            } else {
                ++failed;
            }
            Clock::sleep_for(100ms);
        }
        CHECK(failed == 0 && answered > 10,
              viaGate
                ? "after a lockup recovery behind the gate, control requests are answered"
                : "after a lockup recovery in a quiet session, control requests are answered");
        shared.onReset = {};
    }
    shared.lockup = false;
    shared.halted = false;
}

// a chip the J-Link parked in its boot ROM during its own reboot (the RP2040: CHIP_RESET.HAD_PSM_RESTART set,
// the core asleep in the boot ROM's wfi loop, the RTT block gone). While the printer waits for the block it sees
// the sign and resets the chip itself, said as an error; with the word alone (the core awake) it only says why
// it does not. A reset brings the image back.
inline void parkedInBootRom() {
    for(bool const asleep : {true, false}) {
        Scenario                s;
        auto&                   shared = s.shared;
        RamTarget               target{shared, 0x800, oneKiBRing};
        constexpr std::uint32_t word    = FakeTransport::Shared::ramBase + 0x740;
        constexpr std::uint32_t rescued = 1U << 20;
        Collected               parked;
        auto const              blockId = [&](bool there) {   // the RTT block's id at ramBase
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            std::memcpy(shared.ram.data(), there ? "SEGGER RTT" : "\0\0\0\0\0\0\0\0\0\0", 10);
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, 0);
            shared.ramReadDelay = 0ms;
        }
        blockId(true);
        auto r = makeReader(
          parked,
          {.block
           = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
           .catalogText = "hello direct",
           .options
           = {.park
              = BasicJLinkRttReader<FakeTransport>::ParkSignature{word, rescued, rescued, 0x30}}});
        CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 3000ms),
              "park: a session runs first");
        auto const resetsBefore = shared.resets.load();
        shared.onReset          = [&]() {   // the printer's reset: the image boots again
            shared.asleep = false;
            blockId(true);
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, 0);
        };
        {   // the reboot and the DLL's rescue: the block gone, the word set, the core (maybe) asleep
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, rescued);
        }
        shared.asleep = asleep;
        blockId(false);
        shared.coreWasReset = true;
        if(asleep) {
            CHECK(waitFor(
                    [&]() {
                        return parked.anyErrorContains("J-Link parked the target in its boot ROM");
                    },
                    4000ms),
                  "park: said as an error");
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 3000ms),
                  "park: the printer resets it");
            CHECK(waitFor(
                    [&]() {
                        return parked.anyErrorContains(
                          "the core asleep as in the park loop at 0x30");
                    },
                    3000ms),
                  "park: the sign named");
        } else {
            CHECK(waitFor([&]() { return parked.anyToolContains("but the core is not asleep"); },
                          4000ms),
                  "park: the word alone is said, not acted on");
            Clock::sleep_for(1500ms);
            CHECK(shared.resets.load() == resetsBefore && !parked.anyErrorContains("J-Link parked"),
                  "park: no reset while the core is awake");
        }
        r.reset();
        shared.onReset = {};
        shared.asleep  = false;
        blockId(true);
    }
}

inline void parkInTheGate() {
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
    {   // a chip parked in its boot ROM is reset at once by the session start's flash read, not polled for the
        // gate's whole wait first: the health checks run once before any polling
        constexpr std::uint32_t word    = FakeTransport::Shared::ramBase + 0x740;
        constexpr std::uint32_t rescued = 1U << 20;
        ssi(0x0007'0000);   // the boot ROM drives the flash
        put(word, rescued);
        shared.asleep                       = true;
        shared.flashReads                   = 0;
        shared.romHung                      = false;
        auto const             resetsBefore = shared.resets.load();
        std::atomic<long long> resetAfterMs{-1};
        auto const             connectedAt = std::make_shared<std::atomic<Clock::time_point>>();
        shared.onReset = [&]() {   // the reset: the image boots and maps its flash
            if(resetAfterMs.load() < 0) {
                resetAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - connectedAt->load())
                                 .count();
            }
            shared.asleep = false;
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.putRam(word, 0);
            shared.putRam(FakeTransport::Shared::ssiWord, 0x005f'0300);
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = false;
        }
        auto r = makeReader(
          flashCheck,
          {
            .block
            = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
            .catalogText = "hello direct",
            .options
            = {.park
               = BasicJLinkRttReader<FakeTransport>::ParkSignature{word, rescued, rescued, 0x30},
                    .flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300}}}
        });
        connectedAt->store(Clock::now());
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 8000ms),
              "park in the gate: reset");
        CHECK(resetAfterMs.load() >= 0 && resetAfterMs.load() < 2000 && !shared.romHung.load(),
              "park in the gate: reset before the gate's wait ran out, no flash read while parked");
        r.reset();
        shared.onReset = {};
        shared.asleep  = false;
        put(word, 0);
    }
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{       "stallSaidAndRecovered",        stallSaidAndRecovered},
  uc_log::test::Case{            "stallWedgedFlash",             stallWedgedFlash},
  uc_log::test::Case{      "stallWordSetNowAndThen",       stallWordSetNowAndThen},
  uc_log::test::Case{    "stallNeedsEveryCondition",     stallNeedsEveryCondition},
  uc_log::test::Case{ "stallLadderAcrossReconnects",  stallLadderAcrossReconnects},
  uc_log::test::Case{              "stallFirstStep",               stallFirstStep},
  uc_log::test::Case{   "stallWhileWaitingForBlock",    stallWhileWaitingForBlock},
  uc_log::test::Case{             "lockupDetection",              lockupDetection},
  uc_log::test::Case{"lockupRecoveryKeepsAnswering", lockupRecoveryKeepsAnswering},
  uc_log::test::Case{             "parkedInBootRom",              parkedInBootRom},
  uc_log::test::Case{               "parkInTheGate",                parkInTheGate},
};

}   // namespace uc_log_test_rtt_reader::recovery
