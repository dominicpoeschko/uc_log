#pragma once
#include "jlink/JLink.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <thread>
#include <vector>

struct JLinkRttReader {
private:
    using Clock = std::chrono::steady_clock;
    struct Channel {
        std::vector<std::byte> buffer;
        Clock::time_point      lastValidRead{Clock::now()};

        void read(JLink& jlink, std::uint32_t channel) {
            auto const oldSize = buffer.size();
            buffer.resize(oldSize + 32768);
            auto const ret
              = jlink.rttRead(channel, std::span{buffer.data() + oldSize, buffer.size() - oldSize});
            buffer.resize(oldSize + ret.size());
        }

        bool run(
          std::stop_token&                                    stoken,
          JLink&                                              jlink,
          std::function<void(std::size_t, std::string_view)>& printF,
          std::uint32_t                                       channel,
          std::map<std::uint16_t, std::string>                stringConstantsMap,
          std::function<void(std::string_view)>               errorMessagef) {
            read(jlink, channel);
            bool gotMessage{};
            if(!buffer.empty()) {
                while(!stoken.stop_requested()) {
                    auto const [os, subrange, unparsed_bytes]
                      = remote_fmt::parse(buffer, stringConstantsMap, errorMessagef);
                    buffer.erase(
                      buffer.begin(),
                      std::next(
                        buffer.begin(),
                        static_cast<std::make_signed_t<std::size_t>>(
                          buffer.size() - subrange.size())));
                    if(unparsed_bytes != 0) {
                        errorMessagef(fmt::format(
                          "channel {} corrupted data removed {} byte{}",
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
                if(Clock::now() > lastValidRead + std::chrono::milliseconds{100} && !buffer.empty())
                {
                    buffer.erase(buffer.begin());
                    errorMessagef(fmt::format("channel {} timeout removed 1 byte", channel));
                }
            } else {
                lastValidRead = Clock::now();
            }
            return gotMessage;
        }
    };
    static void run(
      std::stop_token                                           stoken,
      std::string                                               host,
      std::string                                               device,
      std::uint32_t                                             speed,
      std::uint32_t                                             numChannels,
      std::atomic<JLink::Status>&                               status,
      std::atomic<bool>&                                        target_reset_flag,
      std::atomic<bool>&                                        jlink_reset_flag,
      std::atomic<bool>&                                        flash_flag,
      std::function<std::uint32_t(void)>                        blockAddressf,
      std::function<std::string(void)>                          hexFileNamef,
      std::function<std::map<std::uint16_t, std::string>(void)> catalogMapf,
      std::function<void(std::size_t, std::string_view)>        entryPrintf,
      std::function<void(std::string_view)>                     messagef,
      std::function<void(std::string_view)>                     errorMessagef) {
        while(!stoken.stop_requested()) {
            try {
                jlink_reset_flag = false;
                JLink jlink      = [&]() {
                    if(host.empty()) {
                        return JLink{device, speed};
                    }
                    return JLink{device, speed, host};
                }();

                bool restart = false;

                if(flash_flag) {
                    messagef("flashing target");
                    jlink.flash(hexFileNamef());
                    messagef("flashing target succeeded");
                    flash_flag        = false;
                    target_reset_flag = true;
                    restart           = true;
                }
                if(target_reset_flag) {
                    messagef("reseting target");
                    jlink.resetTarget();
                    target_reset_flag = false;
                    messagef("reseting target succeeded");
                    restart = true;
                }

                if(restart) {
                    continue;
                }

                auto const stringConstantsMap = catalogMapf();

                jlink.startRtt(numChannels, blockAddressf());
                std::vector<Channel> channels{};
                channels.resize(numChannels);
                auto lastMessage = Clock::now();

                while(!stoken.stop_requested() && !jlink_reset_flag && !target_reset_flag
                      && !flash_flag && !(Clock::now() > lastMessage + std::chrono::seconds{5}))
                {
                    for(std::size_t id{}; auto& channel : channels) {
                        if(channel.run(
                             stoken,
                             jlink,
                             entryPrintf,
                             id++,
                             stringConstantsMap,
                             errorMessagef))
                        {
                            lastMessage = Clock::now();
                        }
                    }
                    jlink.checkConnected();
                    JLink::Status local_status = jlink.readStatus();
                    status                     = local_status;
                    if(
                      local_status.isRunning == 0
                      || local_status.numUpBuffers != static_cast<int>(numChannels))
                    {
                        throw std::runtime_error("lost connection");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("catched {}", +e.what()));
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
            }
        }
    }

    std::atomic<JLink::Status> status{};
    std::atomic<bool>          target_reset_flag{};
    std::atomic<bool>          jlink_reset_flag{};
    std::atomic<bool>          flash_flag{};
    std::jthread               thread;

public:
    template<
      typename BlockAddressF,
      typename EntryPrintF,
      typename HexFileNameF,
      typename CatalogMapF,
      typename MessageF,
      typename ErrorMessageF>
    JLinkRttReader(
      std::string     host,
      std::string     device,
      std::uint32_t   speed,
      std::uint32_t   channels,
      BlockAddressF&& blockAddressf,
      HexFileNameF&&  hexFileNamef,
      CatalogMapF&&   catalogMapf,
      EntryPrintF&&   entryPrintf,
      MessageF&&      messagef,
      ErrorMessageF&& errorMessagef)
      : thread{
        &JLinkRttReader::run,
        host,
        device,
        speed,
        channels,
        std::ref(status),
        std::ref(target_reset_flag),
        std::ref(jlink_reset_flag),
        std::ref(flash_flag),
        std::forward<BlockAddressF>(blockAddressf),
        std::forward<HexFileNameF>(hexFileNamef),
        std::forward<CatalogMapF>(catalogMapf),
        std::forward<EntryPrintF>(entryPrintf),
        std::forward<MessageF>(messagef),
        std::forward<ErrorMessageF>(errorMessagef)} {}

    JLink::Status getStatus() { return status; }
    void          resetJLink() { jlink_reset_flag = true; }
    void          resetTarget() { target_reset_flag = true; }
    void          flash() { flash_flag = true; }
};
