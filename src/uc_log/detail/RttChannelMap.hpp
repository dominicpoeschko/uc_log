#pragma once

#include "remote_fmt/fmt_wrapper.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log { namespace detail {

    struct DuplexChannelDesc {
        std::size_t                  ordinal{};
        std::string                  name;
        std::optional<std::uint32_t> upIndex;   // nullopt: host -> target only
        std::uint32_t                downIndex{};
    };

    struct RttChannelMap {
        std::vector<std::uint32_t>     logChannels;
        std::vector<DuplexChannelDesc> duplexChannels;
    };

    // callbacks the reader loop uses to bridge duplex channels to the TCP servers, all optional
    struct DuplexBridge {
        std::function<void(std::vector<DuplexChannelDesc> const&)>    configure;
        std::function<void(std::size_t, std::span<std::byte const>)>  sendToClient;
        std::function<std::size_t(std::size_t, std::span<std::byte>)> peekFromClient;
        std::function<void(std::size_t, std::size_t)>                 consumeFromClient;
    };

    // pairs up and down buffers carrying the same non-empty name into duplex channels, every
    // unpaired up buffer stays a log channel. Templated on the transport so the logic is
    // testable without the JLink DLL.
    template<typename TransportT>
    RttChannelMap buildRttChannelMap(TransportT&                                  jlink,
                                     typename TransportT::Status const&           status,
                                     std::function<void(std::string_view)> const& messagef,
                                     std::function<void(std::string_view)> const& errorMessagef) {
        RttChannelMap map{};

        auto const numUp   = static_cast<std::uint32_t>(std::max(status.numUpBuffers, 0));
        auto const numDown = static_cast<std::uint32_t>(std::max(status.numDownBuffers, 0));

        if(numDown == 0) {
            for(std::uint32_t i{}; i < numUp; ++i) { map.logChannels.push_back(i); }
            return map;
        }

        using BufferDescOpt = decltype(jlink.rttBufferDesc(false, std::uint32_t{}));
        std::vector<BufferDescOpt> upDescs;
        std::vector<BufferDescOpt> downDescs;
        for(std::uint32_t i{}; i < numUp; ++i) { upDescs.push_back(jlink.rttBufferDesc(false, i)); }
        for(std::uint32_t i{}; i < numDown; ++i) {
            downDescs.push_back(jlink.rttBufferDesc(true, i));
        }

        bool const descsUsable
          = std::ranges::all_of(downDescs, [](auto const& d) { return d && !d->name.empty(); })
         && std::ranges::all_of(upDescs, [](auto const& d) { return d.has_value(); });

        if(descsUsable) {
            std::vector<bool> upPaired(numUp, false);
            for(std::uint32_t downIndex{}; downIndex < numDown; ++downIndex) {
                auto const& downName = downDescs[downIndex]->name;

                std::optional<std::uint32_t> upIndex;
                for(std::uint32_t u{}; u < numUp; ++u) {
                    if(!upPaired[u] && upDescs[u]->name == downName) {
                        upIndex     = u;
                        upPaired[u] = true;
                        break;
                    }
                }
                if(!upIndex) {
                    errorMessagef(
                      fmt::format("duplex channel \"{}\" has no matching up buffer, "
                                  "host to target only",
                                  downName));
                }
                map.duplexChannels.push_back(
                  DuplexChannelDesc{map.duplexChannels.size(), downName, upIndex, downIndex});
            }
            for(std::uint32_t u{}; u < numUp; ++u) {
                if(!upPaired[u]) { map.logChannels.push_back(u); }
            }
            // order duplex channels by up index so ordinals match the firmware declaration order
            std::ranges::sort(map.duplexChannels, [](auto const& a, auto const& b) {
                return a.upIndex.value_or(0xFFFFFFFFU) < b.upIndex.value_or(0xFFFFFFFFU);
            });
            for(std::size_t ordinal{}; auto& dc : map.duplexChannels) { dc.ordinal = ordinal++; }
            messagef(fmt::format("rtt channel map from buffer names: {} log, {} duplex",
                                 map.logChannels.size(),
                                 map.duplexChannels.size()));
        } else {
            // old DLL without getDesc or unnamed buffers: pair by position from the end, which is
            // exactly the layout the firmware emits (log buffers first, duplex pairs appended)
            auto const numLog = numUp > numDown ? numUp - numDown : 0U;
            for(std::uint32_t i{}; i < numLog; ++i) { map.logChannels.push_back(i); }
            for(std::uint32_t i{}; i < numDown; ++i) {
                map.duplexChannels.push_back(
                  DuplexChannelDesc{i,
                                    fmt::format("duplex{}", i),
                                    numLog + i < numUp ? std::optional{numLog + i} : std::nullopt,
                                    i});
            }
            messagef(
              fmt::format("rtt buffer names unavailable, positional channel map: "
                          "{} log, {} duplex",
                          map.logChannels.size(),
                          map.duplexChannels.size()));
        }
        return map;
    }

}}   // namespace uc_log::detail
