#pragma once
#include "jlink/JLink.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/detail/AnnouncedReset.hpp"
#include "uc_log/detail/BootLedger.hpp"
#include "uc_log/detail/DecodeWorker.hpp"
#include "uc_log/detail/DirectRtt.hpp"
#include "uc_log/detail/Le32.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/RttChannel.hpp"
#include "uc_log/detail/RttChannelMap.hpp"
#include "uc_log/detail/RttControlBlock.hpp"
#include "uc_log/detail/RttSource.hpp"
#include "uc_log/detail/TargetHealth.hpp"
#include "uc_log/detail/TargetOp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Templated on the transport so the reader loop (framing, liveness, overflow reporting,
// reconnect policy) is testable with an in-memory fake instead of the JLink DLL.
template<typename TransportT = JLink>
struct BasicJLinkRttReader {
    /// Signs of a stuck target the chip package names (detail/TargetHealth.hpp); set with setParkSignature /
    /// setStallSignature.
    using ParkSignature  = uc_log::detail::ParkSignature;
    using StallSignature = uc_log::detail::StallSignature;

private:
    using Clock  = std::chrono::steady_clock;
    using Status = typename TransportT::Status;

public:
    using Probe = typename TransportT::Probe;

    /// What other threads ask of the target (detail/TargetOp.hpp), and the answer: bytes per op, or why not.
    using TargetOp     = uc_log::detail::TargetOp;
    using TargetResult = std::expected<std::vector<std::vector<std::byte>>, std::string>;

    /// Reads on the reader thread itself (setOnSessionStart); false = unreadable.
    using DirectMemoryRead = std::function<bool(std::uint32_t, std::span<std::byte>)>;

    // What the transport is told to open: `host` non-empty means J-Link over IP at that
    // address, otherwise `probe` names a J-Link by serial number or nickname (empty: the
    // only one on USB).
    struct ConnectionSettings {
        std::string host{};
        std::string probe{};
    };

private:
    static constexpr std::size_t RttBufferChunkSize = 32768;
    static constexpr std::size_t DuplexChunkSize    = 4096;
    static constexpr auto        HaltPoll           = std::chrono::milliseconds{10};
    // J-Link reset type 2: through the probe's nRESET pin (the reset line; the stall ladder's step 3 uses it too)
    static constexpr std::uint8_t ResetTypeLine = 2;
    // The halt/S_RESET_ST poll interval while passes read bytes: each poll is a probe round trip, and a target that
    // halts or resets stops filling the rings, so the next empty pass brings back HaltPoll. Kept a pass below the
    // frame timeout: a core halted mid-frame beside one that floods on must be seen as halted before its frame is
    // resynced away (RttChannel::drain resyncs only while no halt was seen).
    static constexpr auto BusyHaltPoll = std::chrono::milliseconds{50};
    static_assert(BusyHaltPoll + std::chrono::milliseconds{30}
                  < uc_log::detail::RttChannel::FrameTimeout);
    using Health                                   = uc_log::detail::TargetHealth<TransportT>;
    static constexpr auto          StallCheckAfter = Health::StallCheckAfter;
    static constexpr auto          HaltGracePeriod = std::chrono::seconds{60};
    static constexpr std::uint32_t HaltStackWords  = 24;
    static constexpr unsigned      FlashAttempts   = 3;
    static constexpr auto          RetryBootTime = std::chrono::milliseconds{500};   // prepareRetry
    // DHCSR at 0xE000EDF0, bit 25 S_RESET_ST: at least one reset since the last read of the
    // DHCSR, cleared by that read. Armv6-M ARM DDI 0419E C1.6.3 (Table C1-10 for the address),
    // Armv7-M ARM DDI 0403E.e C1.6.2, and for the M33 the RP2350 data sheet 3.7, Table 252.
    static constexpr std::uint32_t DhcsrAddress     = 0xE000'EDF0;
    static constexpr std::uint32_t CpuidAddress     = 0xE000'ED00;
    static constexpr std::uint32_t DhcsrResetSt     = 1U << 25;
    static constexpr auto          ControlBlockPoll = std::chrono::milliseconds{20};
    // An announced reset (uc_log/detail/AnnouncedReset.hpp, Kvasir_SDK Util/AnnouncedReset.hpp):
    // how often the block is read (the firmware waits >= 0.5 s for the ack), how long the
    // target gets to put its last lines into the rings before they are drained, and how long
    // the probe stays off the chip after the ack (the RP2040's reboot into its boot ROM or the
    // next image takes a few ms; the firmware's grace after the ack is ~0.1 s).
    static constexpr auto AnnouncedResetPoll = std::chrono::milliseconds{10};
    // how often a session that found no announced-reset block looks for it again, and how long
    // a session start waits for it (a boot copies .data a few ms after the reset)
    static constexpr auto AnnouncedResetArmRetry = std::chrono::milliseconds{20};
    static constexpr auto AnnouncedResetArmWait  = std::chrono::milliseconds{100};
    static constexpr auto AnnouncedResetDrain    = std::chrono::milliseconds{20};

public:
    static constexpr auto AnnouncedResetPause = std::chrono::milliseconds{1500};
    // the longest stay-away a firmware's request is granted (its field holds up to 6553.5 s)
    static constexpr auto AnnouncedResetPauseMax = std::chrono::milliseconds{600'000};   // 10 min

private:
    template<typename Transport>
    static std::optional<uc_log::detail::announced_reset::Block>
    readAnnouncedResetBlock(Transport&    jlink,
                            std::uint32_t address) {
        std::array<std::byte, uc_log::detail::announced_reset::BlockSize> raw{};
        try {
            jlink.readMemory(address, raw);
        } catch(std::exception const&) { return std::nullopt; }
        return uc_log::detail::announced_reset::decode(raw);
    }

    // true when the core was reset since the last look (reading clears the bit - a look the flash reads took counts:
    // resetSeenAside_); a failed read says no
    template<typename Transport>
    bool resetSinceLastLook(Transport& jlink) {
        bool const aside = std::exchange(resetSeenAside_, false);
        auto const dhcsr = uc_log::detail::tryReadWord(jlink, DhcsrAddress);
        return aside || (dhcsr && (*dhcsr & DhcsrResetSt) != 0);
    }

    // A flash read's look at DHCSR.S_RESET_ST: set -> the bit is kept for resetSinceLastLook, and true.
    template<typename Transport>
    bool resetDuringFlashRead(Transport& jlink) {
        auto const dhcsr = uc_log::detail::tryReadWord(jlink, DhcsrAddress);
        if(!dhcsr || (*dhcsr & DhcsrResetSt) == 0) { return false; }
        resetSeenAside_ = true;
        return true;
    }

    // the control block's id is at its address: the image has started
    template<typename Transport>
    static bool controlBlockPresent(Transport&    jlink,
                                    std::uint32_t address) {
        std::array<std::byte, uc_log::detail::rtt_block::Id.size()> raw{};
        try {
            jlink.readMemory(address, raw);
        } catch(std::exception const&) { return false; }
        return uc_log::detail::rtt_block::View{raw}.hasId();
    }

    // The probe got no answer from the target for TargetSilentLimit: the DLL's connection is closed and opened again (a
    // watchdog reset of the RP2040 by the stall recovery left every DLL call failing -1 for 6.5 minutes, on one
    // connection, while the health checks took each failure for "no sign"). run() escalates when it keeps happening.
    struct TargetSilent : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // While a wait polls the target: whether it still answers - CPUID (0xE000ED00), which a read leaves as it is (a
    // DHCSR read would clear S_RESET_ST, the sign of a reset from outside); silent for TargetSilentLimit -> TargetSilent.
    template<typename Transport>
    void checkTargetAnswers(Transport&         jlink,
                            Clock::time_point& lastAnswer) {
        if(uc_log::detail::tryReadWord(jlink, CpuidAddress)) {
            lastAnswer = Clock::now();
            return;
        }
        if(Clock::now() - lastAnswer > TargetSilentLimit) {
            throw TargetSilent{fmt::format(
              "no answer from the target for {} s",
              std::chrono::duration_cast<std::chrono::seconds>(TargetSilentLimit).count())};
        }
    }

    // Every connection first sees the target answer, within TargetSilentLimit - else TargetSilent (run() escalates).
    // Without it a silent target made the session start's reads fail into the J-Link DLL's own RTT search, which then
    // searched a target nobody could read for minutes.
    template<typename Transport>
    void awaitTargetAnswers(Transport&             jlink,
                            std::stop_token const& stoken) {
        auto lastAnswer = Clock::now();
        while(!stoken.stop_requested() && !uc_log::detail::tryReadWord(jlink, CpuidAddress)) {
            checkTargetAnswers(jlink, lastAnswer);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }

    // After a reset from outside, the next session waits for the new boot's control block: the target may run its boot
    // ROM for a while (RP2350 BOOTSEL zeroes the block), and a session begun then reports its flash as a firmware
    // mismatch and fails startRtt after seconds of RAM scans. A block alone is not the new boot's: RAM keeps the old one
    // while the boot ROM runs, so with an epoch it waits for another epoch than the ended boot's (the new boot moved it
    // before it initialised its block) - and the firmware check after it reads a flash the boot stage has mapped.
    template<typename Transport>
    void awaitControlBlockAfterReset(Transport&             jlink,
                                     std::stop_token const& stoken) {
        if(!handover_.awaitBlock) { return; }
        handover_.awaitBlock  = false;
        auto const endedEpoch = std::exchange(handover_.endedEpoch, std::nullopt);
        auto const epochAt    = rttEpochAddress();
        auto const newBoot    = [&](std::uint32_t at) {
            if(!controlBlockPresent(jlink, at)) { return false; }
            if(!endedEpoch || !epochAt) { return true; }
            auto const epoch = uc_log::detail::tryReadWord(jlink, *epochAt);
            return epoch && *epoch != *endedEpoch;
        };
        auto address = blockInfoCallback().address;
        if(address == 0) { return; }
        bool       told            = false;
        auto const waitStart       = Clock::now();
        auto       nextHealthCheck = waitStart + HealthCheckPeriod;
        auto       lastAnswer      = waitStart;
        health_.beginWait();
        while(!stoken.stop_requested() && !targetResetFlag && !flashFlag && !jlinkResetFlag
              && !newBoot(address))
        {
            checkTargetAnswers(jlink, lastAnswer);
            if(!told) {
                messageCallback(
                  fmt::format("waiting for the RTT control block at {:#010x}: the "
                              "target runs something else (its boot ROM?)",
                              address));
                told = true;
            }
            if(Clock::now() > nextHealthCheck) {
                nextHealthCheck   = Clock::now() + HealthCheckPeriod;
                auto const action = health_.checkWhileWaiting(jlink, waitStart, haltRequested_);
                if(action == Health::Action::printerReset) {
                    targetResetFlag = true;
                    break;
                }
                if(action == Health::Action::recovered || action == Health::Action::resetLine) {
                    // the recovery reset the chip: the DLL's connection may not see the target again until it connects
                    // anew, and a connect right away can land in the new boot's boot ROM window - off the probe a moment
                    // first; the next connection waits for the new boot's block
                    handover_.awaitBlock = true;
                    afterRecovery(action);
                    throw std::runtime_error{
                      "the target was recovered: connecting again to see it"};
                }
                // the build's files may change meanwhile (a rebuild, or back to what the target runs): the block is
                // where the map says now - waiting on at the old map's address waited for good
                if(auto const now = blockInfoCallback().address; now != address) {
                    if(now == 0) { break; }
                    address = now;
                    told    = false;
                }
            }
            serviceTargetOps(jlink);
            std::this_thread::sleep_for(ControlBlockPoll);
        }
    }

    // The DLL is only ever driven from this thread, so the probe list the GUI asked for is
    // produced here too. A failed listing is reported, never a reason to reconnect.
    void serviceProbeListRequest() {
        if(!probeListRequest.exchange(false)) { return; }
        try {
            auto found = TransportT::listProbes();
            toolMessageCallback(
              fmt::format("found {} J-Link probe{}", found.size(), found.size() == 1 ? "" : "s"));
            {
                std::lock_guard<std::mutex> lock{probesMutex};
                probes_ = std::move(found);
            }
            probesVersion_.fetch_add(1, std::memory_order_release);
        } catch(std::exception const& e) {
            toolErrorMessageCallback(fmt::format("listing J-Link probes failed: {}", e.what()));
        }
    }

    // The session's RTT: the printer's own where the map gives the control block's address (and the DLL's is not
    // asked for), the J-Link DLL's otherwise (a block found by the DLL's RAM search).
    template<typename Transport>
    std::unique_ptr<uc_log::detail::RttSource<Transport>>
    openRttSource(Transport&          jlink,
                  RttBlockInfo const& blockInfo) {
        if(useDirectRtt_.load()) {
            if(auto direct = uc_log::detail::DirectRtt<Transport>::open(jlink,
                                                                        blockInfo.address,
                                                                        blockInfo.totalBuffers,
                                                                        rttEpochAddress()))
            {
                toolMessageCallback(
                  fmt::format("RTT read by the printer at {:#010x}", blockInfo.address));
                if(auto const skipped = direct->staleSkipped()) {
                    messageCallback(fmt::format(
                      "the RTT epoch stays odd ({:#x}): an image without the init fence, or a boot "
                      "that died in its init - {} byte{} in the rings skipped unread (they may "
                      "be the ended boot's tail; never read from the middle of a record)",
                      direct->sessionEpoch(),
                      *skipped,
                      *skipped == 1 ? "" : "s"));
                }
                return std::make_unique<uc_log::detail::DirectSource<Transport>>(jlink,
                                                                                 std::move(*direct),
                                                                                 flashRead(jlink));
            }
        }
        toolMessageCallback("RTT read by the J-Link DLL");
        return std::make_unique<uc_log::detail::DllRtt<Transport>>(jlink,
                                                                   blockInfo.address,
                                                                   blockInfo.totalBuffers,
                                                                   errorMessageCallback);
    }

    // Boots nobody read (detail/BootLedger.hpp): said as a status error and handed to setUnreadBootsCallback (the
    // printer writes it into the log as a row too); a delta that cannot be counted from is said with no number.
    void sayUnreadBoots(uc_log::detail::BootLedger::Verdict const& verdict) {
        if(auto const* u = std::get_if<uc_log::detail::BootLedger::Uncountable>(&verdict)) {
            errorMessageCallback(
              fmt::format("the count of boots that logged moved from {} to {}: boots may have "
                          "passed unread, how many is unknown (a power-on, a download over RAM, or "
                          "an image of another layout)",
                          u->from,
                          u->to));
            return;
        }
        auto const* unread = std::get_if<uc_log::detail::BootLedger::Unread>(&verdict);
        if(unread == nullptr) { return; }
        errorMessageCallback(
          fmt::format("{} boot{} passed unread (boots that logged {} -> {}): {} log nobody read{}",
                      unread->boots,
                      unread->boots == 1 ? "" : "s",
                      unread->from,
                      unread->to,
                      unread->boots == 1 ? "its" : "their",
                      unread->replacing ? ", the last one replaced now by the printer" : ""));
        std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> f;
        {
            std::lock_guard<std::mutex> const lock{unreadBootsMutex_};
            f = unreadBootsCallback_;
        }
        if(f) { f(unread->boots, unread->from, unread->to); }
    }

    // After the printer's own download the count of boots that logged starts again (BootLedger::restart): said, and
    // handed to setBootCountRestartCallback (the printer writes it into the log as a row, so a reader of the log
    // restarts its count too).
    void restartBootCount() {
        bootLedger_.restart();
        messageCallback(
          "the count of boots that logged starts again: the printer's download wrote over the "
          "target's RAM");
        sayBootCountRestart();
    }

    void sayBootCountRestart() {
        std::function<void()> f;
        {
            std::lock_guard<std::mutex> const lock{unreadBootsMutex_};
            f = bootCountRestartCallback_;
        }
        if(f) { f(); }
    }

    // the image's count of boots that logged (setLoggedBootsSource; detail/RttEpoch.hpp: bits 31:16 of its word), read
    // now; nullopt: none, or unreadable
    template<typename Transport>
    std::optional<std::pair<std::uint32_t,
                            uc_log::detail::BootLedger::Count>>
    loggedBoots(Transport& jlink) {
        std::function<std::optional<std::uint32_t>()> f;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            f = loggedBootsCallback;
        }
        auto const address = f ? f() : std::nullopt;
        if(!address) { return std::nullopt; }
        auto const value = uc_log::detail::tryReadWord(jlink, *address);
        auto const check = uc_log::detail::tryReadWord(jlink, *address + 4);
        if(!value || !check) { return std::nullopt; }
        bool const restarted = *check == (~*value ^ uc_log::detail::RestartMark);
        if(*check != ~*value && !restarted) {
            return std::nullopt;
        }   // being written, or junk the next boot restarts
        if(restarted && lostCountSaid_ != *value) {
            // the boot found the count not kept (the target's RAM was lost: on the RP2350 a watchdog reboot, a download)
            // and started it again: no number across it, said once per value; this count is the new baseline
            lostCountSaid_ = *value;
            bootLedger_.restart();
            messageCallback(
              "the count of boots that logged was not kept (the target's RAM was lost: a watchdog "
              "reboot, "
              "a download): it starts again");
            sayBootCountRestart();
        }
        return std::pair{*address, static_cast<uc_log::detail::BootLedger::Count>(*value >> 16U)};
    }

    // before the printer's own flash or reset: the boot it replaces is settled (counted if it logged and nobody read it)
    template<typename Transport>
    void noteBootsBeforeReplacing(Transport& jlink) {
        if(auto const logged = loggedBoots(jlink)) {
            sayUnreadBoots(bootLedger_.replacing(logged->first, logged->second));
        }
    }

    std::optional<std::uint32_t> rttEpochAddress() {
        std::function<std::optional<std::uint32_t>()> f;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            f = rttEpochCallback;
        }
        return f ? f() : std::nullopt;
    }

