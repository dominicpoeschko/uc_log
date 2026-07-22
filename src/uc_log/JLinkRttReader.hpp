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
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
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

    static constexpr std::size_t RttBufferChunkSize = 32768;
    static constexpr auto        HaltGracePeriod    = std::chrono::seconds{60};

    // target- or host-side RTT overflow means log messages were silently dropped: record
    // the gap in the log stream itself (file, tcp, gui), not only in the status bar. The
    // marker uses the producer wire format so it parses as a regular error-level entry.
    void reportRttDataLoss(Status const&                     previous,
                           Status const&                     current,
                           std::vector<std::uint32_t> const& logChannels) {
        auto const markerChannel
          = logChannels.empty() ? std::size_t{0} : std::size_t{logChannels.front()};
        auto emitMarker = [&](std::size_t channel, std::string const& what) {
            errorMessageCallback(what);
            entryPrintCallback(channel,
                               fmt::format(R"(("uc_log", 0, 4, 0ns, """rtt""")⚠ {})", what));
        };
        if(current.hostOverflowCount > previous.hostOverflowCount) {
            emitMarker(
              markerChannel,
              fmt::format("RTT host overflow, log data lost ({} event{})",
                          current.hostOverflowCount - previous.hostOverflowCount,
                          current.hostOverflowCount - previous.hostOverflowCount == 1 ? "" : "s"));
        }
        auto const newOverflows = current.overflowMask & ~previous.overflowMask;
        if(newOverflows != 0) {
            for(std::uint32_t bit{}; bit < 32; ++bit) {
                if((newOverflows & (1U << bit)) == 0) { continue; }
                bool const isLogChannel = std::ranges::find(logChannels, bit) != logChannels.end();
                emitMarker(isLogChannel ? std::size_t{bit} : markerChannel,
                           fmt::format("RTT buffer {} overflow, target-side data lost", bit));
            }
        }
    }

    void run(std::stop_token stoken) {
        auto setStatusNotRunning = [&]() {
            Status local_status{};
            local_status.isRunning = 0;
            status                 = local_status;
        };

        while(!stoken.stop_requested()) {
            toolMessageCallback("start jlink");
            try {
                {
                    std::lock_guard<std::mutex> lock{hostMutex};
                    if(pendingHost) {
                        host = std::move(*pendingHost);
                        pendingHost.reset();
                    }
                }
                jlinkResetFlag   = false;
                TransportT jlink = [&]() {
                    if(host.empty()) {
                        return TransportT{device,
                                          speed,
                                          toolMessageCallback,
                                          toolErrorMessageCallback};
                    }
                    return TransportT{device,
                                      speed,
                                      host,
                                      toolMessageCallback,
                                      toolErrorMessageCallback};
                }();
                jlink.setResetType(pendingResetType.load(std::memory_order_relaxed));
                bool restart = false;

                if(flashFlag) {
                    toolMessageCallback("resetting target");
                    jlink.resetTarget();
                    toolMessageCallback("resetting target succeeded");
                    toolMessageCallback("flashing target");
                    jlink.flash(hexFileNameCallback());
                    toolMessageCallback("flashing target succeeded");
                    flashFlag       = false;
                    targetResetFlag = true;
                    restart         = true;
                }
                if(targetResetFlag) {
                    toolMessageCallback("resetting target");
                    jlink.resetTarget();
                    targetResetFlag = false;
                    toolMessageCallback("resetting target succeeded");
                    restart = true;
                }

                if(restart) { continue; }
                auto const stringConstantsMap = catalogMapCallback();

                auto const blockInfo  = blockInfoCallback();
                auto const rttStatus  = jlink.startRtt(blockInfo.totalBuffers, blockInfo.address);
                auto const channelMap = uc_log::detail::buildRttChannelMap(jlink,
                                                                           rttStatus,
                                                                           messageCallback,
                                                                           errorMessageCallback);
                if(duplexBridge.configure) { duplexBridge.configure(channelMap.duplexChannels); }

                std::vector<std::pair<std::uint32_t, uc_log::detail::RttChannel>> channels{};
                for(auto const upIndex : channelMap.logChannels) {
                    channels.emplace_back(upIndex, uc_log::detail::RttChannel{});
                }
                std::array<std::byte, RttBufferChunkSize> readChunk{};
                std::array<std::byte, 4096>               duplexChunk{};

                auto lastMessage      = Clock::now();
                auto lastHaltDetected = Clock::time_point{};
                bool quietWarned      = false;
                auto previousStatus   = rttStatus;

                while(!stoken.stop_requested() && !jlinkResetFlag && !targetResetFlag && !flashFlag)
                {
                    bool const haltedRecently = Clock::now() < lastHaltDetected + HaltGracePeriod;
                    for(auto& [channelId, channel] : channels) {
                        auto const data = jlink.rttRead(channelId, readChunk);
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
                    if(jlink.isHalted()) { lastHaltDetected = Clock::now(); }
                    Status const local_status = jlink.readStatus();
                    status                    = local_status;
                    reportRttDataLoss(previousStatus, local_status, channelMap.logChannels);
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
            toolMessageCallback("stopped jlink");
        }
    }

    std::string   host;
    std::string   device;
    std::uint32_t speed;

    std::function<RttBlockInfo(void)>                                   blockInfoCallback;
    std::function<std::string(void)>                                    hexFileNameCallback;
    std::function<std::unordered_map<std::uint16_t, std::string>(void)> catalogMapCallback;
    std::function<void(std::size_t, std::string_view)>                  entryPrintCallback;
    std::function<void(std::string_view)>                               messageCallback;
    std::function<void(std::string_view)>                               errorMessageCallback;
    std::function<void(std::string_view)>                               toolMessageCallback;
    std::function<void(std::string_view)>                               toolErrorMessageCallback;
    uc_log::detail::DuplexBridge                                        duplexBridge;

    std::atomic<Status>        status;
    std::atomic<std::uint32_t> noLogTimeoutSeconds_{15};
    std::atomic<bool>          targetResetFlag;
    std::atomic<bool>          targetContinueFlag;
    std::atomic<bool>          targetHaltFlag;
    std::atomic<bool>          targetClearBreakPointsFlag;
    std::atomic<bool>          jlinkResetFlag;
    std::atomic<bool>          flashFlag;
    std::atomic<std::uint8_t>  pendingResetType;
    std::atomic<bool>          hasResetTypeChange;
    std::mutex                 hostMutex;
    std::optional<std::string> pendingHost;
    std::jthread               thread;

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

    void setHost(std::string newHost) {
        {
            std::lock_guard<std::mutex> lock{hostMutex};
            pendingHost = std::move(newHost);
        }
        jlinkResetFlag = true;
    }

    std::string getHost() {
        std::lock_guard<std::mutex> lock{hostMutex};
        return pendingHost.value_or(host);
    }

    void resetTarget() { targetResetFlag = true; }

    void haltTarget() { targetHaltFlag = true; }

    void continueTarget() { targetContinueFlag = true; }

    void clearAllBreakpointsTarget() { targetClearBreakPointsFlag = true; }

    void setResetType(std::uint8_t type) {
        pendingResetType.store(type, std::memory_order_relaxed);
        hasResetTypeChange.store(true, std::memory_order_release);
    }

    void flash() { flashFlag = true; }

    bool isFlashing() const { return flashFlag; }

    void setNoLogTimeout(std::uint32_t seconds) { noLogTimeoutSeconds_ = seconds; }
};

using JLinkRttReader = BasicJLinkRttReader<>;
