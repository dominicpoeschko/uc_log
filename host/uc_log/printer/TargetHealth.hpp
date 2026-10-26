#pragma once

// A target that says nothing may be stuck in a way the printer can see in its registers and undo - without halting
// the core (a halt to look hung the printer for minutes). Three signs, checked while the printer waits for the
// control block, while the decode gate holds, and in a session quiet for stallCheckAfter:
// - park: the J-Link DLL parked the chip in its boot ROM during the chip's own reboot (the RP2040's Rescue DP:
//   CHIP_RESET.HAD_PSM_RESTART, the core asleep in the boot ROM's wfi loop). Undone by the printer's reset.
// - lockup: DHCSR.S_LOCKUP on every read of a series, or the core halted at the lockup address (a debugger's
//   connect halts a locked-up core, which clears S_LOCKUP). Undone by the printer's reset.
// - halted in a fault while nothing can be read: core 0 halted (S_HALT, nobody asked) with a fault
//   handler active - the RP2350 boot ROM's HardFault after a watchdog reboot under a reconnecting printer: the J-Link
//   DLL's connect halts a core in lockup, which leaves it in Debug state at the handler instead of at the lockup
//   address. Only while the printer waits for the block or holds the gate: a firmware's own fault halt in a session
//   (its bkpt) is the crash capture's. Undone by the printer's reset.
// - stall: register words the chip package names (the RP2350's QMI left busy by its boot ROM). Undone by the
//   package's register writes; so does any failed download before the next attempt (JLinkRttReader::prepareRetry).
// At most maxRecoveries in a row of each kind; a session that reads the log sets the counts back. Every recovery is
// said as an error, so the next boot's report of the printer's reset is not taken for the target's own.

#include "jlink/JLink.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/Le32.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// The chip's sign that a debug access parked it during its own reboot (a word, a mask, the value it then has).
struct ParkSignature {
    std::uint32_t address;
    std::uint32_t mask;
    std::uint32_t value;
    std::optional<std::uint32_t>
      pc{};   // the park loop's PC (a wfi): the core must also be asleep; nullopt: the word alone
};

/// A stall the chip package recognises from register words: every condition must hold on every read of a short
/// series - several words when one alone could also be a running image's moment. `recovery`: register writes that
/// undo it (e.g. a watchdog reset of every domain); empty: only said.
struct StallSignature {
    struct Condition {
        std::uint32_t address;
        std::uint32_t mask;
        std::uint32_t value;
    };

    std::vector<Condition>          conditions;
    std::vector<JLink::MemoryWrite> recovery{};
};

template<typename Transport>
class TargetHealth {
public:
    using Clock    = uc_log::printer::Clock;
    using MessageF = std::function<void(std::string_view)>;

    static constexpr auto stallCheckAfter = std::chrono::seconds{2};
    static constexpr int  maxRecoveries   = 3;

    /// What a check did. printerReset: the caller resets the target (its ordinary reset); recovered: the package's
    /// writes ran (or a SYSRESETREQ) - the target reboots, the caller stays off the probe a moment and waits for the new
    /// boot's control block; resetLine: the caller resets the target through its reset line.
    enum class Action { none, printerReset, recovered, resetLine };

    /// The chip package's signatures, fixed for the checker's life. `stallFirstStep`: for tests of the stall ladder on a
    /// board - a stall starts at that step (1..maxRecoveries) instead of 1, so the later steps run without a stall that
    /// outlasts the earlier ones.
    TargetHealth(MessageF error                UC_LOG_LIFETIMEBOUND,
                 MessageF tool                 UC_LOG_LIFETIMEBOUND,
                 std::optional<ParkSignature>  park,
                 std::optional<StallSignature> stall,
                 int                           stallFirstStep)
      : error_{std::move(error)}
      , tool_{std::move(tool)}
      , parkSign{std::move(park)}
      , stallSign{std::move(stall)}
      , firstStallStep{std::clamp(stallFirstStep,
                                  1,
                                  maxRecoveries)} {}

    /// A wait begins (for the control block, or behind the gate): the park check may say once why it does not act.
    void beginWait() { parkWordTold_ = false; }

    /// The log is read: the next recovery counts from 1 again.
    void logRead() {
        parkRecoveries_  = 0;
        stallRecoveries_ = 0;
        givenUpTold_     = false;
    }

