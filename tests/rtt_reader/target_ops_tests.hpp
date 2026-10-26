#pragma once

// rtt_reader tests: target operations through the reader and halts: memory, core registers, the TUI's buttons, captured
// halts. Included only by rtt_reader_tests.cpp (one binary for every rtt_reader case); the fake J-Link and the shared
// pieces: support/FakeJLink.hpp.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"

#include <array>

namespace uc_log_test_rtt_reader::target_ops {

inline void accessTargetReadsAndWrites() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    {
        std::array<uc_log::printer::TargetOp, 2> const pieces{
          uc_log::printer::target_op::Read{0x20000010, 4},
          uc_log::printer::target_op::Read{0x200000FE, 3}
        };
        auto const result = reader.accessTarget(pieces, 2000ms);
        CHECK(result.has_value(), "memory read through the reader thread");
        if(result) {
            CHECK(result->size() == 2 && (*result)[0].size() == 4 && (*result)[1].size() == 3,
                  "one buffer per piece, of its size");
            CHECK((*result)[0][0] == std::byte{0x10} && (*result)[0][3] == std::byte{0x13}
                    && (*result)[1][2] == std::byte{0x00},
                  "the fake target's bytes: the low byte of each address");
        }

        std::array<uc_log::printer::TargetOp, 2> const writes{
          uc_log::printer::target_op::WriteWord{0x5000'000C, 0U},
          uc_log::printer::target_op::WriteWord{0x5000'0010, 7U}
        };
        auto const written = reader.accessTarget(writes, 2000ms);
        CHECK(written.has_value() && written->size() == 2 && (*written)[1].size() == 4
                && (*written)[1][0] == std::byte{0x10},
              "a word per write, read back from the target");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            CHECK((shared.writes
                   == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x5000'000CU, 0U},
                                                                          {0x5000'0010U, 7U}}),
                  "both words written, in order");
        }

        // A write the reader thread has started is not reported as timed out: it happens, so the caller hears
        // that it did (a slow probe once made a poke "time out" and still take effect).
        shared.slowWriteMs = 400;
        std::array<uc_log::printer::TargetOp, 1> const slow{
          uc_log::printer::target_op::WriteWord{0x5000'0014, 9U}
        };
        auto const late    = reader.accessTarget(slow, 100ms);
        shared.slowWriteMs = 0;
        bool written9      = false;
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            written9
              = !shared.writes.empty()
             && shared.writes.back() == std::pair<std::uint32_t, std::uint32_t>{0x5000'0014U, 9U};
        }
        CHECK(written9, "the slow write happened");
        CHECK(late.has_value(), "and the caller is told it did, not that it timed out");
    }
}

inline void coreOpsThroughProbe() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    {
        // halt and resume through the probe's own calls; DHCSR writes refused (they hung the J-Link DLL)
        namespace to    = uc_log::printer::target_op;
        using Access    = uc_log::printer::TargetOp;
        auto const core = [&](Access c) {
            std::array<Access, 1> const op{c};
            return reader.accessTarget(op, 2000ms);
        };
        auto const running = core(to::ReadRegisters{});
        CHECK(!running && running.error().contains("halt it first"),
              "registers of a running core: refused");
        auto const halted = core(to::Halt{});
        auto const polls  = shared.haltPolls.load();
        CHECK(
          waitFor([&]() { return shared.haltPolls.load() >= polls + 3; }, 3000ms)
            && !collected.anyErrorContains("core halted") &&
            [&] {
                std::scoped_lock<std::mutex> const lock{collected.mutex};
                return std::ranges::any_of(collected.tools, [](auto const& t) {
                    return t.contains("halt request");
                });
            }(),
          "a requested halt is a tool message, not a 'core halted' error");
        auto const regs = core(to::ReadRegisters{});
        auto const word = [&](std::size_t i) {
            std::uint32_t v = 0;
            for(std::size_t b = 0; b != 4; ++b) {
                v |= std::to_integer<std::uint32_t>(regs->front()[i * 4 + b]) << (8 * b);
            }
            return v;
        };
        CHECK(regs && regs->front().size() == 19 * 4 && word(0) == 0x1000U
                && word(13) == 0x2000'0100U && word(15) == 0x1000U + 15U
                && word(18) == 0x1000U + 18U,
              "registers of a halted core: r0..r12, sp, lr, pc, xpsr, msp, psp from the probe");
        std::array<Access, 2> const writes{
          to::WriteRegister{ 0,      0x1234U},
          to::WriteRegister{15, 0x1000'2000U}
        };
        auto const wrote = reader.accessTarget(writes, 2000ms);
        auto const again = core(to::ReadRegisters{});
        CHECK(wrote && again && std::to_integer<std::uint32_t>(again->front()[0]) == 0x34U
                && std::to_integer<std::uint32_t>(again->front()[15 * 4 + 1]) == 0x20U,
              "registers written through the probe's own calls read back");
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.writtenRegisters = {};   // the halts below expect the fake's own values
        }
        CHECK(halted && halted->size() == 1 && halted->front().front() == std::byte{1}
                && shared.halted,
              "halt: the core halted, said as 1");
        auto const                  ran = core(to::Go{});
        std::array<Access, 1> const runningWrite{
          to::WriteRegister{0, 1U}
        };
        auto const refusedWrite = reader.accessTarget(runningWrite, 2000ms);
        CHECK(!refusedWrite && refusedWrite.error().contains("halt it first"),
              "a register write to a running core: refused");
        CHECK(ran && ran->front().front() == std::byte{0} && !shared.halted,
              "go: the core runs, said as 0");
        auto const goes = shared.goCalls.load();
        CHECK(core(to::Go{}).has_value() && shared.goCalls.load() == goes,
              "go on a running core does not call the probe's go (it hung on one)");
        std::array<Access, 1> const dhcsr{
          to::WriteWord{0xE000'EDF0U, 0xA05F0003U}
        };
        auto const refused = reader.accessTarget(dhcsr, 2000ms);
        CHECK(!refused && refused.error().contains("DHCSR") && !shared.halted,
              "a DHCSR write is refused, and nothing is halted");
    }
}

inline void haltsCaptured() {
    Scenario                    s;
    auto&                       shared = s.shared;
    ReaderUnderTest             under{};
    [[maybe_unused]] Reader&    reader    = *under.reader;
    [[maybe_unused]] Collected& collected = under.said;
    {
        // the halts of this block only
        auto const firstError = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            return collected.errors.size();
        }();
        auto const haltEntries = [&]() {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            std::vector<std::string>           found;
            for(std::size_t i = firstError; i < collected.errors.size(); ++i) {
                if(collected.errors[i].contains("core halted")) {
                    found.push_back(collected.errors[i]);
                }
            }
            return found;
        };
        // the reader has polled isHalted() at least n more times
        auto const polled = [&](int n) {
            auto const from = shared.haltPolls.load();
            return waitFor([&]() { return shared.haltPolls.load() >= from + n; }, 3000ms);
        };

        shared.halted = true;
        CHECK(waitFor([&]() { return !haltEntries().empty(); }, 3000ms),
              "halt captured as a status error");
        CHECK(polled(20), "the reader polls on while halted");
        auto entries = haltEntries();
        CHECK(entries.size() == 1, "one entry per halt, not one per poll");
        std::string const entry = entries.empty() ? std::string{} : entries.front();
        CHECK(entry.contains("pc=0x0000100f") && entry.contains("sp=0x20000100"),
              "the core registers");
        CHECK(entry.contains("stack: 03020100 07060504"), "the top of the stack");
        {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            CHECK(collected.halts.size() == 1 && collected.halts.front().text() == entry
                    && collected.halts.front().pc == 0x100fU
                    && collected.halts.front().sp == 0x2000'0100U
                    && !collected.halts.front().stack.empty()
                    && collected.halts.front().stack.front() == 0x0302'0100U,
                  "the halt as a struct, its text the error's");
        }
        CHECK(!collected.anyEntryContains("core halted"),
              "the log holds the target's lines only: no halt entry in it");
        CHECK(reader.isHalted(), "isHalted()");
        shared.halted = false;
        CHECK(polled(2), "the reader saw the core run again");

        // SP outside RAM: the stack reads fail, the entry is still written, the session lives
        auto const sessions      = reader.sessionCount();
        auto const constructions = shared.constructions.load();
        shared.stackUnreadable   = true;
        shared.halted            = true;
        CHECK(waitFor([&]() { return haltEntries().size() == 2; }, 3000ms),
              "a halt with an unreadable stack is still reported");
        CHECK(polled(20), "the reader polls on after an unreadable stack");
        entries = haltEntries();
        CHECK(entries.size() == 2, "still one entry per halt");
        std::string const unreadable = entries.size() == 2 ? entries.back() : std::string{};
        CHECK(unreadable.contains("pc=0x0000100f") && unreadable.contains("lr=0x0000100e"),
              "pc and lr are reported without the stack");
        CHECK(unreadable.contains("stack: ???????? ????????"), "unreadable words are marked");
        {
            std::scoped_lock<std::mutex> const lock{collected.mutex};
            CHECK(collected.halts.size() == 2 && collected.halts.back().text() == unreadable
                    && std::ranges::none_of(collected.halts.back().stack,
                                            [](auto const& w) { return w.has_value(); }),
                  "unreadable words are nullopt in the struct");
        }
        CHECK(reader.sessionCount() == sessions && shared.constructions.load() == constructions,
              "no session restart");
        shared.halted          = false;
        shared.stackUnreadable = false;
        CHECK(polled(2), "the reader saw the core run again");

        // halt request, then go into an immediate bkpt (the TUI's and kvasir_bench's panic): a new halt, captured
        namespace to = uc_log::printer::target_op;
        std::array<uc_log::printer::TargetOp, 1> const halt{to::Halt{}};
        std::array<uc_log::printer::TargetOp, 1> const go{to::Go{}};
        CHECK(reader.accessTarget(halt, 2000ms).has_value() && polled(3), "halted by a request");
        shared.haltsOnGo = true;
        auto const ran   = reader.accessTarget(go, 2000ms);
        CHECK(ran && waitFor([&]() { return haltEntries().size() == 3; }, 3000ms),
              "a halt right after a requested go is captured");
        shared.haltsOnGo = false;
        shared.halted    = false;
        CHECK(polled(2), "the reader saw the core run again");
    }
}

// The TUI's debugger buttons through the one queue: its halt is captured (`core halted:`, as a halt the core made
// itself), a client's is not; breakpoints and the reset type go through it too.
inline void tuiCoreOps() {
    Scenario        s;
    auto&           shared = s.shared;
    ReaderUnderTest under{};
    Reader&         reader    = *under.reader;
    Collected&      collected = under.said;
    namespace to              = uc_log::printer::target_op;
    auto const halts          = [&]() {
        std::scoped_lock<std::mutex> const lock{collected.mutex};
        auto const byText = std::ranges::count_if(collected.errors, [](std::string const& e) {
            return e.starts_with("core halted:");
        });
        // the kind and the text agree
        return byText == static_cast<std::ptrdiff_t>(collected.halts.size()) ? byText : -1;
    };
    auto const polled = [&](int n) {
        auto const from = shared.haltPolls.load();
        return waitFor([&]() { return shared.haltPolls.load() >= from + n; }, 3000ms);
    };
    auto const one = [&](uc_log::printer::TargetOp const& op) {
        std::array<uc_log::printer::TargetOp, 1> const ops{op};
        return reader.accessTarget(ops, 2000ms);
    };
    CHECK(waitFor([&]() { return reader.sessionCount() != 0; }, 3000ms), "a session runs");

    CHECK(one(to::Halt{.capture = true}).has_value()
            && waitFor([&]() { return halts() == 1; }, 3000ms),
          "a TUI halt is captured as a halt of the core's own");
    CHECK(one(to::Go{}).has_value() && polled(3), "resumed");
    CHECK(one(to::Halt{}).has_value() && polled(5) && halts() == 1,
          "a client's halt is not captured");
    CHECK(one(to::Go{}).has_value() && polled(3), "resumed again");

    auto const clears = shared.breakpointClears.load();
    CHECK(one(to::ClearBreakpoints{}).has_value() && shared.breakpointClears.load() == clears + 1,
          "breakpoints cleared through the queue");
    CHECK(one(to::SetResetType{1}).has_value() && shared.resetTypeSet.load() == 1,
          "the reset type set through the queue");
    auto const resets = shared.resets.load();
    reader.resetTarget();
    CHECK(waitFor([&]() { return shared.resets.load() > resets; }, 3000ms)
            && shared.resetTypeSet.load() == 1,
          "and the printer's ordinary reset keeps it");
}

// The halt state under a flood: read every busyHaltPoll, not every haltPoll (each read is a probe round trip of a
// pass), yet a core that halts while another floods on is still seen before RttChannel's frame timeout (100 ms):
// a core halted mid-frame must not have its frame resynced away. Quiet again: back to the haltPoll rate.
inline void haltPollRateUnderFlood() {
    Scenario s;
    auto&    shared = s.shared;
    {
        static constexpr std::uint32_t block = FakeTransport::Shared::ramBase;
        RamTarget                      target{shared, 0x800, oneKiBRing};
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 1ms;
            shared.halted       = false;
        }
        // the flood: the ring is refilled right after each read-offset write, so every pass finds bytes however
        // loaded the machine is (a starved producer thread would leave passes empty, which poll at haltPoll)
        auto const refill = [&target, frame = makeFrame(0)]() {   // with shared.mutex held
            static_cast<void>(target.rtt.fillWith(0, frame));
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            refill();
            shared.afterRamWrite = refill;
        }
        Collected busy;
        auto      r = makeReader(busy,
                                 {.block = []() { return uc_log::printer::RttBlockInfo{block, 1}; },
                                  .catalogText = "hello busy"});
        auto const entries = [&]() {
            std::scoped_lock<std::mutex> const lock{busy.mutex};
            return busy.entries.size();
        };
        // the halt polls within `span`
        auto const pollsIn = [&](std::chrono::milliseconds span) {
            auto const from = shared.haltPolls.load();
            Clock::sleep_for(span);
            return shared.haltPolls.load() - from;
        };
        CHECK(waitFor([&]() { return entries() > 200; }, 5000ms),
              "busy: the reader decodes the flood");
        auto const busyPolls = pollsIn(1000ms);
        CHECK(busyPolls >= 5 && busyPolls <= 25,
              "busy: the halt state every ~50 ms, not every pass");
        if(busyPolls < 5 || busyPolls > 25) {
            std::printf("  halt polls in 1 s of flood: %d\n", busyPolls);
        }
        // a core halts while the flood goes on (core 1 beside a core 0 on its breakpoint); the worst of 5 halts
        auto const halts = [&]() {
            std::scoped_lock<std::mutex> const lock{busy.mutex};
            return std::count_if(busy.errors.begin(), busy.errors.end(), [](std::string const& e) {
                return e.contains("core halted");
            });
        };
        Clock::duration worst{};
        for(int i = 0; i != 5; ++i) {
            auto const before = halts();
            auto const t0     = Clock::now();
            shared.halted     = true;
            CHECK(waitFor([&]() { return halts() > before; }, 3000ms),
                  "busy: a halt under a flood is captured");
            worst         = std::max(worst, Clock::now() - t0);
            shared.halted = false;
            Clock::sleep_for(120ms);   // seen running again, busy polling again
        }
        CHECK(worst < uc_log::printer::RttChannel::frameTimeout,
              "busy: every halt is seen before a stuck frame times out");
        if(worst >= uc_log::printer::RttChannel::frameTimeout) {
            std::printf("  a halt seen only after %lld ms\n",
                        static_cast<long long>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(worst).count()));
        }
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.afterRamWrite = nullptr;
        }
        Clock::sleep_for(200ms);   // the ring read empty
        // ~90 a second at haltPoll on an idle machine; at busyHaltPoll it could not pass 21
        auto const quietPolls = pollsIn(1000ms);
        CHECK(quietPolls >= 30, "quiet: the halt state every ~10 ms again");
        if(quietPolls < 30) { std::printf("  halt polls in 1 s quiet: %d\n", quietPolls); }
        r.reset();
        CHECK(!busy.anyErrorContains("corrupted") && !busy.anyErrorContains("resync"),
              "busy: no damaged frame");
    }
}

