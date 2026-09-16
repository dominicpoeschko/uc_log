#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "MultiChannelRttComBackend.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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
template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>>
struct MulticoreRttComBackend;

template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs,
         typename Policy>
struct MulticoreRttComBackend<DebuggerPresentFunction,
                              CoreIdFunction,
                              Mode,
                              ChannelSizes<Sizes...>,
                              DuplexChannelConfigs,
                              Policy> : Policy {
private:
    static constexpr std::size_t RingsPerCore = 1 + Policy::NumIsrRings;

    static_assert(sizeof...(Sizes) == 2 * RingsPerCore,
                  "per core a thread buffer size and one ISR buffer size per ISR ring "
                  "(IsrPolicy::NumIsrRings): core 0's group, then core 1's");

    using ConfigBuilder = detail::RttConfigBuilder<Mode, DuplexChannelConfigs, Sizes...>;
    using RttConfig     = typename ConfigBuilder::Config;
    using RttType       = rtt::ControlBlock<RttConfig>;

    // gnu::used: gcc LTO otherwise drops the section attribute and the buffer lands in .bss
    [[gnu::section(".noInit"), gnu::used]] static inline constinit
      typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
    // Listed in an application's Startup by convention (the log transport belongs next to
    // the peripherals), with nothing for Startup to run: this is what tells Startup's
    // "every entry is a peripheral" rule that the listing is deliberate.
    static constexpr bool isStartupEntry = true;

    // Each core writes to its own group of rings, so an ISR on one core never preempts a record
    // on the other's: Startup holds the policy's contract to each core's interrupts on their
    // own (ListRules::IsrContractHolds).
    static constexpr bool perCoreIsrContexts = true;

    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            std::size_t const base = CoreIdFunction{}() != 0 ? RingsPerCore : 0;
            std::size_t const ring = detail::logRing<Policy>(base);
            if(ring == detail::noLogRing) { return; }
            detail::writeLogRing<ConfigBuilder::NumLogUpBuffers>(rttControlBlock, ring, span);
        }
    }

    // a duplex channel is single-producer/single-consumer: concurrent ISR + thread access to the
    // same channel, or access from both cores, is the application's responsibility
    template<std::size_t I>
    struct DuplexChannel {
        static_assert(I < ConfigBuilder::NumDuplexChannels,
                      "duplex channel index out of range");

        static constexpr std::string_view name
          = detail::DuplexBufferName<ConfigBuilder::template DuplexConfigAt<I>::Name>{};

        // host -> uc, returns the bytes read
        static std::span<std::byte> read(std::span<std::byte> buffer) {
            return rttControlBlock.template read<I>(buffer);
        }

        // uc -> host, returns the unwritten remainder
        static std::span<std::byte const> write(std::span<std::byte const> buffer) {
            if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
                return rttControlBlock.template write<ConfigBuilder::NumLogUpBuffers + I>(buffer);
            }
            return buffer;
        }
    };
};
}   // namespace uc_log