    /// While waiting for the control block or behind the closed gate, quiet since `since`. `haltRequested`: a client
    /// halted the core (a halt on purpose is left alone).
    Action checkWhileWaiting(Transport&        jlink,
                             Clock::time_point since,
                             bool              haltRequested = false) {
        retryLadderWhenDue();
        if(parked(jlink, since)) { return Action::printerReset; }
        if(auto const a = lockup(jlink, since); a != Action::none) { return a; }
        if(!haltRequested) {
            if(auto const a = faultHalt(jlink, since); a != Action::none) { return a; }
        }
        if(Clock::now() > since + stallCheckAfter) { return stallCheck(jlink, since); }
        return Action::none;
    }

    /// In a session that logged nothing since `quietSince` (at least stallCheckAfter).
    Action checkQuietSession(Transport&        jlink,
                             Clock::time_point quietSince) {
        retryLadderWhenDue();
        if(auto const a = lockup(jlink, quietSince); a != Action::none) { return a; }
        return stallCheck(jlink, quietSince);
    }

    /// A download failed (`why`): on a stalled target the package's recovery runs and the caller downloads again -
    /// at most maxRecoveries for one flash request (flashDone() ends it). True: recovered, download again.
    bool recoverStalledFlash(Transport&       jlink,
                             std::string_view why) {
        auto const& signature = stallSignature();
        if(!signature || signature->recovery.empty() || flashRecoveries_ >= maxRecoveries) {
            return false;
        }
        try {
            if(!stalled(jlink, *signature)) { return false; }
        } catch(std::exception const&) { return false; }
        ++flashRecoveries_;
        error_(fmt::format(
          "the flash failed on a stalled target ({}): running the chip package's stall recovery, "
          "then flashing again ({} of {})",
          why,
          flashRecoveries_,
          maxRecoveries));
        runRecovery(jlink, *signature);
        return true;
    }

    void flashDone() { flashRecoveries_ = 0; }

    /// A download failed (`why`) on a target that shows no stall, and another attempt follows: the package's recovery
    /// runs first - a flash interface can be stuck where no register word shows it, and the writes reset only what the
    /// download resets anyway. True: it ran.
    bool recoverBeforeRetry(Transport&       jlink,
                            std::string_view why) {
        auto const& signature = stallSignature();
        if(!signature || signature->recovery.empty()) { return false; }
        error_(
          fmt::format("the flash failed ({}): running the chip package's stall recovery before the "
                      "next attempt",
                      why));
        runRecovery(jlink, *signature);
        return true;
    }

private:
    // a series over 0.5 s, a read each 50 ms: few probe accesses while a boot ROM may run (fast SSI polling can wedge
    // the RP2040's ROM). The words read have no read side
    // effects (RP2040 SSI CTRLR0/SSIENR RW, TXFLR/RXFLR/SR RO: datasheet 4.10.13, Tables 582-593; RP2350 QMI DIRECT_CSR)
    static constexpr int  seriesReads = 10;
    static constexpr auto seriesGap   = std::chrono::milliseconds{50};

    static long long msSince(Clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
    }

    [[nodiscard]] std::optional<ParkSignature> const& parkSignature() const UC_LOG_LIFETIMEBOUND {
        return parkSign;
    }

    [[nodiscard]] std::optional<StallSignature> const& stallSignature() const UC_LOG_LIFETIMEBOUND {
        return stallSign;
    }

