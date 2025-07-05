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

public:
    struct Config {
        std::string   host;
        std::string   device;
        std::uint32_t speed;
        std::uint32_t numChannels;

        std::atomic<JLink::Status>& status;
        std::atomic<bool>&          targetResetFlag;
        std::atomic<bool>&          jlinkResetFlag;
        std::atomic<bool>&          flashFlag;

        std::function<std::uint32_t(void)>                                  blockAddressCallback;
        std::function<std::string(void)>                                    hexFileNameCallback;
        std::function<std::unordered_map<std::uint16_t, std::string>(void)> catalogMapCallback;
        std::function<void(std::size_t, std::string_view)>                  entryPrintCallback;
        std::function<void(std::string_view)>                               messageCallback;
        std::function<void(std::string_view)>                               errorMessageCallback;

        [[nodiscard]] bool isValid() const noexcept {
            return !device.empty() && speed > 0 && numChannels > 0 && blockAddressCallback
                && hexFileNameCallback && catalogMapCallback && entryPrintCallback
                && messageCallback && errorMessageCallback;
        }
    };

private:
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
                                    std::string>              stringConstantsMap,
                 std::function<void(std::string_view)>        errorMessagef) {
            read(jlink, channel);
            bool gotMessage{};
            if(!buffer.empty()) {
                while(!stoken.stop_requested()) {
                    auto const [os, subrange, unparsed_bytes]
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
                    if(os) {
                        lastValidRead = Clock::now();
                        gotMessage    = true;
                        printF(channel, *os);
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

    static void run(std::stop_token stoken,
                    Config const&   config) {
        if(!config.isValid()) {
            return;
        }

        auto setStatusNotRunning = [&]() {
            JLink::Status local_status{};
            local_status.isRunning = 0;
            config.status          = local_status;
        };

        while(!stoken.stop_requested()) {
            config.messageCallback("start jlink");
            try {
                config.jlinkResetFlag = false;
                JLink jlink           = [&]() {
                    if(config.host.empty()) {
                        return JLink{config.device, config.speed};
                    }
                    return JLink{config.device, config.speed, config.host};
                }();

                bool restart = false;

                if(config.flashFlag) {
                    config.messageCallback("flashing target");
                    jlink.flash(config.hexFileNameCallback());
                    config.messageCallback("flashing target succeeded");
                    config.flashFlag       = false;
                    config.targetResetFlag = true;
                    restart                = true;
                }
                if(config.targetResetFlag) {
                    config.messageCallback("resetting target");
                    jlink.resetTarget();
                    config.targetResetFlag = false;
                    config.messageCallback("resetting target succeeded");
                    restart = true;
                }

                if(restart) {
                    continue;
                }

                auto const stringConstantsMap = config.catalogMapCallback();

                jlink.startRtt(config.numChannels, config.blockAddressCallback());
                std::vector<Channel> channels{};
                channels.resize(config.numChannels);
                auto lastMessage = Clock::now();

                while(!stoken.stop_requested() && !config.jlinkResetFlag && !config.targetResetFlag
                      && !config.flashFlag
                      && !(Clock::now() > lastMessage + std::chrono::seconds{5}))
                {
                    for(std::size_t id{}; auto& channel : channels) {
                        if(channel.run(stoken,
                                       jlink,
                                       config.entryPrintCallback,
                                       static_cast<std::uint32_t>(id++),
                                       stringConstantsMap,
                                       config.errorMessageCallback))
                        {
                            lastMessage = Clock::now();
                        }
                    }
                    jlink.checkConnected();
                    JLink::Status local_status = jlink.readStatus();
                    config.status              = local_status;
                    if(local_status.isRunning == 0
                       || local_status.numUpBuffers != static_cast<int>(config.numChannels))
                    {
                        throw std::runtime_error("lost connection");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            } catch(std::exception const& e) {
                config.errorMessageCallback(fmt::format("caught {}", +e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            }
            setStatusNotRunning();
            config.messageCallback("stopped jlink");
        }
    }

    std::atomic<JLink::Status> status{};
    std::atomic<bool>          targetResetFlag{};
    std::atomic<bool>          jlinkResetFlag{};
    std::atomic<bool>          flashFlag{};
    std::jthread               thread;

public:
    template<typename BlockAddressF,
             typename EntryPrintF,
             typename HexFileNameF,
             typename CatalogMapF,
             typename MessageF,
             typename ErrorMessageF>
    JLinkRttReader(std::string     host,
                   std::string     device,
                   std::uint32_t   speed,
                   std::uint32_t   channels,
                   BlockAddressF&& blockAddressf,
                   HexFileNameF&&  hexFileNamef,
                   CatalogMapF&&   catalogMapf,
                   EntryPrintF&&   entryPrintf,
                   MessageF&&      messagef,
                   ErrorMessageF&& errorMessagef)
      : thread{[&](std::stop_token stoken) {
          run(stoken,
              Config{.host                 = std::move(host),
                     .device               = std::move(device),
                     .speed                = speed,
                     .numChannels          = channels,
                     .status               = status,
                     .targetResetFlag      = targetResetFlag,
                     .jlinkResetFlag       = jlinkResetFlag,
                     .flashFlag            = flashFlag,
                     .blockAddressCallback = std::forward<BlockAddressF>(blockAddressf),
                     .hexFileNameCallback  = std::forward<HexFileNameF>(hexFileNamef),
                     .catalogMapCallback   = std::forward<CatalogMapF>(catalogMapf),
                     .entryPrintCallback   = std::forward<EntryPrintF>(entryPrintf),
                     .messageCallback      = std::forward<MessageF>(messagef),
                     .errorMessageCallback = std::forward<ErrorMessageF>(errorMessagef)});
      }} {}

    JLink::Status getStatus() { return status; }

    void resetJLink() { jlinkResetFlag = true; }

    void resetTarget() { targetResetFlag = true; }

    void flash() { flashFlag = true; }
};
