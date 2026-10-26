#pragma once

// The one RTT log backend. Its rings are laid out in groups of 1 + Policy::numIsrRings (a thread ring, then one ring
// per ISR ring of the policy, IsrPolicy.hpp), one group per core and logical channel (RingLayout): core 0's channels,
// then core 1's. A log line is many small write() calls, which must not interleave on a ring: the IPSR split separates
// the thread from the exceptions, the policy keeps the exceptions apart, a second core gets its own groups, and a level
// router gives a level its own channel. No lock, no barrier, nothing on the hot path but the register reads.
//
// The host printer treats every unpaired up buffer as a log channel and shows its index on each line. Applications
// spell it through RttLog (RttLog.hpp); this is the type it resolves to.
//
// One class body for every layout, no macros: the ring choice is one group computation (groupOf), the level hooks and
// perCoreIsrContexts come from bases that exist only in the layouts that have them (detail::LevelRings,
// detail::PerCoreRings). The attributes are part of the code it makes: only the one-core, one-channel ring() is
// always_inline (see there).

#include "AlwaysAttached.hpp"
#include "ChannelRouter.hpp"
#include "ComBackend.hpp"
#include "CortexM.hpp"
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

// The ring sizes in buffer order: per group a thread size, then one size per ISR ring.
template<std::size_t... Sizes>
struct ChannelSizes {};

// Contexts: where a write runs and on which core - SingleCore or TwoCores<CoreId> (CortexM.hpp; each core's rings
// have their own ledger slots, written by that core only), a fake in host tests; Router: SingleChannelRouter or a level
// router (ChannelRouter.hpp).
template<typename ContextsT = SingleCore, typename RouterT = SingleChannelRouter>
struct RingLayout {
    static constexpr std::size_t cores    = ContextsT::cores;
    static constexpr std::size_t channels = RouterT::numLogicalChannels;
    using Contexts                        = ContextsT;
    using Router                          = RouterT;
};

namespace detail {
    template<typename Self,
             typename DebuggerPresentFunction,
             rtt::BufferMode Mode,
             typename Duplex,
             typename Loss,
             typename Sizes>
    struct RttCoreOf;

    template<typename Self,
             typename DebuggerPresentFunction,
             rtt::BufferMode Mode,
             typename Duplex,
             typename Loss,
             std::size_t... Sizes>
    struct RttCoreOf<Self, DebuggerPresentFunction, Mode, Duplex, Loss, ChannelSizes<Sizes...>> {
        using type = RttBackendCore<Self,
                                    DebuggerPresentFunction,
                                    Mode,
                                    RttConfigBuilder<Mode, Duplex, Sizes...>,
                                    Loss>;
        static constexpr std::size_t count = sizeof...(Sizes);
    };

    // The level-templated hooks of a layout whose ring depends on the level (a channel per level group). They come
    // from a base between the core and the backend, chosen by ByLevel, never from the backend itself: a derived
    // `template<LogLevel> write` hides the core's level-blind write(span) whatever its `requires` says (name hiding
    // ignores constraints). LevelBoundBackend passes the level; RingFor<Level>::ring() names the ring the backend's
    // ringFor<Level>() picks.
    template<typename Core, typename Backend, bool ByLevel>
    struct LevelRings : Core {};

    template<typename Core, typename Backend>
    struct LevelRings<Core, Backend, true> : Core {
        template<LogLevel Level>
        struct RingFor {
            static std::size_t ring() { return Backend::template ringFor<Level>(); }
        };

        template<LogLevel Level>
        static void write(std::span<std::byte const> span) {
            Core::template writeOn<RingFor<Level>>(span);
        }

        template<LogLevel Level>
        static void initTransfer()
            requires Core::counted
        {
            Core::beginOn(RingFor<Level>::ring());
        }

        template<LogLevel Level>
        static void finalizeTransfer()
            requires Core::counted
        {
            Core::endOn(RingFor<Level>::ring());
        }

        template<LogLevel Level>
        static auto lossMark()
            requires Core::counted
        {
            return Core::markOn(RingFor<Level>::ring());
        }
    };

    // perCoreIsrContexts, read by its presence (FanOutBackend, Kvasir Startup's ListRules): only on two cores. Each
    // core writes to its own groups of rings, so an ISR on one core never preempts a record on the other's: Startup
    // holds the policy's contract to each core's interrupts on their own (ListRules::IsrContractHolds).
    template<std::size_t Cores>
    struct PerCoreRings {};

    template<>
    struct PerCoreRings<2> {
        static constexpr bool perCoreIsrContexts = true;
    };
}   // namespace detail

// Loss: what a dropped record leaves behind (LossPolicy.hpp). Every parameter is spelled: the defaults are RttLog's
// (RttLog.hpp), which names this type.
template<typename DebuggerPresentFunction,
         typename Layout,
         typename Sizes,
         rtt::BufferMode Mode,
         typename Duplex,
         typename Policy,
         LossPolicy::Policy Loss>
struct RttComBackend
  : Policy
  , detail::LevelRings<
      typename detail::RttCoreOf<
        RttComBackend<DebuggerPresentFunction, Layout, Sizes, Mode, Duplex, Policy, Loss>,
        DebuggerPresentFunction,
        Mode,
        Duplex,
        Loss,
        Sizes>::type,
      RttComBackend<DebuggerPresentFunction, Layout, Sizes, Mode, Duplex, Policy, Loss>,
      (Layout::channels > 1)>
  , detail::PerCoreRings<Layout::cores> {
    static constexpr std::size_t ringsPerGroup = 1 + Policy::numIsrRings;
    static_assert(
      detail::RttCoreOf<RttComBackend,
                        DebuggerPresentFunction,
                        Mode,
                        Duplex,
                        Loss,
                        Sizes>::count
        == Layout::cores * Layout::channels * ringsPerGroup,
      "per core and logical channel a thread buffer size and one ISR buffer size per ISR ring "
      "(IsrPolicy::numIsrRings): core 0's channels, then core 1's");

    // The ring of this write, one channel: the policy's ring (detail::logRing) in the calling core's group. Two
    // declarations for the attribute: one core's is always_inline, two cores' is not - always_inline there made every
    // two-core log image 48-72 bytes bigger.
    [[gnu::always_inline]] static std::size_t ring()
        requires(Layout::cores == 1 && Layout::channels == 1)
    {
        return detail::logRing<Policy, Ctx>(groupOf<0>());
    }

    static std::size_t ring()
        requires(Layout::cores == 2 && Layout::channels == 1)
    {
        return detail::logRing<Policy, Ctx>(groupOf<0>());
    }

    // A channel per level group (Router): the policy's ring in the level's logical channel of the calling core's
    // groups (detail::LevelRings asks it).
    template<LogLevel Level>
    static std::size_t ringFor()
        requires(Layout::channels > 1)
    {
        return detail::logRing<Policy, Ctx>(
          groupOf<Layout::Router::template logicalChannel<Level>>());
    }

private:
    using Ctx = typename Layout::Contexts;

    // the first ring of logical channel Channel in the calling core's groups: core 0's channels, then core 1's
    template<std::size_t Channel>
    [[gnu::always_inline]] static std::size_t groupOf() {
        if constexpr(Layout::cores == 1) {
            return Channel * ringsPerGroup;
        } else {
            return (Ctx::core() != 0 ? Layout::channels : 0) * ringsPerGroup
                 + Channel * ringsPerGroup;
        }
    }
};

}   // namespace uc_log