    // The PC check reads DHCSR.S_SLEEP, not the PC: a halt to read the PC can hang the probe, and would cost a healthy
    // image. The word alone stays set from the first rescue to
    // the next power-up, so with a PC given the core must be asleep too.
    bool parked(Transport&        jlink,
                Clock::time_point since) {
        auto const& signature = parkSignature();
        if(!signature || parkRecoveries_ >= maxRecoveries) { return false; }
        auto const word = tryReadWord(jlink, signature->address);
        if(!word || (*word & signature->mask) != signature->value) { return false; }
        std::string where;
        if(signature->pc) {
            auto const dhcsr = tryReadWord(jlink, cortex_m::dhcsr);
            if(!dhcsr) { return false; }
            if((*dhcsr & cortex_m::dhcsrSleep) == 0) {
                if(!parkWordTold_) {
                    parkWordTold_ = true;
                    tool_(fmt::format(
                      "park check: {:#010x} & {:#x} matches, but the core is not asleep "
                      "(DHCSR {:#010x}; the boot ROM's park loop is a wfi at {:#x})",
                      signature->address,
                      signature->mask,
                      *dhcsr,
                      *signature->pc));
                }
                return false;
            }
            where = fmt::format(", the core asleep as in the park loop at {:#x}", *signature->pc);
        }
        ++parkRecoveries_;
        error_(
          fmt::format("J-Link parked the target in its boot ROM (the DLL's recovery after a reboot "
                      "under an open "
                      "session; {:#010x} & {:#x} == {:#x}{}, nothing to read for {} ms): resetting "
                      "it ({} of {}) - "
                      "the next boot reports the printer's reset, not the reboot the target made",
                      signature->address,
                      signature->mask,
                      signature->value,
                      where,
                      msSince(since),
                      parkRecoveries_,
                      maxRecoveries));
        return true;
    }

    // halted at the lockup address - 0xFFFFFFFE (Armv6-M DDI0419E B1.5.15, Armv7-M DDI0403E B1.5.15) or
    // 0xEFFFFFFE (Armv8-M DDI0553B.y B3.33) - or S_LOCKUP on every read of a series.
    bool lockedUp(Transport& jlink) {
        try {
            if(jlink.isHalted()) {
                auto const pc = jlink.readRegister(Transport::CoreRegister::pc) | 1U;
                return pc == 0xFFFF'FFFFU || pc == 0xEFFF'FFFFU;
            }
        } catch(std::exception const&) { return false; }
        for(int i = 0; i != seriesReads; ++i) {
            auto const dhcsr = tryReadWord(jlink, cortex_m::dhcsr);
            if(!dhcsr || (*dhcsr & cortex_m::dhcsrLockup) == 0) { return false; }
            if(i + 1 != seriesReads) { Clock::sleep_for(seriesGap); }
        }
        return true;
    }

    Action lockup(Transport&        jlink,
                  Clock::time_point quietSince) {
        if(!lockedUp(jlink)) { return Action::none; }
        bool const recover = stallRecoveries_ < maxRecoveries;
        if(!recover && givenUpTold_) { return Action::none; }
        givenUpTold_ = !recover;
        error_(fmt::format(
          "the target's core is in lockup (DHCSR.S_LOCKUP, or halted at the lockup address: a "
          "fault "
          "it could not take, e.g. in its boot ROM), nothing to read for {} ms{}",
          msSince(quietSince),
          recover ? fmt::format(": resetting it ({} of {})", stallRecoveries_ + 1, maxRecoveries)
                  : std::string{": reset too often in a row - tried again in 30 s"}));
        if(!recover) { return Action::none; }
        ++stallRecoveries_;
        return Action::printerReset;
    }

    // Halted in a fault handler: IPSR (xPSR 8:0) is HardFault (3) .. SecureFault (7) - Armv8-M ARM D1.2.120, Armv6-M
    // ARM B1.4.2 (3 only there). Counted with the lockups (one streak of printer resets).
    Action faultHalt(Transport&        jlink,
                     Clock::time_point since) {
        std::uint32_t pc{};
        std::uint32_t exception{};
        try {
            if(!jlink.isHalted()) { return Action::none; }
            exception = jlink.readRegister(Transport::CoreRegister::xpsr) & 0x1FFU;
            pc        = jlink.readRegister(Transport::CoreRegister::pc);
        } catch(std::exception const&) { return Action::none; }
        if(exception < 3 || exception > 7) { return Action::none; }
        bool const recover = stallRecoveries_ < maxRecoveries;
        if(!recover && givenUpTold_) { return Action::none; }
        givenUpTold_ = !recover;
        error_(fmt::format(
          "the target's core is halted in a fault handler (exception {}, pc {:#010x}) with nothing "
          "to "
          "read for {} ms - nobody asked for the halt (the J-Link DLL's connect halts a core in "
          "lockup){}",
          exception,
          pc,
          msSince(since),
          recover ? fmt::format(": resetting it ({} of {})", stallRecoveries_ + 1, maxRecoveries)
                  : std::string{": reset too often in a row - tried again in 30 s"}));
        if(!recover) { return Action::none; }
        ++stallRecoveries_;
        return Action::printerReset;
    }