// A core halted in a fault handler while the printer waits for the block (the RP2350 boot ROM's HardFault after a
// watchdog reboot: the J-Link DLL's connect halts a core in lockup and leaves it in Debug state at the handler):
// nobody asked for the halt, nothing can be read - said and reset. A halt a client asked for is left alone.
inline void faultHaltWhileWaiting() {
    for(bool const requested : {false, true}) {
        Scenario   s;
        auto&      shared = s.shared;
        RamTarget  target{shared, 0x800, oneKiBRing};
        Collected  faulted;
        auto const blockId = [&](bool there) {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            std::memcpy(shared.ram.data(), there ? "SEGGER RTT" : "\0\0\0\0\0\0\0\0\0\0", 10);
        };
        auto const faultRegisters = [&](bool on) {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            using R = FakeTransport::CoreRegister;
            shared.writtenRegisters[static_cast<std::size_t>(R::xpsr)]
              = on ? std::optional<std::uint32_t>{0x0100'0003U} : std::nullopt;
            shared.writtenRegisters[static_cast<std::size_t>(R::pc)]
              = on ? std::optional<std::uint32_t>{0x2f8U} : std::nullopt;
        };
        {
            std::scoped_lock<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
        }
        blockId(true);
        auto r = makeReader(
          faulted,
          {.block
           = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
           .catalogText = "hello direct"});
        CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 3000ms),
              "fault halt: a session runs first");
        if(requested) {
            std::array<uc_log::printer::TargetOp, 1> const halt{uc_log::printer::target_op::Halt{}};
            CHECK(r->accessTarget(halt, 2000ms).has_value() && shared.halted,
                  "fault halt: a client halts the core");
        }
        auto const resetsBefore = shared.resets.load();
        shared.onReset          = [&]() {   // the printer's reset: the image boots again
            shared.halted = false;
            faultRegisters(false);
            blockId(true);
        };
        faultRegisters(
          true);   // the reboot: the block gone, the core halted at the boot ROM's HardFault
        shared.halted = true;
        blockId(false);
        shared.coreWasReset = true;
        if(!requested) {
            bool const said = waitFor(
              [&]() { return faulted.anyErrorContains("halted in a fault handler (exception 3"); },
              5000ms);
            CHECK(said, "fault halt: said as an error");
            if(!said) { faulted.dump(); }
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 3000ms),
                  "fault halt: the printer resets it");
            CHECK(waitFor([&]() { return r->sessionCount() >= 2; }, 5000ms),
                  "fault halt: the log comes back");
        } else {
            Clock::sleep_for(3000ms);
            CHECK(shared.resets.load() == resetsBefore
                    && !faulted.anyErrorContains("halted in a fault handler"),
                  "fault halt: a halt a client asked for is left alone");
        }
        r.reset();
        shared.onReset = {};
        shared.halted  = false;
        faultRegisters(false);
        blockId(true);
    }
}