    remote_fmt::FrameCheck frameCheckOfImage() {
        std::function<remote_fmt::FrameCheck()> f;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            f = frameCheckCallback;
        }
        return f ? f() : remote_fmt::FrameCheck::either;
    }

    template<typename Transport>
    bool decodeAllowed(Transport& jlink) {
        std::function<bool(DirectMemoryRead const&)> gate;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            gate = decodeGateCallback;
        }
        if(!gate) { return true; }
        return gate(flashRead(jlink));
    }

    // The firmware check's and the decode gate's reads of the image in flash: only while the flash is mapped. While a
    // boot ROM drives the flash interface by hand, a read through the XIP window collides with its transfer: the
    // RP2040's ROM, reading boot2 in plain SPI mode, then waits for good, and the XIP cache fills with junk. The chip package names the
    // register words of a mapped flash (FlashReady, --flash_ready_signature); the first read waits up to FlashReadyWait
    // for them, every later one reads them first. Not mapped: the read fails (the check says "not checked"), and a boot
    // that never maps it is the stall check's.
    // A core-0-only reset (SYSRESETREQ on the RP2040) leaves the SSI as boot2 set it, so the words still say "mapped"
    // while the boot ROM takes the flash over: DHCSR.S_RESET_ST is read before and after every read - a reset seen,
    // the read is dropped and no further one is made through this reader (the bit goes on to resetSinceLastLook).
    DirectMemoryRead flashRead(TransportT& jlink UC_LOG_LIFETIMEBOUND) UC_LOG_LIFETIMEBOUND {
        std::vector<StallSignature::Condition> conditions;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            conditions = flashReady_;
        }
        return [this, &jlink, ready = std::move(conditions), first = true, stopped = false](
                 std::uint32_t        address,
                 std::span<std::byte> out) mutable {
            if(stopped) { return false; }
            auto const wait
              = std::exchange(first, false) ? FlashReadyWait : std::chrono::milliseconds{0};
            if(!flashMapped(jlink, ready, wait) || resetDuringFlashRead(jlink)) {
                stopped = stopped || resetSeenAside_;
                return false;
            }
            try {
                jlink.readMemory(address, out);
            } catch(std::exception const&) { return false; }
            if(resetDuringFlashRead(jlink)) {
                stopped = true;
                return false;
            }
            return true;
        };
    }

    bool inFlashWindow(std::uint32_t address,
                       std::uint32_t size) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        if(!flashWindow_) { return false; }
        auto const [base, length] = *flashWindow_;
        return std::uint64_t{address} + size > base && address < std::uint64_t{base} + length;
    }

    // every condition holds on one read, within `wait` - and before any waiting, the health checks once: a chip the DLL
    // parked in its boot ROM (the RP2040's) is reset first, as before the gate existed, not polled for seconds
    bool flashMapped(TransportT&                                   jlink,
                     std::vector<StallSignature::Condition> const& ready,
                     std::chrono::milliseconds                     wait) {
        auto const deadline = Clock::now() + wait;
        bool       checked  = wait.count() == 0;
        while(true) {
            bool const mapped
              = std::ranges::all_of(ready, [&jlink](StallSignature::Condition const& c) {
                    auto const word = uc_log::detail::tryReadWord(jlink, c.address);
                    return word && (*word & c.mask) == c.value;
                });
            if(mapped) { return true; }
            if(!std::exchange(checked, true)) {
                auto const action = health_.checkWhileWaiting(jlink, Clock::now(), haltRequested_);
                if(action == Health::Action::printerReset) {
                    targetResetFlag = true;
                    return false;
                }
                if(action != Health::Action::none) {
                    afterRecovery(action);
                    return false;
                }
            }
            if(Clock::now() >= deadline) { break; }
            std::this_thread::sleep_for(FlashReadyPoll);
        }
        if(wait.count() != 0) {
            toolMessageCallback(
              fmt::format("the flash is not mapped after {} ms (the boot ROM drives it?): the "
                          "firmware check does not read it",
                          wait.count()));
        }
        return false;
    }

    // One op on the reader thread; its answer's bytes.
    template<typename Transport>
    std::vector<std::byte> runOp(Transport&      jlink,
                                 TargetOp const& op) {
        namespace to          = uc_log::detail::target_op;
        using Reg             = typename Transport::CoreRegister;
        auto const haltedOnly = [&jlink] {
            if(!jlink.isHalted()) { throw std::runtime_error{"the core runs: halt it first"}; }
        };
        if(auto const* r = std::get_if<to::Read>(&op)) {
            std::vector<std::byte> out(r->size);
            if(inFlashWindow(
                 r->address,
                 r->size)) {   // through the flash's gate, as the firmware check's reads
                if(!flashRead(jlink)(r->address, out)) {
                    throw std::runtime_error{
                      "not read: the flash is not mapped now (a boot ROM drives it) or the core "
                      "was "
                      "just reset"};
                }
                return out;
            }
            jlink.readMemory(r->address, out);
            return out;
        }
        if(auto const* w = std::get_if<to::WriteWord>(&op)) {
            if(w->address == DhcsrAddress) {
                throw std::runtime_error{
                  "DHCSR is the probe's: halt and resume through the halt / go requests "
                  "(a halt written behind the J-Link DLL's back hangs it)"};
            }
            jlink.writeWord(w->address, w->value);
            std::vector<std::byte> out(4);
            jlink.readMemory(w->address, out);
            return out;
        }
        if(std::holds_alternative<to::Halt>(op) || std::holds_alternative<to::Go>(op)) {
            bool const wasHalted = jlink.isHalted();
            if(std::holds_alternative<to::Halt>(op) && !wasHalted) {
                jlink.halt();
                haltRequested_ = true;
            }
            if(std::holds_alternative<to::Go>(op) && wasHalted) {
                jlink.go();
                haltRequested_ = false;
                resumed_       = true;
            }
            halted_ = jlink.isHalted();
            return std::vector<std::byte>(1U,
                                          std::byte{halted_ ? std::uint8_t{1} : std::uint8_t{0}});
        }
        if(std::holds_alternative<to::ReadRegisters>(op)) {
            haltedOnly();
            std::vector<std::byte> out(19U * 4U);
            for(int reg = 0; reg != 19; ++reg) {
                auto const v = jlink.readRegister(static_cast<Reg>(reg));
                for(int b = 0; b != 4; ++b) {
                    out[static_cast<std::size_t>(reg * 4 + b)]
                      = static_cast<std::byte>(v >> (8 * b));
                }
            }
            return out;
        }
        auto const& w = std::get<to::WriteRegister>(op);
        haltedOnly();
        jlink.writeRegister(static_cast<Reg>(w.index), w.value);
        return {};
    }

    // Serves only what is queued now: clients asking in a loop would otherwise starve the RTT reads.
    template<typename Transport>
    void serviceTargetOps(Transport& jlink) {
        std::deque<std::shared_ptr<PendingOps>> batch;
        {
            std::lock_guard<std::mutex> const lock{pendingOpsMutex_};
            batch.swap(pendingOps_);
        }
        for(auto const& pending : batch) {
            // the requester may give up at the same moment: whoever moves the state first wins
            if(!pending->claim(PendingOps::taken)) { continue; }
            std::size_t done = 0;
            try {
                std::vector<std::vector<std::byte>> answers;
                answers.reserve(pending->ops.size());
                for(; done != pending->ops.size(); ++done) {
                    answers.push_back(runOp(jlink, pending->ops[done]));
                }
                pending->result.set_value(std::move(answers));
            } catch(std::exception const& e) {
                pending->result.set_value(std::unexpected{
                  fmt::format("{} ({})", e.what(), uc_log::detail::describe(pending->ops[done]))});
            }
        }
    }

    // Only reads; whoever halted the core resumes it. Unreadable words show as ????????.
    template<typename Transport>
    void captureHalt(Transport& jlink) {
        using Reg = typename Transport::CoreRegister;
        auto const readWord
          = [&](std::uint32_t address) { return uc_log::detail::tryReadWord(jlink, address); };
        auto const word = [&](std::uint32_t address) {
            auto const v = readWord(address);
            return v ? fmt::format("{:#010x}", *v) : std::string{"????????"};
        };
        auto const pc   = jlink.readRegister(Reg::pc);
        auto const lr   = jlink.readRegister(Reg::lr);
        auto const sp   = jlink.readRegister(Reg::sp);
        auto const xpsr = jlink.readRegister(Reg::xpsr);
        auto const msp  = jlink.readRegister(Reg::msp);
        auto const psp  = jlink.readRegister(Reg::psp);
        auto       text = fmt::format(
          "core halted: pc={:#010x} lr={:#010x} sp={:#010x} "
          "xpsr={:#010x} msp={:#010x} psp={:#010x} exception={}",
          pc,
          lr,
          sp,
          xpsr,
          msp,
          psp,
          xpsr & 0x1FFU);
        // CPUID.ARCHITECTURE 0xF = mainline, which has CFSR/HFSR/MMFAR/BFAR; on Armv6-M (0xC)
        // those addresses are reserved (RP2040 data sheet table 104, RP2350 table 200).
        if(auto const cpuid = readWord(0xE000'ED00U); cpuid && ((*cpuid >> 16U) & 0xFU) == 0xFU) {
            text += fmt::format(" cfsr={} hfsr={} mmfar={} bfar={}",
                                word(0xE000'ED28U),
                                word(0xE000'ED2CU),
                                word(0xE000'ED34U),
                                word(0xE000'ED38U));
        }
        text += " stack:";
        for(std::uint32_t i = 0; i != HaltStackWords; ++i) {
            auto const v = readWord(sp + 4U * i);
            text += v ? fmt::format(" {:08x}", *v) : std::string{" ????????"};
        }
        errorMessageCallback(text);
    }

    void failTargetOps(std::string const& why) {
        std::lock_guard<std::mutex> const lock{pendingOpsMutex_};
        for(auto& pending : pendingOps_) {
            if(pending->claim(PendingOps::taken)) {
                pending->result.set_value(std::unexpected{why});
            }
        }
        pendingOps_.clear();
    }

    // One connection a turn: connect, serve a pending flash / reset (a new connection follows it), wait for the
    // control block where a reset left the target, run one session. A failure reconnects after a second; an announced
    // reset keeps the probe off the chip for the pause the target asked for.
    void run(std::stop_token stoken) {
        while(!stoken.stop_requested()) {
            toolMessageCallback("start jlink");
            std::optional<std::chrono::milliseconds>
              announcedPause;   // the session ended for an announced reset
            try {
                takePendingConnection();
                TransportT jlink{
                  device,
                  speed,
                  typename TransportT::Connection{host, 19020, probe},
                  toolMessageCallback,
                  toolErrorMessageCallback
                };
                bool const resetLine
                  = std::exchange(resetPinOnce_, false);   // this connection's reset: the line
                jlink.setResetType(resetLine ? ResetTypeLine
                                             : pendingResetType.load(std::memory_order_relaxed));
                applyPreResetCommands(jlink);
                if(!resetLine) {
                    awaitTargetAnswers(jlink, stoken);
                }   // the reset line is tried on a silent one
                if(serviceFlashAndReset(jlink)) { continue; }
                awaitControlBlockAfterReset(jlink, stoken);
                // our own reset (or flash) set it: only a reset after this point is from outside
                (void)resetSinceLastLook(jlink);
                announcedPause = runSession(jlink, stoken);
            } catch(TargetSilent const& e) {
                escalateSilentTarget(e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            } catch(std::exception const& e) {
                toolErrorMessageCallback(fmt::format("caught {}", e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            } catch(...) {
                // a non-std exception (e.g. from a callback) must reconnect, not terminate
                toolErrorMessageCallback("caught unknown exception");
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            }
            Status notRunning{};
            notRunning.isRunning = 0;
            status               = notRunning;
            halted_ = false;   // haltRequested_ stays: a core a client halted is still halted
            failTargetOps("the target is not connected");
            toolMessageCallback("stopped jlink");
            if(announcedPause) { pauseForAnnouncedReset(stoken, *announcedPause); }
            if(std::exchange(offProbeOnce_, false)) {
                toolMessageCallback(
                  fmt::format("off the probe {} ms after the recovery: the new boot runs its boot "
                              "ROM untouched",
                              RecoveryOffProbe.count()));
                std::this_thread::sleep_for(RecoveryOffProbe);
            }
        }
    }

    // After a recovery that reboots the target: the next connection waits RecoveryOffProbe first (run()); the reset line
    // is the next connection's reset.
    void afterRecovery(typename Health::Action action) {
        if(action == Health::Action::resetLine) {
            resetPinOnce_   = true;
            targetResetFlag = true;
            return;
        }
        if(action == Health::Action::recovered) { offProbeOnce_ = true; }
    }

    // A target that stays silent across connections: connect anew (each time), then the reset line once, then ask for a
    // power cycle - and keep trying. A session that reads the log again starts over (silentConnections_ = 0).
    void escalateSilentTarget(std::string_view what) {
        handover_.awaitBlock
          = true;   // the next connection waits (and watches the silence) before a session
        auto const n = ++silentConnections_;
        if(n < SilentResetAfter) {
            errorMessageCallback(
              fmt::format("{}: the probe connects again ({} of {} before a reset)",
                          what,
                          n,
                          SilentResetAfter - 1));
        } else if(n == SilentResetAfter) {
            errorMessageCallback(
              fmt::format("{} after {} new connections: resetting it through its reset line",
                          what,
                          n - 1));
            resetPinOnce_   = true;
            targetResetFlag = true;
        } else {
            errorMessageCallback(fmt::format(
              "{} after a reset through its reset line: power-cycle it (unplug its USB) - "
              "the printer keeps trying",
              what));
        }
    }

    // a host / probe set meanwhile (setConnection) applies from this connection on; the before-connect hook runs
    void takePendingConnection() {
        std::function<void()> beforeConnect;
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            if(pendingConnection) {
                host  = std::move(pendingConnection->host);
                probe = std::move(pendingConnection->probe);
                pendingConnection.reset();
            }
            beforeConnect = beforeConnectCallback;
        }
        if(beforeConnect) { beforeConnect(); }
        jlinkResetFlag = false;
        serviceProbeListRequest();
    }

    // A pending flash or reset, served; true: the target was replaced or reset, a new connection follows. A request
    // is taken while it runs: it can only be withdrawn before or between attempts.
    bool serviceFlashAndReset(TransportT& jlink) {
        if(flashFlag.load() || targetResetFlag.load()) { noteBootsBeforeReplacing(jlink); }
        bool restart = false;
        if(flashFlag.exchange(false)) { restart = flashTarget(jlink); }
        if(targetResetFlag.exchange(false)) { restart = resetTargetNow(jlink) || restart; }
        return restart;
    }

    bool flashTarget(TransportT& jlink) {
        flashing_           = true;
        auto const ramImage = ramImageStart();
        try {
            if(ramImage) {
                // no reset after the download: it would boot flash, not the image
                if(!*ramImage) { throw std::runtime_error{ramImage->error()}; }
                toolMessageCallback("flashing RAM image");
                jlink.flashRamImage(hexFileNameCallback(), **ramImage);
                toolMessageCallback("flashing RAM image succeeded, started it");
            } else {
                toolMessageCallback("resetting target");
                jlink.resetTarget();
                toolMessageCallback("resetting target succeeded");
                toolMessageCallback("flashing target");
                jlink.flash(hexFileNameCallback());
                toolMessageCallback("flashing target succeeded");
            }
        } catch(std::exception const& e) {
            flashing_ = false;
            // a target whose flash interface is stalled (an RP2350 QMI left busy by its boot ROM) fails every download
            // ("RAMCode did not respond") until the package's recovery has run: run it and download again, not
            // counted as an attempt
            if(!ramImage && health_.recoverStalledFlash(jlink, e.what())) {
                flashFlag = true;
                throw;
            }
            // a J-Link with a second client fails about every other download; a flash interface stuck where no
            // register shows it fails every one until the chip has booted once more (prepareRetry)
            if(++flashAttempt >= FlashAttempts) {
                flashAttempt = 0;
                setOutcome(flashOutcome_, std::string{e.what()}, sessionCount());
            } else {
                if(!ramImage) { prepareRetry(jlink, e.what()); }
                flashFlag = true;
            }
            throw;
        }
        flashAttempt = 0;
        health_.flashDone();
        restartBootCount();
        flashing_      = false;
        haltRequested_ = false;   // the image starts anew
        if(!ramImage) { targetResetFlag = true; }
        setOutcome(flashOutcome_, std::string{}, sessionCount());
        return true;
    }

    // Before the next attempt at a failed download: the package's stall recovery, then a reset with the core let run
    // for RetryBootTime: a flash interface a boot ROM left half set up can fail every download ("RAMCode-sided
    // Prepare() Error") until the boot ROM has booted once. Costs RetryBootTime on a download that failed anyway.
    void prepareRetry(TransportT&      jlink,
                      std::string_view why) {
        health_.recoverBeforeRetry(jlink, why);
        try {
            jlink.resetTarget();
            jlink.go();
            std::this_thread::sleep_for(RetryBootTime);
        } catch(std::exception const& e) {
            toolErrorMessageCallback(
              fmt::format("the reset before the next download attempt failed: {}", e.what()));
        }
    }

    // The printer's own reset (a RAM image is loaded again: a reset boots flash); true: reset.
    bool resetTargetNow(TransportT& jlink) {
        // every resetAndWait() numbered up to here is served by this reset
        auto const  covered  = resetRequests_.load();
        auto const  ramImage = ramImageStart();
        std::string error;
        bool        reset = false;
        if(ramImage && !*ramImage) {
            // nothing to start: told, not retried (the files will not change)
            error = ramImage->error();
            toolErrorMessageCallback(fmt::format("not reset: {}", error));
        } else {
            // a reset asked for through the reset pin (resetAndWait's `line`): J-Link reset type 2 for this one, then
            // the configured type again
            bool const line = resetLineRequested_.exchange(false);
            toolMessageCallback(
              ramImage ? fmt::format("resetting the RAM image{}: loading {} again and starting it",
                                     line ? " through its reset line" : "",
                                     hexFileNameCallback())
                       : std::string{line ? "resetting target through its reset line"
                                          : "resetting target"});
            if(line) { jlink.setResetType(ResetTypeLine); }
            try {
                if(ramImage) {
                    jlink.flashRamImage(hexFileNameCallback(), **ramImage);
                } else {
                    jlink.resetTarget();
                }
            } catch(...) {
                if(line) {
                    jlink.setResetType(pendingResetType.load(std::memory_order_relaxed));
                    resetLineRequested_ = true;   // retried as asked
                }
                targetResetFlag = true;
                throw;
            }
            if(line) { jlink.setResetType(pendingResetType.load(std::memory_order_relaxed)); }
            toolMessageCallback("resetting target succeeded");
            haltRequested_ = false;   // the core runs again
            reset          = true;
        }
        std::lock_guard<std::mutex> const lock{resetOutcome_.mutex};
        resetOutcome_.serial         = covered;
        resetOutcome_.sessionsBefore = sessionCount();
        resetOutcome_.error          = std::move(error);
        return reset;
    }

    // Why a session ended, and what follows it.
    enum class SessionEnd {
        stopOrReplace,   // the printer stops, or resets / flashes the target: the rings are drained
        announced,       // the target announced a reset: drained, acked, the probe stays away
        newBoot,   // a boot the printer did not ask for (outside reset, re-init, a late announced reset)
        recovered,   // a health recovery: the next session waits for the new boot's block
    };

    struct LogChannel {
        std::uint32_t              upIndex;
        std::uint32_t              channel;
        uc_log::detail::RttChannel rtt;
    };

    // What lives for one session: the RTT source, the log channels and their decoder, the read chunks, the
    // announced-reset block, the loop's clocks. Built when the session opens, gone when it ends.
    class Session {
    public:
        Session(BasicJLinkRttReader& reader     UC_LOG_LIFETIMEBOUND,
                TransportT& transport           UC_LOG_LIFETIMEBOUND,
                std::unordered_map<std::uint16_t,
                                   std::string> strings,
                std::optional<std::uint32_t>    announceBlock,
                std::optional<std::uint32_t>    armedAt)
          : r{reader}
          , jlink{transport}
          , strings_{std::move(strings)}
          , blockInfo{reader.blockInfoCallback()}
          , rtt{reader.openRttSource(transport,
                                     blockInfo)}
          , started{rtt->started()}
          , channelMap{uc_log::detail::buildRttChannelMap(*rtt,
                                                          started,
                                                          reader.messageCallback,
                                                          reader.errorMessageCallback)}
          , announceAddress{announceBlock}
          , announceAt{armedAt}
          , epochFenced{rtt->sessionEpoch().has_value()} {
            if(r.duplexBridge.configure) { r.duplexBridge.configure(channelMap.duplexChannels); }
            auto const frameCheck = r.frameCheckOfImage();
            for(auto const& log : channelMap.logChannels) {
                channels.push_back({log.upIndex, log.channel, uc_log::detail::RttChannel{}});
                channels.back().rtt.frameCheck = frameCheck;
            }
            takeHandover();
            allocateChunks();
            decoder.emplace([this](uc_log::detail::DecodeWorker::Item& item) {
                auto& [upIndex, channelId, channel] = channels[item.slot];
                if(!item.bytes.empty()) { channel.append(item.bytes); }
                return channel.drain(r.entryPrintCallback,
                                     channelId,
                                     strings_,
                                     r.errorMessageCallback,
                                     item.haltedRecently);
            });
        }

        Session(Session const&)            = delete;
        Session& operator=(Session const&) = delete;

        // The session died by an exception (a probe failure): what was read is decoded, and the unfinished bytes of
        // each channel - the head of a record whose tail is still in the target - go to the next session with
        // this session's RTT epoch.
        void keepUnfinished() noexcept {
            try {
                decoder->barrier();
            } catch(...) {}
            for(auto const& ch : channels) {
                if(!ch.rtt.buffer.empty()) { r.handover_.carry[ch.upIndex] = ch.rtt.buffer; }
            }
            if(!r.handover_.carry.empty()) { r.handover_.carryEpoch = rtt->sessionEpoch(); }
        }

        // The session's first bytes: its boot logged and is read (detail/BootLedger.hpp) - the boots before it that
        // logged and nobody read are said now, ahead of this boot's lines.
        void noteBootRead() {
            bootNoted = true;
            if(auto const logged = r.loggedBoots(jlink)) {
                bootCount = logged->second;
                r.sayUnreadBoots(r.bootLedger_.bytesRead(logged->second));
            }
        }

        // At the session's end: a boot noted as read whose bytes gave no line (the session ended on a probe failure
        // before a whole record, the bytes dropped) was not read after all - a boot of the watchdog loop the printer
        // then replaced went uncounted.
        void settleUnprinted() {
            printedAny = printedAny || decoder->takePrinted();
            if(bootNoted && !printedAny && bootCount) { r.bootLedger_.notRead(*bootCount); }
        }

        // Hands what a read took from each log channel's ring to the decoder - an empty item too: the decode times a
        // stuck frame out (RttChannel::drain) and decodes what a channel already holds. A probe failure the read kept
        // is thrown after its bytes are with the decoder: the session ends as on any.
        std::size_t decodeRead(std::span<std::span<std::byte> const> read,
                               bool                                  haltedRecently) {
            std::size_t bytes = 0;
            for(std::size_t c = 0; c != channels.size(); ++c) {
                auto const upIndex = channels[c].upIndex;
                auto const data    = upIndex < read.size() ? read[upIndex] : std::span<std::byte>{};
                bytes += data.size();
                decoder->push(
                  {c, std::vector<std::byte>(data.begin(), data.end()), haltedRecently});
            }
            r.rttBytes_ += bytes;
            if(bytes != 0 && !bootNoted) { noteBootRead(); }
            if(auto const failure = rtt->takeFailure()) { std::rethrow_exception(failure); }
            return bytes;
        }

        // What each log ring holds now, read whole and decoded after everything read before it (one read per ring
        // ending on a record boundary: a target that floods on does not keep a stopping printer reading); the
        // bytes it read and the bytes of unfinished lines left in the channels.
        std::pair<std::size_t,
                  std::size_t>
        drainAll(bool haltedRecently) {
            auto const read = decodeRead(rtt->drain(logOuts), haltedRecently);
            decoder->barrier();
            std::size_t unfinished{};
            for(auto const& ch : channels) { unfinished += ch.rtt.buffer.size(); }
            return {read, unfinished};
        }

        BasicJLinkRttReader&                                   r;
        TransportT&                                            jlink;
        std::unordered_map<std::uint16_t, std::string>         strings_;
        RttBlockInfo                                           blockInfo;
        std::unique_ptr<uc_log::detail::RttSource<TransportT>> rtt;
        Status                                                 started;
        uc_log::detail::RttChannelMap                          channelMap;
        std::optional<std::uint32_t>                           announceAddress;
        std::optional<std::uint32_t>                           announceAt;
        bool                                                   epochFenced;
        std::vector<LogChannel>                                channels;
        // A chunk per up ring the session reads, by its index: a log ring's holds the whole ring (the drain takes a
        // ring whole), a duplex ring's what one pass passes on. `passOuts`: what a pass reads, `logOuts`: what a
        // drain reads.
        std::vector<std::vector<std::byte>>    upChunks;
        std::vector<std::span<std::byte>>      passOuts;
        std::vector<std::span<std::byte>>      logOuts;
        std::array<std::byte, DuplexChunkSize> duplexOut{};
        // the loop's clocks
        Clock::time_point lastMessage{Clock::now()};
        Clock::time_point lastHaltDetected{};
        Clock::time_point nextAnnouncePoll{};
        Clock::time_point nextAnnounceArm{Clock::now() + AnnouncedResetArmRetry};
        Clock::time_point lastHaltPoll{};
        bool              lastPassRead{};   // the pass before read bytes (BusyHaltPoll)
        Clock::time_point nextStallCheck{Clock::now() + StallCheckAfter};
        bool              wasHalted{};
        bool              quietWarned{};
        bool              bootNoted{};   // the session's boot is noted as read (noteBootRead)
        std::optional<uc_log::detail::BootLedger::Count>
                                  bootCount;      // its count of boots that logged then
        bool                      printedAny{};   // the decoder gave a line in this session
        std::chrono::milliseconds announcedPause{AnnouncedResetPause};
        // the bytes of each pass are decoded on a thread of their own while the next pass reads
        // (detail/DecodeWorker.hpp); last: it goes before the channels it decodes into
        std::optional<uc_log::detail::DecodeWorker> decoder;

    private:
        // The session before died on a probe failure holding the head of a record whose tail is still in the target:
        // it goes in front of this session's first bytes - only if the target has not booted since (the same RTT
        // epoch); otherwise its loss is said with its size.
        void takeHandover() {
            auto& carry = r.handover_.carry;
            if(carry.empty()) { return; }
            // the same boot, and its rings read on where the session before stopped (not skipped: DirectRtt::skipUnread)
            bool const  sameBoot = rtt->sessionEpoch() && r.handover_.carryEpoch
                                && *rtt->sessionEpoch() == *r.handover_.carryEpoch
                                && !rtt->staleSkipped();
            std::size_t dropped  = 0;
            for(auto const& [upIndex, bytes] : carry) {
                auto const ch = std::ranges::find(channels, upIndex, &LogChannel::upIndex);
                if(sameBoot && ch != channels.end()) {
                    ch->rtt.append(bytes);
                } else {
                    dropped += bytes.size();
                }
            }
            if(dropped != 0) {
                r.messageCallback(fmt::format(
                  "{} byte{} of an unfinished line dropped: the session before ended on a "
                  "probe failure{}",
                  dropped,
                  dropped == 1 ? "" : "s",
                  rtt->sessionEpoch() ? " and the target booted again since"
                                      : " (no RTT epoch to tell whether the target booted "
                                        "since)"));
            }
            carry.clear();
            r.handover_.carryEpoch.reset();
        }

        void allocateChunks() {
            upChunks.resize(static_cast<std::size_t>(std::max(started.numUpBuffers, 0)));
            passOuts.resize(upChunks.size());
            logOuts.resize(upChunks.size());
            for(auto const& ch : channels) {
                if(ch.upIndex >= upChunks.size()) { continue; }
                upChunks[ch.upIndex].resize(
                  std::max<std::size_t>(RttBufferChunkSize, rtt->upSize(ch.upIndex)));
                passOuts[ch.upIndex] = logOuts[ch.upIndex] = std::span{upChunks[ch.upIndex]};
            }
            for(auto const& dc : channelMap.duplexChannels) {
                if(!dc.upIndex || *dc.upIndex >= upChunks.size() || !r.duplexBridge.sendToClient) {
                    continue;
                }
                upChunks[*dc.upIndex].resize(DuplexChunkSize);
                passOuts[*dc.upIndex] = std::span{upChunks[*dc.upIndex]};
            }
        }
    };

    // One session: the catalog, the session-start callback (the firmware check, before any RTT read: the target keeps
    // its lines in its rings meanwhile), the decode gate, the announced-reset block armed, then the read loop until
    // something ends it. The announced pause when the target announced a reset.
    std::optional<std::chrono::milliseconds> runSession(TransportT&            jlink,
                                                        std::stop_token const& stoken) {
        auto strings = catalogMapCallback();
        runSessionStart(jlink);
        if(!decodeAllowed(jlink)) {
            holdAtGate(jlink, stoken);
            return std::nullopt;
        }
        // Armed only after the session start's flash reads: the firmware's AnnouncedReset::waitForPrinter returns once
        // the printer listens, and a reset it then makes must not land in an XIP read (the RP2040's boot ROM hangs on
        // it). Right after a reset .data is not copied yet: a few ms of polling.
        auto const announceAddress = announcedResetAddress();   // the map file, once
        auto const announceAt      = armAnnouncedResetSoon(jlink, announceAddress, stoken);
        Session    s{*this, jlink, std::move(strings), announceAddress, announceAt};
        if constexpr(requires { jlink.serialNumber(); }) { probeSerial_ = jlink.serialNumber(); }
        if(auto const logged = loggedBoots(jlink)) {
            sayUnreadBoots(bootLedger_.start(logged->first, logged->second));
        }
        sessionCount_.fetch_add(1, std::memory_order_release);
        // not health_.logRead() here: a session starts on the old block after every reconnect, and the stall ladder must
        // go on across them - only lines read set it back (readPass)
        silentConnections_ = 0;   // the target answers again
        std::optional<SessionEnd> end;
        try {
            while(!end) { end = readPass(s, stoken); }
        } catch(...) {
            s.keepUnfinished();
            s.settleUnprinted();
            throw;
        }
        auto const pause = endSession(s, *end, stoken);
        s.settleUnprinted();
        return pause;
    }

    void runSessionStart(TransportT& jlink) {
        std::function<void(DirectMemoryRead const&)> onStart;
        {
            std::lock_guard<std::mutex> const lock{sessionStartMutex};
            onStart = sessionStartCallback;
        }
        if(!onStart) { return; }
        onStart(flashRead(jlink));
    }

    // The decode gate: no RTT read while the strings would be the wrong ones (the target runs another image than
    // the files describe). Nothing is read, so nothing is lost: the target keeps its lines in its rings (and counts
    // what does not fit). Opens when the files change to match (a rebuild); a flash or reset starts a new session
    // anyway. Opening ends this session so the next one loads the new catalog and map.
    void holdAtGate(TransportT&            jlink,
                    std::stop_token const& stoken) {
        messageCallback(
          "not decoding: the target runs another image than the build's files - the log waits in "
          "the "
          "target's rings until it is flashed (or the files match)");
        bool       opened          = false;
        bool       recovered       = false;
        auto const gateStart       = Clock::now();
        auto       nextHealthCheck = gateStart + HealthCheckPeriod;
        auto       lastAnswer      = gateStart;
        health_.beginWait();
        while(!stoken.stop_requested() && !jlinkResetFlag && !targetResetFlag && !flashFlag) {
            checkTargetAnswers(jlink, lastAnswer);
            serviceTargetOps(jlink);
            serviceProbeListRequest();
            if(resetSinceLastLook(jlink)) { break; }   // a new boot: a new session checks it again
            // "another image" may be a parked chip whose flash the boot ROM left unmapped
            if(Clock::now() > nextHealthCheck) {
                nextHealthCheck   = Clock::now() + HealthCheckPeriod;
                auto const action = health_.checkWhileWaiting(jlink, gateStart, haltRequested_);
                if(action != Health::Action::none) {
                    targetResetFlag = targetResetFlag || action == Health::Action::printerReset;
                    afterRecovery(action);
                    recovered = true;
                    break;
                }
            }
            std::this_thread::sleep_for(DecodeGatePoll);
            if(decodeAllowed(jlink)) {
                opened = true;
                break;
            }
        }
        if(opened) { messageCallback("the build's files match the target now: decoding"); }
        handover_.awaitBlock = recovered;   // reset: the new boot's block is waited for
    }

    // One pass of the read loop; a reason when the session ends.
    std::optional<SessionEnd> readPass(Session&               s,
                                       std::stop_token const& stoken) {
        if(stoken.stop_requested() || jlinkResetFlag || targetResetFlag || flashFlag) {
            return SessionEnd::stopOrReplace;
        }
        bool const haltedRecently = Clock::now() < s.lastHaltDetected + HaltGracePeriod;
        bool const haltPollDue
          = Clock::now() >= s.lastHaltPoll + (s.lastPassRead ? BusyHaltPoll : HaltPoll);
        if(haltPollDue) { s.lastHaltPoll = Clock::now(); }
        if(auto const end = pollAnnouncedReset(s, haltedRecently)) { return end; }
        // DHCSR.S_RESET_ST: every pass - or, where the RTT epoch fences what a pass reads, only in the passes that
        // poll the halt state (a reset then shows in the next pass that reads bytes, DirectRtt::readAll; this poll
        // ends a session whose rings stay empty): one probe round trip a pass less. Read before the halt poll of the
        // same pass: the J-Link DLL's own DHCSR reads may clear the bit.
        if((!s.epochFenced || haltPollDue) && resetSinceLastLook(s.jlink)) {
            // Someone else reset the target (picotool, a watchdog, the button). SEGGER's RTT reader carries on across
            // it and, once the new image or the boot ROM rewrites the control block, reads an old lap of a ring: take
            // what is there, then start over as after a reset of our own.
            sayNewBoot(s,
                       "the target was reset from outside the printer",
                       "of a line cut off by the reset",
                       haltedRecently);
            return SessionEnd::newBoot;
        }
        auto const passed = s.rtt->readPass(s.passOuts);
        s.lastPassRead    = s.decodeRead(passed, haltedRecently) != 0;
        bool morePending  = s.lastPassRead;
        for(std::size_t i = 0; i != s.passOuts.size();
            ++i) {   // a ring that filled its chunk has more
            morePending
              = morePending || (!s.passOuts[i].empty() && passed[i].size() == s.passOuts[i].size());
        }
        if(s.decoder->takePrinted()) {
            s.printedAny  = true;
            s.lastMessage = Clock::now();
            s.quietWarned = false;
            health_.logRead();
        }
        if(s.rtt->reinitSeen()) {
            // A boot re-initialised the RTT block (a reset nobody announced: the RTT epoch moved). What was read
            // across it is dropped; the next session starts at the new block's first byte.
            sayNewBoot(s,
                       "the target booted again (its RTT block was re-initialised)",
                       "of a line cut off by the reset",
                       haltedRecently);
            return SessionEnd::newBoot;
        }
        serviceDuplex(s, passed);
        s.jlink.checkConnected();
        // the halt state every HaltPoll (BusyHaltPoll while passes read bytes), not every pass
        if(haltPollDue) { halted_ = s.jlink.isHalted(); }
        noteHalt(s);
        Status const current = s.rtt->status();
        status               = current;
        if(current.isRunning == 0 || current.numUpBuffers != s.started.numUpBuffers
           || current.numDownBuffers != s.started.numDownBuffers)
        {
            throw std::runtime_error("lost connection");
        }
        warnIfQuiet(s);
        if(auto const end = checkQuietTarget(s)) { return end; }
        // no pause after a pass that found bytes: the target is writing, the next pass reads at once
        if(!morePending) { std::this_thread::sleep_for(std::chrono::milliseconds{1}); }
        serviceGuiRequests(s.jlink);
        serviceProbeListRequest();
        serviceTargetOps(s.jlink);
        return std::nullopt;
    }

    static TransportT& jlink(Session& s) { return s.jlink; }

    // A boot the printer did not ask for ends the session: what the rings hold is taken first, the next session waits
    // for the new boot's block.
    void sayNewBoot(Session&         s,
                    std::string_view what,
                    std::string_view cut,
                    bool             haltedRecently) {
        auto const unfinished = s.drainAll(haltedRecently).second;
        messageCallback(
          fmt::format("{}: a new RTT session{}", what, unfinishedText(unfinished, cut)));
        handover_.awaitBlock = true;
        handover_.endedEpoch = s.rtt->sessionEpoch();
    }

    static std::string unfinishedText(std::size_t      bytes,
                                      std::string_view what) {
        if(bytes == 0) { return {}; }
        return fmt::format(" ({} byte{} {} dropped)", bytes, bytes == 1 ? "" : "s", what);
    }

    // The firmware's announced reset (Kvasir_SDK Util/AnnouncedReset.hpp): armed again where the block was not
    // there yet; a pending request is answered - drain the last lines, stop RTT, clear the control block's id (a reboot
    // into a boot ROM keeps RAM), ack as the last access; the probe then stays away.
    std::optional<SessionEnd> pollAnnouncedReset(Session& s,
                                                 bool     haltedRecently) {
        if(s.announceAddress && !s.announceAt && Clock::now() >= s.nextAnnounceArm) {
            s.nextAnnounceArm = Clock::now() + AnnouncedResetArmRetry;
            s.announceAt      = armAnnouncedReset(s.jlink, s.announceAddress, true);
        }
        if(!s.announceAt || Clock::now() < s.nextAnnouncePoll) { return std::nullopt; }
        s.nextAnnouncePoll = Clock::now() + AnnouncedResetPoll;
        auto const block   = readAnnouncedResetBlock(s.jlink, *s.announceAt);
        if(!block || !block->pending()) { return std::nullopt; }
        std::this_thread::sleep_for(AnnouncedResetDrain);
        auto const unfinished = s.drainAll(haltedRecently).second;
        // The firmware waits a bounded time (~0.5 s). A printer slow to see the request - busy with a halt it caught,
        // a slow probe - finds it reset already: writing then would clear the id of the new boot's control block and
        // ack into its block. So the request must still wait.
        if(auto const still = readAnnouncedResetBlock(s.jlink, *s.announceAt);
           !still || !still->pending() || still->request != block->request)
        {
            messageCallback(fmt::format(
              "the target announced a reset and did it before the printer answered: a new RTT "
              "session{}",
              unfinishedText(unfinished, "of an unfinished line")));
            handover_.awaitBlock = true;
            return SessionEnd::newBoot;
        }
        // the firmware may ask for longer (a watchdog it lets run out)
        s.announcedPause = std::clamp(std::chrono::milliseconds{block->stayAwayMs()},
                                      AnnouncedResetPause,
                                      AnnouncedResetPauseMax);
        messageCallback(
          fmt::format("the target announced a reset: the probe stays off it for {} ms{}",
                      s.announcedPause.count(),
                      unfinishedText(unfinished, "of an unfinished line")));
        s.rtt->stop();
        if(s.blockInfo.address != 0) { s.jlink.writeWord(s.blockInfo.address, 0); }
        s.jlink.writeWord(*s.announceAt + uc_log::detail::announced_reset::AckOffset,
                          block->request);
        return SessionEnd::announced;
    }

    void serviceDuplex(Session&                              s,
                       std::span<std::span<std::byte> const> passed) {
        for(auto const& dc : s.channelMap.duplexChannels) {
            if(dc.upIndex && duplexBridge.sendToClient) {
                auto const data
                  = *dc.upIndex < passed.size() ? passed[*dc.upIndex] : std::span<std::byte>{};
                if(!data.empty()) {
                    s.lastMessage = Clock::now();   // live duplex traffic proves the link is alive
                    duplexBridge.sendToClient(dc.ordinal, data);
                }
            }
            if(duplexBridge.peekFromClient && duplexBridge.consumeFromClient) {
                auto const pending = duplexBridge.peekFromClient(dc.ordinal, s.duplexOut);
                if(pending == 0) { continue; }
                auto const written
                  = s.rtt->rttWrite(dc.downIndex,
                                    std::span<std::byte const>{s.duplexOut}.first(pending));
                if(written != 0) { duplexBridge.consumeFromClient(dc.ordinal, written); }
            }
        }
    }

    // A halt a client asked for is no crash: said as a tool message, so a wait for a log line beside it (kvasir_bench
    // wait-for ends on "core halted") does not fail. Any other halt is captured (the fault handler's bkpt).
    // A go from a request ran the core even when it halts again before the next pass (a raised panic's bkpt is
    // microseconds away): that halt is a new one.
    void noteHalt(Session& s) {
        if(std::exchange(resumed_, false)) { s.wasHalted = false; }
        bool const halted = halted_;
        if(halted) {
            s.lastHaltDetected = Clock::now();
            if(!s.wasHalted) {
                if(haltRequested_) {
                    toolMessageCallback("core halted by a control-socket halt request");
                } else {
                    captureHalt(s.jlink);
                }
            }
        }
        s.wasHalted = halted;
    }

    // a quiet target is not a dead link (checkConnected and the status say that): only a warning
    void warnIfQuiet(Session& s) {
        auto const quietSeconds = noLogTimeoutSeconds_.load();
        if(quietSeconds == 0 || s.quietWarned
           || Clock::now() <= s.lastMessage + std::chrono::seconds{quietSeconds})
        {
            return;
        }
        s.quietWarned = true;
        messageCallback(fmt::format("no log messages for {} s, link is alive", quietSeconds));
    }

    // A target quiet for StallCheckAfter: in lockup, or stalled in a way its chip package can tell (and undo)?
    std::optional<SessionEnd> checkQuietTarget(Session& s) {
        if(Clock::now() <= s.lastMessage + StallCheckAfter || Clock::now() <= s.nextStallCheck) {
            return std::nullopt;
        }
        s.nextStallCheck  = Clock::now() + StallCheckAfter;
        auto const action = health_.checkQuietSession(s.jlink, s.lastMessage);
        if(action == Health::Action::printerReset) {
            targetResetFlag = true;   // the printer's ordinary reset: the rings are drained first
            return SessionEnd::stopOrReplace;
        }
        if(action == Health::Action::recovered || action == Health::Action::resetLine) {
            handover_.awaitBlock = true;   // the next session waits for the new boot's block
            afterRecovery(action);
            return SessionEnd::recovered;
        }
        return std::nullopt;
    }

    // what the GUI asked for between passes
    void serviceGuiRequests(TransportT& jlink) {
        if(targetContinueFlag.exchange(false)) { jlink.go(); }
        if(targetHaltFlag.exchange(false)) { jlink.halt(); }
        if(targetClearBreakPointsFlag.exchange(false)) { jlink.clearAllBreakpoints(); }
        if(hasResetTypeChange.exchange(false, std::memory_order_acquire)) {
            jlink.setResetType(pendingResetType.load(std::memory_order_relaxed));
        }
        if(hasPreResetCommandsChange.load(std::memory_order_acquire)) {
            applyPreResetCommands(jlink);
        }
    }

    // The end of a session. Stopping, or replacing the target by the printer's own flash or reset: what is in the
    // rings is read and decoded now - with the DLL's RTT first what the DLL holds, then its RTT stops, then
    // what it read meanwhile (what it took and had not handed over is lost at the stop: the mode's weakness). Every
    // end: a printer that goes disarms the announced-reset block (a later reset of the firmware must not wait for it).
    std::optional<std::chrono::milliseconds> endSession(Session&               s,
                                                        SessionEnd             end,
                                                        std::stop_token const& stoken) {
        if(end == SessionEnd::stopOrReplace) {
            try {
                auto const before = s.drainAll(true).first;
                s.rtt->stop();
                auto const [after, unfinished] = s.drainAll(true);
                toolMessageCallback(
                  fmt::format("the session ends: {} + {} bytes read and decoded at its end",
                              before,
                              after));
                if(unfinished != 0) {
                    messageCallback(
                      fmt::format("the session ends: {}", unfinishedLines(s, unfinished)));
                }
            } catch(std::exception const& e) {
                toolErrorMessageCallback(
                  fmt::format("draining at the end of the session: {}", e.what()));
            }
        }
        if(stoken.stop_requested() && s.announceAt) {
            try {
                s.jlink.writeWord(*s.announceAt + uc_log::detail::announced_reset::ArmedOffset, 0);
            } catch(std::exception const&) {}
        }
        if(end == SessionEnd::announced) { return s.announcedPause; }
        return std::nullopt;
    }

    // "N bytes of an unfinished line dropped" with the first bytes of each channel's (for finding where a record broke)
    static std::string unfinishedLines(Session&    s,
                                       std::size_t unfinished) {
        std::string where;
        for(auto const& [upIndex, channelId, channel] : s.channels) {
            if(channel.buffer.empty()) { continue; }
            where += fmt::format(" [ch {}: {:02x}]",
                                 channelId,
                                 fmt::join(std::span{channel.buffer}.first(
                                             std::min<std::size_t>(channel.buffer.size(), 40)),
                                           " "));
        }
        return fmt::format("{} byte{} of an unfinished line dropped{}",
                           unfinished,
                           unfinished == 1 ? "" : "s",
                           where);
    }

    // The session is closed (the transport is gone): no DLL call until the pause is over, and
    // memory accesses asked meanwhile fail at once instead of waiting for it.
    void pauseForAnnouncedReset(std::stop_token const&    stoken,
                                std::chrono::milliseconds pause) {
        pausedForReset_ = true;
        auto const end  = Clock::now() + pause;
        while(!stoken.stop_requested() && Clock::now() < end) {
            failTargetOps(std::string{PausedMessage});
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        pausedForReset_ = false;
        failTargetOps(std::string{PausedMessage});
        // the next session waits for the new boot's control block (its id was cleared)
        handover_.awaitBlock = true;
        // a printer stopped during the pause does not connect again (the reader loop ends):
        // saying so would make a park in the reboot look like this printer's doing
        toolMessageCallback(stoken.stop_requested()
                              ? "the printer stops during an announced reset: no new session"
                              : "the announced reset is over: connecting again");
    }

    static constexpr std::string_view PausedMessage{
      "target paused for an announced reset: the probe stays off it"};

    // armAnnouncedReset() polled quietly for up to AnnouncedResetArmWait; reported once if the
    // block is still not there (the session's loop keeps looking every AnnouncedResetArmRetry).
    template<typename Transport>
    std::optional<std::uint32_t> armAnnouncedResetSoon(Transport&                          jlink,
                                                       std::optional<std::uint32_t> const& address,
                                                       std::stop_token const&              stoken) {
        if(!address) { return std::nullopt; }
        auto const end = Clock::now() + AnnouncedResetArmWait;
        while(true) {
            if(auto const armed = armAnnouncedReset(jlink, address, true); armed) { return armed; }
            if(stoken.stop_requested() || Clock::now() >= end) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return armAnnouncedReset(jlink, address, false);   // says why
    }

    std::optional<std::uint32_t> announcedResetAddress() {
        std::function<std::optional<std::uint32_t>()> f;
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            f = announcedResetAddressCallback;
        }
        if(!f) { return std::nullopt; }
        return f();
    }

    // The block's address once armed for this session (the firmware then waits for our ack
    // before resetting itself); nullopt: not armed now. `retry`: no message unless it succeeds.
    template<typename Transport>
    std::optional<std::uint32_t> armAnnouncedReset(Transport&                          jlink,
                                                   std::optional<std::uint32_t> const& address,
                                                   bool                                retry) {
        if(!address) { return std::nullopt; }
        auto const block = readAnnouncedResetBlock(jlink, *address);
        if(!block || !block->valid()) {
            if(!retry) {
                toolMessageCallback(fmt::format(
                  "announced resets: no block at {:#010x} yet ({} not set up or not readable) - "
                  "looking again every {} ms",
                  *address,
                  uc_log::detail::announced_reset::Symbol,
                  AnnouncedResetArmRetry.count()));
            }
            return std::nullopt;
        }
        try {
            jlink.writeWord(*address + uc_log::detail::announced_reset::ArmedOffset,
                            uc_log::detail::announced_reset::HostArmed);
        } catch(std::exception const& e) {
            if(!retry) {
                toolErrorMessageCallback(
                  fmt::format("announced resets: arming the block at {:#010x} failed: {}",
                              *address,
                              e.what()));
            }
            return std::nullopt;
        }
        toolMessageCallback(
          fmt::format("announced resets: listening at {:#010x} (request {}, ack {})",
                      *address,
                      block->request,
                      block->ack));
        return address;
    }

    std::string   host;
    std::string   device;
    std::uint32_t speed;
    std::string   probe;   // by serial number or nickname; empty = the only one on USB

    std::function<RttBlockInfo(void)>                                   blockInfoCallback;
    std::function<std::string(void)>                                    hexFileNameCallback;
    std::function<std::unordered_map<std::uint16_t, std::string>(void)> catalogMapCallback;
    std::function<void(std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>)>
                                          entryPrintCallback;
    std::function<void(std::string_view)> messageCallback;
    std::function<void(std::string_view)> errorMessageCallback;
    std::function<void(std::string_view)> toolMessageCallback;
    std::function<void(std::string_view)> toolErrorMessageCallback;
    uc_log::detail::DuplexBridge          duplexBridge;

    std::atomic<Status>               status;
    std::atomic<std::uint32_t>        noLogTimeoutSeconds_{15};
    std::atomic<bool>                 targetResetFlag;
    std::atomic<bool>                 targetContinueFlag;
    std::atomic<bool>                 targetHaltFlag;
    std::atomic<bool>                 targetClearBreakPointsFlag;
    std::atomic<bool>                 jlinkResetFlag;
    std::atomic<bool>                 flashFlag;
    std::atomic<std::uint8_t>         pendingResetType;
    std::atomic<bool>                 hasResetTypeChange;
    std::mutex                        connectionMutex;
    std::optional<ConnectionSettings> pendingConnection;
    std::atomic<bool>                 hasPreResetCommandsChange;
    std::vector<JLink::MemoryWrite>   preResetCommands;   // guarded by connectionMutex

public:
    using RamImageStartResult = std::expected<JLink::RamImageStart, std::string>;

private:
    std::function<void()> beforeConnectCallback;   // guarded by connectionMutex
    // set: the image lives in RAM, flash and reset load it and start it (guarded by connectionMutex)
    std::function<RamImageStartResult()> ramImageStartCallback;

    // nullopt: an ordinary image; else where the RAM image starts, or why that is not known
    std::optional<RamImageStartResult> ramImageStart() {
        std::function<RamImageStartResult()> f;
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            f = ramImageStartCallback;
        }
        if(!f) { return std::nullopt; }
        return f();
    }

    template<typename T>
    void applyPreResetCommands(T& jlink) {
        std::vector<JLink::MemoryWrite> commands;
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            hasPreResetCommandsChange.store(false, std::memory_order_relaxed);
            commands = preResetCommands;
        }
        jlink.setPreResetCommands(std::move(commands));
    }

    // Queued by other threads; the J-Link is only ever driven from the reader thread.
    // queued -> taken (the reader thread runs it, the requester waits for its result) or queued -> abandoned (the
    // requester timed out first, the reader skips it). Never both: a write the reader has started is not reported
    // as timed out (that was possible once: a poke "failed" and took effect).
    struct PendingOps {
        enum State : int { queued, taken, abandoned };

        std::vector<TargetOp>      ops;
        std::promise<TargetResult> result;
        std::atomic<int>           state{queued};

        bool claim(State to) {
            int expected = queued;
            return state.compare_exchange_strong(expected, to, std::memory_order_acq_rel);
        }
    };

    // empty error = success
    struct Outcome {
        std::mutex    mutex;
        std::uint64_t serial{};
        std::uint64_t sessionsBefore{};
        std::string   error;
    };

    static void setOutcome(Outcome&      o,
                           std::string   error,
                           std::uint64_t sessions) {
        std::lock_guard<std::mutex> const lock{o.mutex};
        ++o.serial;
        o.sessionsBefore = sessions;
        o.error          = std::move(error);
    }

    Outcome                    flashOutcome_;
    Outcome                    resetOutcome_;   // serial: requests covered
    std::atomic<std::uint64_t> resetRequests_{0};
    std::atomic<unsigned>      flashAttempt{};
    std::atomic<bool>          flashing_{false};
    std::atomic<std::uint64_t> sessionCount_{0};
    std::atomic<std::uint32_t> probeSerial_{0};
    uc_log::detail::BootLedger bootLedger_;   // reader thread
    std::mutex                 unreadBootsMutex_;
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)>
                          unreadBootsCallback_;        // unreadBootsMutex_
    std::function<void()> bootCountRestartCallback_;   // unreadBootsMutex_
    std::atomic<bool>     halted_{false};
    bool              haltRequested_{false};   // reader thread: the halt came from a halt request
    bool              resumed_{false};   // reader thread: a go request ran the core since noteHalt
    std::atomic<bool> pausedForReset_{false};
    // guarded by connectionMutex; empty or nullopt: the image has no announced-reset block
    std::function<std::optional<std::uint32_t>()> announcedResetAddressCallback;
    std::mutex                                    sessionStartMutex;
    std::function<void(DirectMemoryRead const&)>  sessionStartCallback;
    std::function<bool(DirectMemoryRead const&)>  decodeGateCallback;

    // What one session leaves to the next (reader thread): whether to wait for the new boot's control block, and
    // the unfinished bytes of each log channel (by up ring) of a session that died by an exception, with its RTT
    // epoch - the next session continues them if the target has not booted since.
    struct Handover {
        bool awaitBlock{};
        // the RTT epoch of the boot the last session read: after a reset the next one waits for a block of another
        // epoch - the old boot's block stays in RAM while the boot ROM runs
        std::optional<std::uint32_t>                    endedEpoch;
        std::map<std::uint32_t, std::vector<std::byte>> carry;
        std::optional<std::uint32_t>                    carryEpoch;
    } handover_;

    std::function<remote_fmt::FrameCheck()>       frameCheckCallback;
    std::function<std::optional<std::uint32_t>()> rttEpochCallback;
    std::function<std::optional<std::uint32_t>()> loggedBootsCallback;   // sessionStartMutex
    std::atomic<bool>                             useDirectRtt_{true};
    // park, lockup and stall checks (its signatures are set from any thread); declared before `thread`
    Health                     health_{[this](std::string_view m) { errorMessageCallback(m); },
                                       [this](std::string_view m) { toolMessageCallback(m); }};
    static constexpr auto      HealthCheckPeriod = std::chrono::milliseconds{1000};
    static constexpr int       MaxParkRecoveries = 3;
    std::atomic<std::uint64_t> rttBytes_{0};
    static constexpr auto      DecodeGatePoll    = std::chrono::milliseconds{500};
    static constexpr auto      FlashReadyWait    = std::chrono::milliseconds{3000};
    static constexpr auto      TargetSilentLimit = std::chrono::milliseconds{3000};
    static constexpr int       SilentResetAfter  = 3;   // silent connections before the reset line
    int                        silentConnections_{};    // reader thread
    bool                       resetPinOnce_{};   // reader thread: the next reset by the reset line
    std::atomic<bool>
         resetLineRequested_{};   // resetAndWait(line): the next resetTargetNow by the line
    bool offProbeOnce_{};         // reader thread: pause before the next connect
    static constexpr auto        RecoveryOffProbe = std::chrono::milliseconds{1500};
    bool                         resetSeenAside_{};   // reader thread: S_RESET_ST a flash read saw
    std::optional<std::uint32_t> lostCountSaid_;      // reader thread: the junk count word said
    // few probe accesses while a boot ROM may run: fast polling of the flash interface can wedge an RP2040's boot ROM
    static constexpr auto                  FlashReadyPoll = std::chrono::milliseconds{50};
    std::vector<StallSignature::Condition> flashReady_;                    // sessionStartMutex
    std::optional<std::pair<std::uint32_t, std::uint32_t>> flashWindow_;   // sessionStartMutex
    std::mutex                                             pendingOpsMutex_;
    std::deque<std::shared_ptr<PendingOps>>                pendingOps_;
    std::atomic<bool>                                      probeListRequest{false};
    std::mutex                                             probesMutex;
    std::vector<Probe>                                     probes_;
    std::atomic<std::uint64_t>                             probesVersion_{0};
    std::jthread                                           thread;

public:
    template<typename BlockInfoF,
             typename EntryPrintF,
             typename HexFileNameF,
             typename CatalogMapF,
             typename MessageF,
             typename ErrorMessageF,
             typename ToolMessageF,
             typename ToolErrorMessageF>
    BasicJLinkRttReader(std::string                  host_,
                        std::string                  device_,
                        std::uint32_t                speed_,
                        std::string                  probe_,
                        BlockInfoF&&                 blockInfof,
                        HexFileNameF&&               hexFileNamef,
                        CatalogMapF&&                catalogMapf,
                        EntryPrintF&&                entryPrintf,
                        MessageF&&                   messagef,
                        ErrorMessageF&&              errorMessagef,
                        ToolMessageF&&               toolMessagef,
                        ToolErrorMessageF&&          toolErrorMessagef,
                        uc_log::detail::DuplexBridge duplexBridge_ = {})
      : host{std::move(host_)}
      , device{std::move(device_)}
      , speed{speed_}
      , probe{std::move(probe_)}
      , blockInfoCallback{std::forward<BlockInfoF>(blockInfof)}
      , hexFileNameCallback{std::forward<HexFileNameF>(hexFileNamef)}
      , catalogMapCallback{std::forward<CatalogMapF>(catalogMapf)}
      , entryPrintCallback{std::forward<EntryPrintF>(entryPrintf)}
      , messageCallback{std::forward<MessageF>(messagef)}
      , errorMessageCallback{std::forward<ErrorMessageF>(errorMessagef)}
      , toolMessageCallback{std::forward<ToolMessageF>(toolMessagef)}
      , toolErrorMessageCallback{std::forward<ToolErrorMessageF>(toolErrorMessagef)}
      , duplexBridge{std::move(duplexBridge_)}
      , thread{[this](std::stop_token stoken) { run(std::move(stoken)); }} {}

    Status getStatus() const { return status; }

    void resetJLink() { jlinkResetFlag = true; }

    /// Runs `ops` on the reader thread, the core running. From any thread; blocks until done or `timeout`. The default is long
    /// enough for a busy probe: on an RP2040 at 2 MHz SWD under a log flood one pass of the reader took over 250 ms
    /// (the DLL's own RTT polling holds the probe), and pokes failed.
    TargetResult accessTarget(std::span<TargetOp const> ops,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        if(pausedForReset_) { return std::unexpected{std::string{PausedMessage}}; }
        auto pending = std::make_shared<PendingOps>();
        pending->ops.assign(ops.begin(), ops.end());
        auto future = pending->result.get_future();
        {
            std::lock_guard<std::mutex> const lock{pendingOpsMutex_};
            pendingOps_.push_back(pending);
        }
        if(future.wait_for(timeout) != std::future_status::ready
           && pending->claim(PendingOps::abandoned))
        {
            return std::unexpected{std::string{"timed out: is the target connected?"}};
        }
        // done, or the reader has started it: its result is the truth, a late one included
        return future.get();
    }

    /// Called on the reader thread before every connect (jlinkPrinter: SEGGER's daily EDU terms accepted for today -
    /// a session can run past midnight).
    void setBeforeConnect(std::function<void()> f) {
        std::lock_guard<std::mutex> lock{connectionMutex};
        beforeConnectCallback = std::move(f);
    }

    /// The register words that hold while the target's flash is mapped (its boot stage has set XIP up): the firmware
    /// check reads the flash only then (flashRead). Empty: always. From any thread.
    void setFlashReadySignature(std::vector<StallSignature::Condition> conditions) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        flashReady_ = std::move(conditions);
    }

    /// The chip's flash window (XIP and its interface's registers): a control-socket read there goes through
    /// flashRead's gate. nullopt: none. From any thread.
    void setFlashWindow(std::optional<std::pair<std::uint32_t,
                                                std::uint32_t>> baseAndSize) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        flashWindow_ = baseAndSize;
    }

    /// see detail/TargetHealth.hpp; nullopt: none. From any thread.
    void setStallSignature(std::optional<StallSignature> signature) {
        health_.setStall(std::move(signature));
    }

    /// see detail/TargetHealth.hpp (a test switch: UC_LOG_STALL_FIRST_STEP). From any thread.
    void setStallFirstStep(int step) { health_.setStallFirstStep(step); }

    /// see detail/TargetHealth.hpp; nullopt: a chip that does not park. From any thread.
    void setParkSignature(std::optional<ParkSignature> signature) {
        health_.setPark(std::move(signature));
    }

    /// false: RTT through the J-Link DLL even where the printer could read it itself (from the next session on)
    void setUseDirectRtt(bool use) { useDirectRtt_ = use; }

    /// bytes of log read from the target's rings since the printer started
    std::uint64_t rttBytes() const { return rttBytes_.load(std::memory_order_relaxed); }

    /// (unread boots, count from, count to): see detail/BootLedger.hpp
    void setUnreadBootsCallback(std::function<void(std::uint32_t,
                                                   std::uint32_t,
                                                   std::uint32_t)> f) {
        std::lock_guard<std::mutex> const lock{unreadBootsMutex_};
        unreadBootsCallback_ = std::move(f);
    }

    /// the count of boots that logged starts again (the printer's own download): see restartBootCount
    void setBootCountRestartCallback(std::function<void()> f) {
        std::lock_guard<std::mutex> const lock{unreadBootsMutex_};
        bootCountRestartCallback_ = std::move(f);
    }

    /// the serial number of the probe of the last session (0 before the first)
    std::uint32_t probeSerial() const { return probeSerial_.load(std::memory_order_relaxed); }

    /// Asked at every session start: the address of the image's RTT epoch (detail/RttEpoch.hpp, from the map), which
    /// fences every ring access of the printer's own RTT reading against a boot re-initialising the block. nullopt:
    /// an image without it.
    void setRttEpochSource(std::function<std::optional<std::uint32_t>()> f) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        rttEpochCallback = std::move(f);
    }

    /// Asked at every session start and before every own flash or reset: the address of the image's count of boots
    /// that logged (detail/RttEpoch.hpp, the word after the epoch, from the map) - the unread boots are counted by
    /// it (detail/BootLedger.hpp). nullopt: an image without it, no count.
    void setLoggedBootsSource(std::function<std::optional<std::uint32_t>()> f) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        loggedBootsCallback = std::move(f);
    }

    /// Asked at every session start: what the image's catalog says about its frames (jlinkPrinter: "FrameCheck"
    /// in the string catalog). None set: either kind of frame is taken.
    void setFrameCheckSource(std::function<remote_fmt::FrameCheck()> f) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        frameCheckCallback = std::move(f);
    }

    /// Asked at every session start after the session-start callback, and then every DecodeGatePoll while it says
    /// no: whether the log may be decoded with the files at hand (jlinkPrinter: the firmware check did not say
    /// "different"). No gate: always.
    void setDecodeGate(std::function<bool(DirectMemoryRead const&)> f) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        decodeGateCallback = std::move(f);
    }

    /// Called on the reader thread at every session start, before the first log line is read.
    void setOnSessionStart(std::function<void(DirectMemoryRead const&)> f) {
        std::lock_guard<std::mutex> const lock{sessionStartMutex};
        sessionStartCallback = std::move(f);
    }

    std::uint64_t sessionCount() const { return sessionCount_.load(std::memory_order_acquire); }

    bool isHalted() const { return halted_; }

    /// Returns once the log runs again. From any thread but the reader's. `cancelled`: the caller
    /// gives up early (printer stopping, client gone).
    std::expected<void,
                  std::string>
    flashAndWait(std::chrono::milliseconds    timeout,
                 std::function<bool()> const& cancelled = {}) {
        std::uint64_t serial{};
        {
            std::lock_guard<std::mutex> const lock{flashOutcome_.mutex};
            serial = flashOutcome_.serial;
        }
        std::uint64_t session{};
        auto const    end = Clock::now() + timeout;
        flashAttempt      = 0;
        flashFlag         = true;
        bool flashed      = false;
        while(Clock::now() < end && !(cancelled && cancelled())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            if(!flashed) {
                std::lock_guard<std::mutex> const lock{flashOutcome_.mutex};
                if(flashOutcome_.serial == serial) { continue; }
                if(!flashOutcome_.error.empty()) { return std::unexpected{flashOutcome_.error}; }
                flashed = true;
                // a session that began while the request was on its way is the old firmware's
                session = flashOutcome_.sessionsBefore;
            }
            // wait for isRunning too: the firmware check sits between session start and log
            if(sessionCount() != session && getStatus().isRunning != 0) { return {}; }
        }
        // withdrawn, or it would fire whenever a target shows up
        if(!flashed && flashFlag.exchange(false)) {
            return std::unexpected{std::string{
              Clock::now() < end ? "not flashed: cancelled (the request is withdrawn)"
                                 : "not flashed: no target connected (the request is withdrawn)"}};
        }
        return std::unexpected{std::string{flashed ? "flashed, but the log did not come back"
                                           : Clock::now() < end ? "cancelled"
                                                                : "timed out"}};
    }

    std::expected<void,
                  std::string>
    resetAndWait(std::chrono::milliseconds    timeout,
                 std::function<bool()> const& cancelled = {},
                 bool                         line      = false) {
        // counted before the flag is set, so the reset that takes the flag covers this number
        auto const request = ++resetRequests_;
        auto const end     = Clock::now() + timeout;
        if(line) {
            resetLineRequested_ = true;
        }   // before the flag: the reset that takes it sees it
        targetResetFlag = true;
        std::optional<std::uint64_t> session;
        while(Clock::now() < end && !(cancelled && cancelled())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            if(!session) {
                std::lock_guard<std::mutex> const lock{resetOutcome_.mutex};
                if(resetOutcome_.serial < request) { continue; }
                if(!resetOutcome_.error.empty()) {
                    return std::unexpected{"not reset: " + resetOutcome_.error};
                }
                session = resetOutcome_.sessionsBefore;
            }
            if(sessionCount() != *session && getStatus().isRunning != 0) { return {}; }
        }
        // withdrawn, or it would reset the board whenever it comes back
        if(!session && targetResetFlag.exchange(false)) {
            resetLineRequested_ = false;
            return std::unexpected{std::string{
              Clock::now() < end ? "not reset: cancelled (the request is withdrawn)"
                                 : "not reset: no target connected (the request is withdrawn)"}};
        }
        return std::unexpected{std::string{session ? "reset, but the log did not come back"
                                           : Clock::now() < end ? "cancelled"
                                                                : "timed out"}};
    }

    // Reconnect with a different host / probe.
    void setConnection(ConnectionSettings settings) {
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            pendingConnection = std::move(settings);
        }
        jlinkResetFlag = true;
    }

    ConnectionSettings getConnection() {
        std::lock_guard<std::mutex> lock{connectionMutex};
        return pendingConnection.value_or(ConnectionSettings{host, probe});
    }

    // Ask the reader thread to list the probes; getProbes() has the result once
    // getProbesVersion() changed.
    void refreshProbes() { probeListRequest = true; }

    std::vector<Probe> getProbes() {
        std::lock_guard<std::mutex> lock{probesMutex};
        return probes_;
    }

    std::uint64_t getProbesVersion() const {
        return probesVersion_.load(std::memory_order_acquire);
    }

    void resetTarget() { targetResetFlag = true; }

    void haltTarget() { targetHaltFlag = true; }

    void continueTarget() { targetContinueFlag = true; }

    void clearAllBreakpointsTarget() { targetClearBreakPointsFlag = true; }

    void setResetType(std::uint8_t type) {
        pendingResetType.store(type, std::memory_order_relaxed);
        hasResetTypeChange.store(true, std::memory_order_release);
    }

    // Written before every reset and download (JLink::setPreResetCommands).
    void setPreResetCommands(std::vector<JLink::MemoryWrite> commands) {
        {
            std::lock_guard<std::mutex> lock{connectionMutex};
            preResetCommands = std::move(commands);
        }
        hasPreResetCommandsChange.store(true, std::memory_order_release);
    }

    /// An image that lives in RAM (a Kvasir RAM_ONLY target): a reset would boot flash, so flash
    /// and reset load it and start it at its vector table (JLink::flashRamImage) instead. `f`
    /// tells where it starts, asked anew before each of them (a rebuild may move it). Empty: an
    /// ordinary image.
    void setRamImage(std::function<RamImageStartResult()> f) {
        std::lock_guard<std::mutex> lock{connectionMutex};
        ramImageStartCallback = std::move(f);
    }

    bool isRamImage() {
        std::lock_guard<std::mutex> lock{connectionMutex};
        return static_cast<bool>(ramImageStartCallback);
    }

    /// The image's announced-reset block (Kvasir_SDK Util/AnnouncedReset.hpp): `f` gives its
    /// address, asked anew at every session start (from the map file; nullopt: none, the
    /// feature is off). The printer arms it, and when the firmware announces a reset of its own
    /// the session is closed and the probe stays off the chip for AnnouncedResetPause.
    void setAnnouncedReset(std::function<std::optional<std::uint32_t>()> f) {
        std::lock_guard<std::mutex> lock{connectionMutex};
        announcedResetAddressCallback = std::move(f);
    }

    /// True while the probe stays off the chip for an announced reset.
    bool isPausedForReset() const { return pausedForReset_; }

    void flash() { flashFlag = true; }

    bool isFlashing() const { return flashFlag || flashing_; }

    void setNoLogTimeout(std::uint32_t seconds) { noLogTimeoutSeconds_ = seconds; }
};

using JLinkRttReader = BasicJLinkRttReader<>;
