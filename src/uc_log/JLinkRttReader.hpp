#pragma once
#include "jlink/JLink.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

struct JLinkRttReader {
private:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t RttBufferChunkSize = 32768;
    static constexpr auto        RttTimeout         = std::chrono::milliseconds{100};

    struct Channel {
        std::vector<std::byte> buffer;
        Clock::time_point      lastValidRead{Clock::now()};

        void read(JLink&        jlink,
                  std::uint32_t channel) {
            auto const oldSize = buffer.size();
            buffer.resize(oldSize + RttBufferChunkSize);
            auto span      = std::span{buffer};
            span           = span.subspan(oldSize);
            auto const ret = jlink.rttRead(channel, span);
            buffer.resize(oldSize + ret.size());
        }

        bool run(std::stop_token&                             stoken,
                 JLink&                                       jlink,
                 std::function<void(std::size_t,
                                    std::string_view)> const& printF,
                 std::uint32_t                                channel,
                 std::unordered_map<std::uint16_t,
                                    std::string> const&       stringConstantsMap,
                 std::function<void(std::string_view)> const& errorMessagef) {
            read(jlink, channel);
            bool gotMessage{};
            if(!buffer.empty()) {
                while(!stoken.stop_requested()) {
                    auto const [output_stream, subrange, unparsed_bytes]
                      = remote_fmt::parse(buffer, stringConstantsMap, errorMessagef);
                    buffer.erase(buffer.begin(),
                                 std::next(buffer.begin(),
                                           static_cast<std::make_signed_t<std::size_t>>(
                                             buffer.size() - subrange.size())));
                    if(unparsed_bytes != 0) {
                        errorMessagef(fmt::format("channel {} corrupted data removed {} byte{}",
                                                  channel,
                                                  unparsed_bytes,
                                                  unparsed_bytes == 1 ? "" : "s"));
                    }
                    if(output_stream) {
                        lastValidRead = Clock::now();
                        gotMessage    = true;
                        printF(channel, *output_stream);
                        continue;
                    }
                    break;
                }
                if(Clock::now() > lastValidRead + RttTimeout && !buffer.empty()) {
                    buffer.erase(buffer.begin());
                    errorMessagef(fmt::format("channel {} timeout removed 1 byte", channel));
                }
            } else {
                lastValidRead = Clock::now();
            }
            return gotMessage;
        }
    };

    void run(std::stop_token stoken) {
        auto setStatusNotRunning = [&]() {
            JLink::Status local_status{};
            local_status.isRunning = 0;
            status                 = local_status;
        };

        while(!stoken.stop_requested()) {
            toolMessageCallback("start jlink");
            try {
                jlinkResetFlag = false;
                JLink jlink    = [&]() {
                    if(host.empty()) {
                        return JLink{device, speed, toolMessageCallback, toolErrorMessageCallback};
                    }
                    return JLink{device,
                                 speed,
                                 host,
                                 toolMessageCallback,
                                 toolErrorMessageCallback};
                }();
                bool restart = false;

                if(flashFlag) {
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

                if(restart) {
                    continue;
                }
                auto const stringConstantsMap = catalogMapCallback();

                jlink.startRtt(numChannels, blockAddressCallback());
                std::vector<Channel> channels{};
                channels.resize(numChannels);
                auto lastMessage      = Clock::now();
                auto lastHaltDetected = Clock::time_point{};

                while(!stoken.stop_requested() && !jlinkResetFlag && !targetResetFlag && !flashFlag
                      && !((Clock::now() > lastMessage + std::chrono::seconds{5})
                           && (Clock::now() > lastHaltDetected + std::chrono::seconds{60})))
                {
                    for(std::size_t channelId{}; auto& channel : channels) {
                        if(channel.run(stoken,
                                       jlink,
                                       entryPrintCallback,
                                       static_cast<std::uint32_t>(channelId++),
                                       stringConstantsMap,
                                       errorMessageCallback))
                        {
                            lastMessage = Clock::now();
                        }
                    }
                    jlink.checkConnected();
                    if(jlink.isHalted()) {
                        lastHaltDetected = Clock::now();
                    }
                    JLink::Status const local_status = jlink.readStatus();
                    status                           = local_status;
                    if(local_status.isRunning == 0
                       || local_status.numUpBuffers != static_cast<int>(numChannels))
                    {
                        throw std::runtime_error("lost connection");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            } catch(std::exception const& e) {
                toolErrorMessageCallback(fmt::format("caught {}", e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            }
            setStatusNotRunning();
            toolMessageCallback("stopped jlink");
        }
    }

    std::string   host;
    std::string   device;
    std::uint32_t speed;
    std::uint32_t numChannels;

    std::function<std::uint32_t(void)>                                  blockAddressCallback;
    std::function<std::string(void)>                                    hexFileNameCallback;
    std::function<std::unordered_map<std::uint16_t, std::string>(void)> catalogMapCallback;
    std::function<void(std::size_t, std::string_view)>                  entryPrintCallback;
    std::function<void(std::string_view)>                               messageCallback;
    std::function<void(std::string_view)>                               errorMessageCallback;
    std::function<void(std::string_view)>                               toolMessageCallback;
    std::function<void(std::string_view)>                               toolErrorMessageCallback;

    std::atomic<JLink::Status> status;
    std::atomic<bool>          targetResetFlag;
    std::atomic<bool>          jlinkResetFlag;
    std::atomic<bool>          flashFlag;
    std::jthread               thread;

public:
    template<typename BlockAddressF,
             typename EntryPrintF,
             typename HexFileNameF,
             typename CatalogMapF,
             typename MessageF,
             typename ErrorMessageF,
             typename ToolMessageF,
             typename ToolErrorMessageF>
    JLinkRttReader(std::string         host_,
                   std::string         device_,
                   std::uint32_t       speed_,
                   std::uint32_t       numChannels_,
                   BlockAddressF&&     blockAddressf,
                   HexFileNameF&&      hexFileNamef,
                   CatalogMapF&&       catalogMapf,
                   EntryPrintF&&       entryPrintf,
                   MessageF&&          messagef,
                   ErrorMessageF&&     errorMessagef,
                   ToolMessageF&&      toolMessagef,
                   ToolErrorMessageF&& toolErrorMessagef)
      : host{std::move(host_)}
      , device{std::move(device_)}
      , speed{speed_}
      , numChannels{numChannels_}
      , blockAddressCallback{std::forward<BlockAddressF>(blockAddressf)}
      , hexFileNameCallback{std::forward<HexFileNameF>(hexFileNamef)}
      , catalogMapCallback{std::forward<CatalogMapF>(catalogMapf)}
      , entryPrintCallback{std::forward<EntryPrintF>(entryPrintf)}
      , messageCallback{std::forward<MessageF>(messagef)}
      , errorMessageCallback{std::forward<ErrorMessageF>(errorMessagef)}
      , toolMessageCallback{std::forward<ToolMessageF>(toolMessagef)}
      , toolErrorMessageCallback{std::forward<ToolErrorMessageF>(toolErrorMessagef)}
      , thread{[this](std::stop_token stoken) { run(std::move(stoken)); }} {}

    JLink::Status getStatus() const { return status; }

    void resetJLink() { jlinkResetFlag = true; }

    void resetTarget() { targetResetFlag = true; }

    void flash() { flashFlag = true; }

    bool isFlashing() const { return flashFlag; }
};