// The TUI's core buttons while the reader waits for a new boot's control block (Q5.14-f: they are TargetOps on the
// one queue, served in that wait too): Halt is answered, the halted core is not taken for a stall or a fault - it is
// left alone, no reset - and Go resumes; once the block is back the log comes back.
inline void tuiHaltWhileWaitingForBlock() {
    Scenario   s;
    auto&      shared = s.shared;
    RamTarget  target{shared, 0x800, oneKiBRing};
    Collected  said;
    auto const blockId = [&](bool there) {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        std::memcpy(shared.ram.data(), there ? "SEGGER RTT" : "\0\0\0\0\0\0\0\0\0\0", 10);
    };
    {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.ramReadDelay = 0ms;
    }
    blockId(true);
    auto r = makeReader(
      said,
      {.block = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
       .catalogText = "hello direct"});
    CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 3000ms), "a session runs first");
    // a reboot into something without the block (its boot ROM): the reader waits for it
    blockId(false);
    shared.coreWasReset = true;
    CHECK(waitFor([&]() { return said.anyMessageContains("waiting for the RTT control block"); },
                  5000ms),
          "the reader waits for the block");
    namespace to   = uc_log::printer::target_op;
    auto const one = [&](uc_log::printer::TargetOp const& op) {
        std::array<uc_log::printer::TargetOp, 1> const ops{op};
        return r->accessTarget(ops, 2000ms);
    };
    auto const resets = shared.resets.load();
    auto const halted = one(to::Halt{.capture = true});
    CHECK(halted && !halted->empty() && !halted->front().empty()
            && halted->front()[0] == std::byte{1} && shared.halted,
          "a TUI halt while waiting is answered: halted");
    Clock::sleep_for(3000ms);   // health checks run meanwhile
    CHECK(shared.resets.load() == resets && !said.anyErrorContains("halted in a fault handler"),
          "a core halted by hand, not in a fault, is left alone while waiting");
    auto const went = one(to::Go{});
    CHECK(went && !went->empty() && went->front()[0] == std::byte{0} && !shared.halted,
          "Go resumes it");
    auto const sessions = r->sessionCount();
    blockId(true);
    CHECK(waitFor([&]() { return r->sessionCount() > sessions; }, 5000ms),
          "the block back: a new session");
    r.reset();
}

// the cases in their order (rtt_reader_tests.cpp runs every file's table; CMakeLists.txt CASES lists them)
inline constexpr std::array cases{
  uc_log::test::Case{ "accessTargetReadsAndWrites",  accessTargetReadsAndWrites},
  uc_log::test::Case{        "coreOpsThroughProbe",         coreOpsThroughProbe},
  uc_log::test::Case{              "haltsCaptured",               haltsCaptured},
  uc_log::test::Case{                 "tuiCoreOps",                  tuiCoreOps},
  uc_log::test::Case{"tuiHaltWhileWaitingForBlock", tuiHaltWhileWaitingForBlock},
  uc_log::test::Case{     "haltPollRateUnderFlood",      haltPollRateUnderFlood},
  uc_log::test::Case{      "faultHaltWhileWaiting",       faultHaltWhileWaiting},
};

}   // namespace uc_log_test_rtt_reader::target_ops
