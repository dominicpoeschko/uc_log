#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "LossPolicy.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttBackendCore.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <utility>

namespace uc_log {
namespace detail {
    // one ring of MainSize, then NumIsrRings of IsrSize
    template<rtt::BufferMode Mode,
             typename DuplexChannelConfigs,
             std::size_t MainSize,
             std::size_t IsrSize,
             std::size_t... Is>
    auto defaultConfigBuilder(std::index_sequence<Is...>) -> RttConfigBuilder<Mode,
                                                                              DuplexChannelConfigs,
                                                                              MainSize,
                                                                              repeatSize<IsrSize,
                                                                                         Is>...>;

    template<rtt::BufferMode Mode,
             typename DuplexChannelConfigs,
             std::size_t MainSize,
             std::size_t IsrSize,
             std::size_t NumIsrRings>
    using DefaultConfigBuilder
      = decltype(defaultConfigBuilder<Mode, DuplexChannelConfigs, MainSize, IsrSize>(
        std::make_index_sequence<NumIsrRings>{}));
}   // namespace detail

// One thread ring of MainBufferSize, then Policy::NumIsrRings ISR rings of IsrBufferSize
// each (IsrPolicy.hpp). Loss: what a dropped record leaves behind (LossPolicy.hpp).
template<typename DebuggerPresentFunction,
         std::size_t     MainBufferSize,
         std::size_t     IsrBufferSize = MainBufferSize,
         rtt::BufferMode Mode          = rtt::BufferMode::block,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>,
         LossPolicy::Policy Loss       = LossPolicy::Silent>
struct DefaultRttComBackend
  : Policy
  , detail::RttBackendCore<DefaultRttComBackend<DebuggerPresentFunction,
                                                MainBufferSize,
                                                IsrBufferSize,
                                                Mode,
                                                DuplexChannelConfigs,
                                                Policy,
                                                Loss>,
                           DebuggerPresentFunction,
                           Mode,
                           detail::DefaultConfigBuilder<Mode,
                                                        DuplexChannelConfigs,
                                                        MainBufferSize,
                                                        IsrBufferSize,
                                                        Policy::NumIsrRings>,
                           Loss> {
private:
    friend typename DefaultRttComBackend::RttBackendCore;

    // the thread ring or the policy's ISR ring of the one group
    [[gnu::always_inline]] static std::size_t ring() { return detail::logRing<Policy>(0); }
};
}   // namespace uc_log
