#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "LogLevel.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uc_log {

template<std::size_t... Sizes>
struct ChannelSizes {};

// Per logical channel (Router) one thread ring, then Policy::NumIsrRings ISR rings
// (IsrPolicy.hpp).
template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>>
struct MultiChannelRttComBackend;

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs,
         typename Policy>
struct MultiChannelRttComBackend<DebuggerPresentFunction,
                                 Router,
                                 Mode,
                                 ChannelSizes<Sizes...>,
                                 DuplexChannelConfigs,
                                 Policy> : Policy {
private:
    static constexpr std::size_t RingsPerChannel = 1 + Policy::NumIsrRings;

    static_assert(sizeof...(Sizes) == Router::NumLogicalChannels * RingsPerChannel,
                  "Provide a thread buffer size and one ISR buffer size per ISR ring "
                  "(IsrPolicy::NumIsrRings) for each logical channel");

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

    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    template<LogLevel Level>
    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            constexpr std::size_t base = Router::template logicalChannel<Level> * RingsPerChannel;
            std::size_t const     ring = detail::logRing<Policy>(base);
            if(ring == detail::noLogRing) { return; }
            detail::writeLogRing<ConfigBuilder::NumLogUpBuffers>(rttControlBlock, ring, span);
        }
    }

    // a duplex channel is single-producer/single-consumer: concurrent ISR + thread access to the
    // same channel is the application's responsibility
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
