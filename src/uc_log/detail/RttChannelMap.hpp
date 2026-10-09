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

    struct LogChannelDesc {
        std::uint32_t upIndex{};
        std::uint32_t channel{};   // N of its name uc_logN, else upIndex
    };

    struct RttChannelMap {
        std::vector<LogChannelDesc>    logChannels;
        std::vector<DuplexChannelDesc> duplexChannels;
    };

    // N of a buffer name "uc_logN"
    inline std::optional<std::uint32_t> logChannelNumber(std::string_view name) {
        constexpr std::string_view prefix{"uc_log"};
        if(!name.starts_with(prefix) || name.size() == prefix.size()
           || name.size() > prefix.size() + 9)
        {
            return std::nullopt;
        }
        std::uint32_t n{};
        for(char const c : name.substr(prefix.size())) {
            if(c < '0' || c > '9') { return std::nullopt; }
            n = n * 10 + static_cast<std::uint32_t>(c - '0');
        }
        return n;
    }

    // Numbers the log channels by their uc_logN names if all are distinct, else keeps positions.
    template<typename BufferDescOpt>
    void numberLogChannels(RttChannelMap&                               map,
                           std::vector<BufferDescOpt> const&            upDescs,
                           std::function<void(std::string_view)> const& errorMessagef) {
        std::vector<std::uint32_t> numbers;
        for(auto const& log : map.logChannels) {
            auto const n = log.upIndex < upDescs.size() && upDescs[log.upIndex]
                           ? logChannelNumber(upDescs[log.upIndex]->name)
                           : std::nullopt;
            if(!n || std::ranges::find(numbers, *n) != numbers.end()) { return; }
            numbers.push_back(*n);
        }
        bool reordered = false;
        for(std::size_t i{}; i < map.logChannels.size(); ++i) {
            map.logChannels[i].channel = numbers[i];
            reordered                  = reordered || numbers[i] != map.logChannels[i].upIndex;
        }
        std::ranges::sort(map.logChannels, {}, &LogChannelDesc::channel);
        if(reordered) {
            std::string order;
            for(auto const& log : map.logChannels) {
                order += fmt::format("{}uc_log{} in buffer {}",
                                     order.empty() ? "" : ", ",
                                     log.channel,
                                     log.upIndex);
            }
            errorMessagef(fmt::format(
              "the target's RTT log buffers are not in channel order ({}): logged by their names. "
              "Its rtt.hpp is an old one that was built with libstdc++ - rebuild it",
              order));
        }
    }

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

        using BufferDescOpt = decltype(jlink.rttBufferDesc(false, std::uint32_t{}));
        std::vector<BufferDescOpt> upDescs;
        std::vector<BufferDescOpt> downDescs;
        for(std::uint32_t i{}; i < numUp; ++i) { upDescs.push_back(jlink.rttBufferDesc(false, i)); }

        if(numDown == 0) {
            for(std::uint32_t i{}; i < numUp; ++i) { map.logChannels.push_back({i, i}); }
            numberLogChannels(map, upDescs, errorMessagef);
            return map;
        }

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
                if(!upPaired[u]) { map.logChannels.push_back({u, u}); }
            }
            // order duplex channels by up index so ordinals match the firmware declaration order
            std::ranges::sort(map.duplexChannels, [](auto const& a, auto const& b) {
                return a.upIndex.value_or(0xFFFFFFFFU) < b.upIndex.value_or(0xFFFFFFFFU);
            });
            for(std::size_t ordinal{}; auto& dc : map.duplexChannels) { dc.ordinal = ordinal++; }
            numberLogChannels(map, upDescs, errorMessagef);
            messagef(fmt::format("rtt channel map from buffer names: {} log, {} duplex",
                                 map.logChannels.size(),
                                 map.duplexChannels.size()));
        } else {
            // old DLL without getDesc or unnamed buffers: pair by position from the end, which is
            // exactly the layout the firmware emits (log buffers first, duplex pairs appended)
            auto const numLog = numUp > numDown ? numUp - numDown : 0U;
            for(std::uint32_t i{}; i < numLog; ++i) { map.logChannels.push_back({i, i}); }
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
