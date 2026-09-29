#pragma once
#include "jlink/JLink.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/detail/RttChannel.hpp"
#include "uc_log/detail/RttChannelMap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
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
private:
    using Clock  = std::chrono::steady_clock;
    using Status = typename TransportT::Status;

public:
    using Probe = typename TransportT::Probe;

    /// With `write`: that word is written first, then 4 bytes are read back.
    struct MemoryAccess {
        std::uint32_t                address{};
        std::uint32_t                size{};
        std::optional<std::uint32_t> write{};
    };

    using MemoryResult = std::expected<std::vector<std::vector<std::byte>>, std::string>;

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
    static constexpr std::size_t   RttBufferChunkSize     = 32768;
    static constexpr auto          HaltGracePeriod        = std::chrono::seconds{60};
    static constexpr auto          ControlBlockDumpPeriod = std::chrono::seconds{60};
    static constexpr auto          OverflowReportPeriod   = std::chrono::seconds{5};
    static constexpr std::uint32_t HaltStackWords         = 24;
    static constexpr unsigned      FlashAttempts          = 3;
    // DHCSR at 0xE000EDF0, bit 25 S_RESET_ST: at least one reset since the last read of the
    // DHCSR, cleared by that read. Armv6-M ARM DDI 0419E C1.6.3 (Table C1-10 for the address),
    // Armv7-M ARM DDI 0403E.e C1.6.2, and for the M33 the RP2350 data sheet 3.7, Table 252.
    static constexpr std::uint32_t DhcsrAddress = 0xE000'EDF0;
    static constexpr std::uint32_t DhcsrResetSt = 1U << 25;
    // DLL reads taken after an outside reset, before the new session: bounds the drain of a
    // target that already talks again
    static constexpr int  ResetDrainReads  = 64;
    static constexpr auto ControlBlockPoll = std::chrono::milliseconds{20};

    // true when the core was reset since the last look (reading clears the bit); a failed read
    // says no
    template<typename Transport>
    static bool resetSinceLastLook(Transport& jlink) {
        std::array<std::byte, 4> raw{};
        try {
            jlink.readMemory(DhcsrAddress, raw);
        } catch(std::exception const&) { return false; }
        std::uint32_t value{};
        for(std::size_t i = 0; i != raw.size(); ++i) {
            value |= std::to_integer<std::uint32_t>(raw[i]) << (8 * i);
        }
        return (value & DhcsrResetSt) != 0;
    }

    // the control block's id ("SEGGER RTT", rtt.hpp) is at its address: the image has started
    template<typename Transport>
    static bool controlBlockPresent(Transport&    jlink,
                                    std::uint32_t address) {
        static constexpr std::string_view Id{"SEGGER RTT"};
        std::array<std::byte, Id.size()>  raw{};
        try {
            jlink.readMemory(address, raw);
        } catch(std::exception const&) { return false; }
        return std::ranges::equal(raw, Id, [](std::byte b, char c) {
            return std::to_integer<char>(b) == c;
        });
    }

    // After a reset from outside, the next session waits for the control block: the target may
    // run its boot ROM for a while (RP2350 BOOTSEL zeroes the block), and a session begun then
    // reports its flash as a firmware mismatch and fails startRtt after seconds of RAM scans.
    template<typename Transport>
    void awaitControlBlockAfterReset(Transport&             jlink,
                                     std::stop_token const& stoken) {
        if(!awaitControlBlock_) { return; }
        awaitControlBlock_ = false;
        auto const address = blockInfoCallback().address;
        if(address == 0) { return; }
        bool told = false;
        while(!stoken.stop_requested() && !targetResetFlag && !flashFlag && !jlinkResetFlag
              && !controlBlockPresent(jlink, address))
        {
            if(!told) {
                messageCallback(
                  fmt::format("waiting for the RTT control block at {:#010x}: the "
                              "target runs something else (its boot ROM?)",
                              address));
                told = true;
            }
            serviceMemoryAccesses(jlink);
            std::this_thread::sleep_for(ControlBlockPoll);
        }
    }

    // Host overflows come once per read pass: summed up, reported once per OverflowReportPeriod.
    struct LossReport {
        int               events{};
        std::uint32_t     transferred{};
        std::uint32_t     read{};
        Clock::time_point lastReport{};
    };

    bool reportRttDataLoss(Status const& previous,
                           Status const& current,
                           LossReport&   report) {
        // a counter that went down was restarted by the DLL
        auto const delta = [](std::uint32_t before, std::uint32_t now) {
            return now >= before ? now - before : now;
        };
        bool lost = false;
        report.transferred += delta(previous.numBytesTransferred, current.numBytesTransferred);
        report.read += delta(previous.numBytesRead, current.numBytesRead);
        if(current.hostOverflowCount > previous.hostOverflowCount) {
            lost = true;
            report.events += current.hostOverflowCount - previous.hostOverflowCount;
        }
        if(report.events != 0 && Clock::now() > report.lastReport + OverflowReportPeriod) {
            errorMessageCallback(
              fmt::format("⚠ RTT host overflow, log data lost ({} event{}; since the last report "
                          "{} bytes from the target, {} read by the printer)",
                          report.events,
                          report.events == 1 ? "" : "s",
                          report.transferred,
                          report.read));
            report.events     = 0;
            report.lastReport = Clock::now();
        }
        if(report.events == 0) {
            report.transferred = 0;
            report.read        = 0;
        }
        auto const newOverflows = current.overflowMask & ~previous.overflowMask;
        for(std::uint32_t bit{}; bit < 32; ++bit) {
            if((newOverflows & (1U << bit)) == 0) { continue; }
            errorMessageCallback(
              fmt::format("⚠ RTT buffer {} overflow, target-side data lost", bit));
            lost = true;
        }
        return lost;
    }

    // After data loss: is the control block still sane? Layout as in rtt/src/rtt/rtt.hpp.
    template<typename Transport>
    void dumpControlBlock(Transport&    jlink,
                          std::uint32_t address,
                          std::uint32_t totalBuffers) {
        std::string text;
        if(address == 0) { return; }   // block found by the DLL's search: no address to read
        try {
            std::vector<std::byte> raw(24U + 24U * std::min<std::uint32_t>(totalBuffers, 16U));
            jlink.readMemory(address, raw);
            auto const word = [&](std::size_t offset) {
                std::uint32_t v{};
                for(std::size_t i = 0; i != 4; ++i) {
                    v |= std::to_integer<std::uint32_t>(raw[offset + i]) << (8U * i);
                }
                return v;
            };
            std::string id;
            for(std::size_t i = 0; i != 16 && raw[i] != std::byte{}; ++i) {
                auto const c = std::to_integer<unsigned char>(raw[i]);
                id += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
            }
            bool sane = id == "SEGGER RTT";
            text      = fmt::format("RTT control block at {:#010x}: id {:?} up {} down {}",
                                    address,
                                    id,
                                    word(16),
                                    word(20));
            for(std::size_t b = 0; 24U + 24U * (b + 1U) <= raw.size(); ++b) {
                auto const base = 24U + 24U * b;
                auto const size = word(base + 8);
                auto const wr   = word(base + 12);
                auto const rd   = word(base + 16);
                bool const bad  = size == 0 || wr >= size || rd >= size;
                sane            = sane && !bad;
                text += fmt::format(" | {} buf={:#010x} size={} wr={} rd={} flags={}{}",
                                    b,
                                    word(base + 4),
                                    size,
                                    wr,
                                    rd,
                                    word(base + 20),
                                    bad ? " OUT OF RANGE" : "");
            }
            text += sane ? " - looks sane" : " - CORRUPT";
        } catch(std::exception const& e) {
            text = fmt::format("RTT control block at {:#010x} could not be read: {}",
                               address,
                               e.what());
        }
        errorMessageCallback(text);
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

    // Serves only what is queued now: clients reading in a loop would otherwise starve the RTT
    // reads.
    template<typename Transport>
    void serviceMemoryAccesses(Transport& jlink) {
        std::deque<std::shared_ptr<PendingMemoryAccess>> batch;
        {
            std::lock_guard<std::mutex> const lock{memoryAccessesMutex};
            batch.swap(memoryAccesses);
        }
        for(auto const& pending : batch) {
            if(pending->abandoned) { continue; }
            MemoryAccess current{};
            try {
                std::vector<std::vector<std::byte>> data;
                data.reserve(pending->accesses.size());
                for(auto const& r : pending->accesses) {
                    current = r;
                    if(r.write) { jlink.writeWord(r.address, *r.write); }
                    data.emplace_back(r.write ? 4U : r.size);
                    jlink.readMemory(r.address, data.back());
                }
                pending->result.set_value(std::move(data));
            } catch(std::exception const& e) {
                pending->result.set_value(
                  std::unexpected{current.write ? fmt::format("{} (writing {:#010x} at {:#010x})",
                                                              e.what(),
                                                              *current.write,
                                                              current.address)
                                                : fmt::format("{} (reading {} bytes at {:#010x})",
                                                              e.what(),
                                                              current.size,
                                                              current.address)});
            }
        }
    }

    // Only reads; whoever halted the core resumes it. Unreadable words show as ????????.
    template<typename Transport>
    void captureHalt(Transport& jlink) {
        using Reg           = typename Transport::CoreRegister;
        auto const readWord = [&](std::uint32_t address) -> std::optional<std::uint32_t> {
            std::array<std::byte, 4> raw{};
            try {
                jlink.readMemory(address, raw);
            } catch(std::exception const&) { return std::nullopt; }
            std::uint32_t v{};
            for(std::size_t i = 0; i != 4; ++i) {
                v |= std::to_integer<std::uint32_t>(raw[i]) << (8U * i);
            }
            return v;
        };
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

    void failMemoryAccesses(std::string const& why) {
        std::lock_guard<std::mutex> const lock{memoryAccessesMutex};
        for(auto& pending : memoryAccesses) {
            if(!pending->abandoned) { pending->result.set_value(std::unexpected{why}); }
        }
        memoryAccesses.clear();
    }

    void run(std::stop_token stoken) {
        auto setStatusNotRunning = [&]() {
            Status local_status{};
            local_status.isRunning = 0;
            status                 = local_status;
            halted_                = false;
        };

        while(!stoken.stop_requested()) {
            toolMessageCallback("start jlink");
            try {
                {
                    std::lock_guard<std::mutex> lock{connectionMutex};
                    if(pendingConnection) {
                        host  = std::move(pendingConnection->host);
                        probe = std::move(pendingConnection->probe);
                        pendingConnection.reset();
                    }
                }
                jlinkResetFlag = false;
                serviceProbeListRequest();
                TransportT jlink{
                  device,
                  speed,
                  typename TransportT::Connection{host, 19020, probe},
                  toolMessageCallback,
                  toolErrorMessageCallback
                };
                jlink.setResetType(pendingResetType.load(std::memory_order_relaxed));
                applyPreResetCommands(jlink);
                bool restart = false;

                // taken while it runs: a request can only be withdrawn before or between attempts
                if(flashFlag.exchange(false)) {
                    flashing_ = true;
                    try {
                        toolMessageCallback("resetting target");
                        jlink.resetTarget();
                        toolMessageCallback("resetting target succeeded");
                        toolMessageCallback("flashing target");
                        jlink.flash(hexFileNameCallback());
                        toolMessageCallback("flashing target succeeded");
                    } catch(std::exception const& e) {
                        // a J-Link with a second client fails about every other download
                        if(++flashAttempt >= FlashAttempts) {
                            flashAttempt = 0;
                            setOutcome(flashOutcome_, std::string{e.what()}, sessionCount());
                        } else {
                            flashFlag = true;
                        }
                        flashing_ = false;
                        throw;
                    }
                    flashAttempt    = 0;
                    flashing_       = false;
                    targetResetFlag = true;
                    restart         = true;
                    setOutcome(flashOutcome_, std::string{}, sessionCount());
                }
                if(targetResetFlag.exchange(false)) {
                    // every resetAndWait() numbered up to here is served by this reset
                    auto const covered = resetRequests_.load();
                    toolMessageCallback("resetting target");
                    try {
                        jlink.resetTarget();
                    } catch(...) {
                        targetResetFlag = true;
                        throw;
                    }
                    toolMessageCallback("resetting target succeeded");
                    {
                        std::lock_guard<std::mutex> const lock{resetOutcome_.mutex};
                        resetOutcome_.serial         = covered;
                        resetOutcome_.sessionsBefore = sessionCount();
                    }
                    restart = true;
                }

                if(restart) { continue; }
                awaitControlBlockAfterReset(jlink, stoken);
                // our own reset (or flash) set it: only a reset after this point is from outside
                (void)resetSinceLastLook(jlink);
                auto const stringConstantsMap = catalogMapCallback();

                // Firmware check before startRtt: until then the target keeps its lines in its
                // rings, so the slow image read loses none.
                {
                    std::function<void(DirectMemoryRead const&)> onStart;
                    {
                        std::lock_guard<std::mutex> const lock{sessionStartMutex};
                        onStart = sessionStartCallback;
                    }
                    if(onStart) {
                        onStart([&jlink](std::uint32_t address, std::span<std::byte> out) {
                            try {
                                jlink.readMemory(address, out);
                                return true;
                            } catch(std::exception const&) { return false; }
                        });
                    }
                }

                auto const blockInfo  = blockInfoCallback();
                auto const rttStatus  = jlink.startRtt(blockInfo.totalBuffers, blockInfo.address);
                auto const channelMap = uc_log::detail::buildRttChannelMap(jlink,
                                                                           rttStatus,
                                                                           messageCallback,
                                                                           errorMessageCallback);
                if(duplexBridge.configure) { duplexBridge.configure(channelMap.duplexChannels); }

                struct LogChannel {
                    std::uint32_t              upIndex;
                    std::uint32_t              channel;
                    uc_log::detail::RttChannel rtt;
                };

                std::vector<LogChannel> channels{};
                for(auto const& log : channelMap.logChannels) {
                    channels.push_back({log.upIndex, log.channel, uc_log::detail::RttChannel{}});
                }
                std::array<std::byte, RttBufferChunkSize> readChunk{};
                std::array<std::byte, 4096>               duplexChunk{};

                sessionCount_.fetch_add(1, std::memory_order_release);

                auto lastMessage      = Clock::now();
                auto lastHaltDetected = Clock::time_point{};
                bool wasHalted        = false;
                bool quietWarned      = false;
                auto previousStatus   = rttStatus;
                auto lastBlockDump    = Clock::time_point{};
                auto lossReport       = LossReport{};

                while(!stoken.stop_requested() && !jlinkResetFlag && !targetResetFlag && !flashFlag)
                {
                    bool const haltedRecently = Clock::now() < lastHaltDetected + HaltGracePeriod;
                    if(resetSinceLastLook(jlink)) {
                        // Someone else reset the target (picotool, a watchdog, the button).
                        // SEGGER's RTT reader carries on across it, and when the new image - or
                        // the RP2350 boot ROM, which zeroes the block - rewrites the control
                        // block 10-20 ms later, it reads an old lap of a ring: lines a second
                        // time, then garbage (i2c_testing combo, picotool reboot -u,
                        // 2026-09-25). What the DLL holds now it read before that: take it,
                        // then start over as after a reset of our own.
                        std::size_t unfinished{};
                        for(auto& [upIndex, channelId, channel] : channels) {
                            for(int i = 0; i != ResetDrainReads; ++i) {
                                auto const data = jlink.rttRead(upIndex, readChunk);
                                if(data.empty()) { break; }
                                channel.append(data);
                            }
                            channel.drain([&stoken]() { return stoken.stop_requested(); },
                                          entryPrintCallback,
                                          channelId,
                                          stringConstantsMap,
                                          errorMessageCallback,
                                          haltedRecently);
                            unfinished += channel.buffer.size();
                        }
                        messageCallback(fmt::format(
                          "the target was reset from outside the printer: a new RTT session{}",
                          unfinished == 0
                            ? std::string{}
                            : fmt::format(" ({} byte{} of a line cut off by the reset dropped)",
                                          unfinished,
                                          unfinished == 1 ? "" : "s")));
                        awaitControlBlock_ = true;
                        break;
                    }
                    for(auto& [upIndex, channelId, channel] : channels) {
                        auto const data = jlink.rttRead(upIndex, readChunk);
                        if(!data.empty()) { channel.append(data); }
                        if(channel.drain([&stoken]() { return stoken.stop_requested(); },
                                         entryPrintCallback,
                                         channelId,
                                         stringConstantsMap,
                                         errorMessageCallback,
                                         haltedRecently))
                        {
                            lastMessage = Clock::now();
                            quietWarned = false;
                        }
                    }
                    for(auto const& dc : channelMap.duplexChannels) {
                        if(dc.upIndex && duplexBridge.sendToClient) {
                            auto const data = jlink.rttRead(*dc.upIndex, duplexChunk);
                            if(!data.empty()) {
                                // live duplex traffic proves the link is alive
                                lastMessage = Clock::now();
                                duplexBridge.sendToClient(dc.ordinal, data);
                            }
                        }
                        if(duplexBridge.peekFromClient && duplexBridge.consumeFromClient) {
                            auto const pending
                              = duplexBridge.peekFromClient(dc.ordinal, duplexChunk);
                            if(pending != 0) {
                                auto const written = jlink.rttWrite(
                                  dc.downIndex,
                                  std::span<std::byte const>{duplexChunk}.first(pending));
                                if(written != 0) {
                                    duplexBridge.consumeFromClient(dc.ordinal, written);
                                }
                            }
                        }
                    }
                    jlink.checkConnected();
                    bool const halted = jlink.isHalted();
                    halted_           = halted;
                    if(halted) {
                        lastHaltDetected = Clock::now();
                        if(!wasHalted) { captureHalt(jlink); }
                    }
                    wasHalted                 = halted;
                    Status const local_status = jlink.readStatus();
                    status                    = local_status;
                    if(reportRttDataLoss(previousStatus, local_status, lossReport)
                       && Clock::now() > lastBlockDump + ControlBlockDumpPeriod)
                    {
                        lastBlockDump = Clock::now();
                        dumpControlBlock(jlink, blockInfo.address, blockInfo.totalBuffers);
                    }
                    previousStatus = local_status;
                    if(local_status.isRunning == 0
                       || local_status.numUpBuffers != rttStatus.numUpBuffers
                       || local_status.numDownBuffers != rttStatus.numDownBuffers)
                    {
                        throw std::runtime_error("lost connection");
                    }
                    // a quiet target is not a dead link: connection loss is detected via
                    // checkConnected/readStatus above, so only warn here, never tear down
                    auto const quietSeconds = noLogTimeoutSeconds_.load();
                    if(quietSeconds != 0 && !quietWarned
                       && Clock::now() > lastMessage + std::chrono::seconds{quietSeconds})
                    {
                        quietWarned = true;
                        messageCallback(
                          fmt::format("no log messages for {} s, link is alive", quietSeconds));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    if(targetContinueFlag) {
                        targetContinueFlag = false;
                        jlink.go();
                    }
                    if(targetHaltFlag) {
                        targetHaltFlag = false;
                        jlink.halt();
                    }
                    if(targetClearBreakPointsFlag) {
                        targetClearBreakPointsFlag = false;
                        jlink.clearAllBreakpoints();
                    }
                    if(hasResetTypeChange.exchange(false, std::memory_order_acquire)) {
                        jlink.setResetType(pendingResetType.load(std::memory_order_relaxed));
                    }
                    if(hasPreResetCommandsChange.load(std::memory_order_acquire)) {
                        applyPreResetCommands(jlink);
                    }
                    serviceProbeListRequest();
                    serviceMemoryAccesses(jlink);
                }
            } catch(std::exception const& e) {
                toolErrorMessageCallback(fmt::format("caught {}", e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            } catch(...) {
                // a non-std exception (e.g. from a callback) must reconnect, not terminate
                toolErrorMessageCallback("caught unknown exception");
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            }
            setStatusNotRunning();
            failMemoryAccesses("the target is not connected");
            toolMessageCallback("stopped jlink");
        }
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
    bool                              awaitControlBlock_{};   // reader thread only
    std::atomic<std::uint8_t>         pendingResetType;
    std::atomic<bool>                 hasResetTypeChange;
    std::mutex                        connectionMutex;
    std::optional<ConnectionSettings> pendingConnection;
    std::atomic<bool>                 hasPreResetCommandsChange;
    std::vector<JLink::MemoryWrite>   preResetCommands;   // guarded by connectionMutex

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
    struct PendingMemoryAccess {
        std::vector<MemoryAccess>  accesses;
        std::promise<MemoryResult> result;
        std::atomic<bool>          abandoned{false};
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

    Outcome                                          flashOutcome_;
    Outcome                                          resetOutcome_;   // serial: requests covered
    std::atomic<std::uint64_t>                       resetRequests_{0};
    std::atomic<unsigned>                            flashAttempt{};
    std::atomic<bool>                                flashing_{false};
    std::atomic<std::uint64_t>                       sessionCount_{0};
    std::atomic<bool>                                halted_{false};
    std::mutex                                       sessionStartMutex;
    std::function<void(DirectMemoryRead const&)>     sessionStartCallback;
    std::mutex                                       memoryAccessesMutex;
    std::deque<std::shared_ptr<PendingMemoryAccess>> memoryAccesses;
    std::atomic<bool>                                probeListRequest{false};
    std::mutex                                       probesMutex;
    std::vector<Probe>                               probes_;
    std::atomic<std::uint64_t>                       probesVersion_{0};
    std::jthread                                     thread;

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

    /// Read (and write) while the core runs. From any thread; blocks until done or `timeout`.
    MemoryResult accessMemory(std::span<MemoryAccess const> accesses,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds{250}) {
        auto pending = std::make_shared<PendingMemoryAccess>();
        pending->accesses.assign(accesses.begin(), accesses.end());
        auto future = pending->result.get_future();
        {
            std::lock_guard<std::mutex> const lock{memoryAccessesMutex};
            memoryAccesses.push_back(pending);
        }
        if(future.wait_for(timeout) != std::future_status::ready) {
            pending->abandoned = true;
            return std::unexpected{std::string{"timed out: is the target connected?"}};
        }
        return future.get();
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
                 std::function<bool()> const& cancelled = {}) {
        // counted before the flag is set, so the reset that takes the flag covers this number
        auto const request = ++resetRequests_;
        auto const end     = Clock::now() + timeout;
        targetResetFlag    = true;
        std::optional<std::uint64_t> session;
        while(Clock::now() < end && !(cancelled && cancelled())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            if(!session) {
                std::lock_guard<std::mutex> const lock{resetOutcome_.mutex};
                if(resetOutcome_.serial < request) { continue; }
                session = resetOutcome_.sessionsBefore;
            }
            if(sessionCount() != *session && getStatus().isRunning != 0) { return {}; }
        }
        // withdrawn, or it would reset the board whenever it comes back
        if(!session && targetResetFlag.exchange(false)) {
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

    void flash() { flashFlag = true; }

    bool isFlashing() const { return flashFlag || flashing_; }

    void setNoLogTimeout(std::uint32_t seconds) { noLogTimeoutSeconds_ = seconds; }
};

using JLinkRttReader = BasicJLinkRttReader<>;
