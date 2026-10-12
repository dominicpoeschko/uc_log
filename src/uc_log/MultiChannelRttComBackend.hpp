#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "LogLevel.hpp"
#include "LossPolicy.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttBackendCore.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <span>

namespace uc_log {

template<std::size_t... Sizes>
struct ChannelSizes {};

// Per logical channel (Router) one thread ring, then Policy::NumIsrRings ISR rings
// (IsrPolicy.hpp). Loss: what a dropped record leaves behind (LossPolicy.hpp).
template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>,
         LossPolicy::Policy Loss       = LossPolicy::Silent>
struct MultiChannelRttComBackend;

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs,
         typename Policy,
         LossPolicy::Policy Loss>
struct MultiChannelRttComBackend<DebuggerPresentFunction,
                                 Router,
                                 Mode,
                                 ChannelSizes<Sizes...>,
                                 DuplexChannelConfigs,
                                 Policy,
                                 Loss>
  : Policy
  , detail::RttBackendCore<MultiChannelRttComBackend<DebuggerPresentFunction,
                                                     Router,
                                                     Mode,
                                                     ChannelSizes<Sizes...>,
                                                     DuplexChannelConfigs,
                                                     Policy,
                                                     Loss>,
                           DebuggerPresentFunction,
                           Mode,
                           detail::RttConfigBuilder<Mode, DuplexChannelConfigs, Sizes...>,
                           Loss> {
private:
    using Core = typename MultiChannelRttComBackend::RttBackendCore;

    static constexpr std::size_t RingsPerChannel = 1 + Policy::NumIsrRings;

    static_assert(sizeof...(Sizes) == Router::NumLogicalChannels * RingsPerChannel,
                  "Provide a thread buffer size and one ISR buffer size per ISR ring "
                  "(IsrPolicy::NumIsrRings) for each logical channel");

    // the policy's ring in the level's logical channel
    template<LogLevel Level>
    static std::size_t ringFor() {
        return detail::logRing<Policy>(Router::template logicalChannel<Level> * RingsPerChannel);
    }

    template<LogLevel Level>
    struct RingFor {
        static std::size_t ring() { return ringFor<Level>(); }
    };

public:
    // the level picks the ring: these hide the core's level-blind ones, and LevelBoundBackend passes the level
    template<LogLevel Level>
    static void write(std::span<std::byte const> span) {
        Core::template writeOn<RingFor<Level>>(span);
    }

    template<LogLevel Level>
    static void initTransfer()
        requires Core::counted
    {
        Core::beginOn(ringFor<Level>());
    }

    template<LogLevel Level>
    static void finalizeTransfer()
        requires Core::counted
    {
        Core::endOn(ringFor<Level>());
    }

    template<LogLevel Level>
    static auto lossMark()
        requires Core::counted
    {
        return Core::markOn(ringFor<Level>());
    }
};
}   // namespace uc_log
