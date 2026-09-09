#pragma once

#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "MultiChannelRttComBackend.hpp"
#include "Tag.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uc_log {

// One RTT up buffer per (core, context) on a dual-core target: core 0 thread, core 0 ISR,
// core 1 thread, core 1 ISR, in that order. A log line is many small write() calls, and the
// only thing that keeps them from interleaving is that every ring has exactly one producer.
// The IPSR split gives that on one core; a second core needs its own pair, which is all this
// backend adds. No lock, no barrier, nothing on the hot path but one extra register read.
//
// The host printer treats every unpaired up buffer as a log channel and shows its index on
// each line, so core 1's lines arrive labelled 2 (thread) and 3 (ISR) with no host change.
//
// CoreIdFunction: a functor returning 0 or 1 (Kvasir::Sio::CpuId on the RP2350).
template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>>
struct MulticoreRttComBackend;

template<typename DebuggerPresentFunction,
         typename CoreIdFunction,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs>
struct MulticoreRttComBackend<DebuggerPresentFunction,
                              CoreIdFunction,
                              Mode,
                              ChannelSizes<Sizes...>,
                              DuplexChannelConfigs> {
private:
    static_assert(sizeof...(Sizes) == 4,
                  "four buffer sizes: core 0 thread, core 0 ISR, core 1 thread, core 1 ISR");

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

    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            auto get_IPSR = []() {
                std::uint32_t result{};
                asm("mrs %0, ipsr" : "=r"(result));
                return result;
            };
            std::uint32_t const core = CoreIdFunction{}() != 0 ? 2 : 0;
            std::uint32_t const isr  = get_IPSR() != 0 ? 1 : 0;
            switch(core + isr) {
            case 0:  rttControlBlock.template write<0>(span); break;
            case 1:  rttControlBlock.template write<1>(span); break;
            case 2:  rttControlBlock.template write<2>(span); break;
            default: rttControlBlock.template write<3>(span); break;
            }
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
