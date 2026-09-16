#pragma once

#include "AlwaysAttached.hpp"
#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "IsrPolicy.hpp"
#include "Tag.hpp"
#include "detail/LogRing.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace uc_log {

// One thread ring of MainBufferSize, then Policy::NumIsrRings ISR rings of IsrBufferSize
// each (IsrPolicy.hpp).
template<typename DebuggerPresentFunction,
         std::size_t     MainBufferSize,
         std::size_t     IsrBufferSize = MainBufferSize,
         rtt::BufferMode Mode          = rtt::BufferMode::block,
         typename DuplexChannelConfigs = DuplexChannels<>,
         typename Policy               = IsrPolicy::SingleLevel<>>
struct DefaultRttComBackend : Policy {
private:
    template<std::size_t... Is>
    static auto makeConfigBuilder(std::index_sequence<Is...>)
      -> detail::RttConfigBuilder<Mode,
                                  DuplexChannelConfigs,
                                  MainBufferSize,
                                  detail::repeatSize<IsrBufferSize,
                                                     Is>...>;

    using ConfigBuilder
      = decltype(makeConfigBuilder(std::make_index_sequence<Policy::NumIsrRings>{}));
    using RttConfig = typename ConfigBuilder::Config;
    using RttType   = rtt::ControlBlock<RttConfig>;

    // gnu::used: gcc LTO otherwise drops the section attribute and the buffer lands in .bss
    [[gnu::section(".noInit"), gnu::used]] static inline constinit
      typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
    // Listed in an application's Startup by convention (the log transport belongs next to
    // the peripherals), with nothing for Startup to run: this is what tells Startup's
    // "every entry is a peripheral" rule that the listing is deliberate. Startup also checks
    // the policy's contract members against the enabled interrupts.
    static constexpr bool isStartupEntry = true;

    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            std::size_t const ring = detail::logRing<Policy>(0);
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