    // every condition on every read of the series (a busy flag that is set now and then in normal running is no stall)
    bool stalled(Transport&            jlink,
                 StallSignature const& signature) {
        if(signature.conditions.empty()) { return false; }
        for(int i = 0; i != seriesReads; ++i) {
            for(auto const& c : signature.conditions) {
                auto const word = tryReadWord(jlink, c.address);
                if(!word || (*word & c.mask) != c.value) { return false; }
            }
            if(i + 1 != seriesReads) { Clock::sleep_for(seriesGap); }
        }
        return true;
    }

    Action stallCheck(Transport&        jlink,
                      Clock::time_point quietSince) {
        auto const& signature = stallSignature();
        if(!signature || !stalled(jlink, *signature)) { return Action::none; }
        // A ladder, counted across sessions and connections (only logRead() sets it back): the package's recovery, a
        // SYSRESETREQ in this session, the reset line, then a power cycle asked for; after stallLadderPause it starts
        // again.
        stallRecoveries_    = std::max(stallRecoveries_, firstStallStep - 1);
        bool const  recover = !signature->recovery.empty() && stallRecoveries_ < maxRecoveries;
        std::string words;
        for(auto const& c : signature->conditions) {
            words += fmt::format("{}{:#010x} & {:#x} == {:#x}",
                                 words.empty() ? "" : " and ",
                                 c.address,
                                 c.mask,
                                 c.value);
        }
        error_(fmt::format(
          "the target is stalled: {} on every read for {} ms, and no log for {} s (the chip "
          "package's sign of a stall, "
          "e.g. its boot ROM waiting on the flash interface){}",
          words,
          seriesReads * seriesGap.count(),
          msSince(quietSince) / 1000,
          recover
            ? fmt::format(": {} ({} of {})",
                          stallSteps[static_cast<std::size_t>(stallRecoveries_)],
                          stallRecoveries_ + 1,
                          maxRecoveries)
            : std::string{signature->recovery.empty()
                            ? ": no recovery known - reset or power-cycle it"
                            : ": power-cycle it (unplug its USB) - the printer tries again"}));
        if(!recover) { return Action::none; }
        switch(stallRecoveries_++) {
        case 0: runRecovery(jlink, *signature); return Action::recovered;
        case 1: try { jlink.writeWord(cortex_m::aircr, cortex_m::aircrSysResetReq);
            } catch(std::exception const&) { /* ignored: the reset may cut the answer */
            }
            return Action::recovered;
        default: return Action::resetLine;
        }
    }

    static constexpr std::array<std::string_view, 3> stallSteps{
      "running its recovery",
      "resetting it by SYSRESETREQ",
      "resetting it through its reset line"};
    static constexpr auto stallLadderPause = std::chrono::seconds{30};

    // a ladder that ended (lockup / fault-halt resets, stall steps) starts again after stallLadderPause: every error is
    // retried for good, never left as it is
    void retryLadderWhenDue() {
        if(stallRecoveries_ < maxRecoveries) { return; }
        if(stallGaveUpAt_ == Clock::time_point{}) {
            stallGaveUpAt_ = Clock::now();
            return;
        }
        if(Clock::now() > stallGaveUpAt_ + stallLadderPause) {
            stallRecoveries_ = 0;
            stallGaveUpAt_   = {};
        }
    }

    static void runRecovery(Transport&            jlink,
                            StallSignature const& signature) {
        for(auto const& w : signature.recovery) {
            try {
                jlink.writeWord(w.address, w.value);
            } catch(std::exception const&) { /* ignored: the chip may reset under the write */
            }
        }
    }

    MessageF                            error_;
    MessageF                            tool_;
    std::optional<ParkSignature> const  parkSign;
    std::optional<StallSignature> const stallSign;
    int const                           firstStallStep;
    // the reader thread's
    int               parkRecoveries_{};
    int               stallRecoveries_{};   // lockups and stalls, in a row
    Clock::time_point stallGaveUpAt_{};   // when the ladder ended (or a power cycle was asked for)
    int               flashRecoveries_{};   // for the flash in hand
    bool              parkWordTold_{};
    bool              givenUpTold_{};
};

}   // namespace uc_log::printer
