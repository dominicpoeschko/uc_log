#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "LossPolicy.hpp"
#include "MultiChannelRttComBackend.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttBackendCore.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <span>

namespace uc_log {

// One group of RTT up buffers per core on a dual-core target: core 0's thread ring and its
// ISR rings, then core 1's (with the default policy: core 0 thread, core 0 ISR, core 1
// thread, core 1 ISR). A log line is many small write() calls, which must not interleave on
// a ring: the IPSR split separates the thread from the exceptions, the policy (IsrPolicy.hpp)
// keeps the exceptions apart, and a second core gets its own group, which is all this backend
// adds. No lock, no barrier, nothing on the hot path but one extra register read.
//
// The host printer treats every unpaired up buffer as a log channel and shows its index on
// each line, so core 1's lines arrive labelled with the indexes after core 0's group (2 for
// the thread and 3 for the ISR with the default policy) with no host change.
//
// CoreIdFunction: a functor returning 0 or 1 (Kvasir::Sio::CpuId on the RP2350).
// Loss: what a dropped record leaves behind (LossPolicy.hpp); each core's rings have their own
// ledger slots, written by that core only.
template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>,
         LossPolicy::Policy Loss       = LossPolicy::Silent>
struct MulticoreRttComBackend;

template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs,
         typename Policy,
         LossPolicy::Policy Loss>
struct MulticoreRttComBackend<DebuggerPresentFunction,
                              CoreIdFunction,
                              Mode,
                              ChannelSizes<Sizes...>,
                              DuplexChannelConfigs,
                              Policy,
                              Loss>
  : Policy
  , detail::RttBackendCore<MulticoreRttComBackend<DebuggerPresentFunction,
                                                  CoreIdFunction,
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
    friend typename MulticoreRttComBackend::RttBackendCore;

    static constexpr std::size_t RingsPerCore = 1 + Policy::NumIsrRings;

    static_assert(sizeof...(Sizes) == 2 * RingsPerCore,
                  "per core a thread buffer size and one ISR buffer size per ISR ring "
                  "(IsrPolicy::NumIsrRings): core 0's group, then core 1's");

    // the policy's ring in the calling core's group
    static std::size_t ring() {
        return detail::logRing<Policy>(CoreIdFunction{}() != 0 ? RingsPerCore : 0);
    }

public:
    // Each core writes to its own group of rings, so an ISR on one core never preempts a record
    // on the other's: Startup holds the policy's contract to each core's interrupts on their
    // own (ListRules::IsrContractHolds).
    static constexpr bool perCoreIsrContexts = true;
};
}   // namespace uc_log
