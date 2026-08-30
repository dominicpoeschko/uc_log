#pragma once

#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "LogLevel.hpp"
#include "Tag.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uc_log {

template<std::size_t... Sizes>
struct ChannelSizes {};

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         typename SizeConfig,
         typename DuplexChannelConfigs = DuplexChannels<>>
struct MultiChannelRttComBackend;

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         std::size_t... Sizes,
         typename DuplexChannelConfigs>
struct MultiChannelRttComBackend<DebuggerPresentFunction,
                                 Router,
                                 Mode,
                                 ChannelSizes<Sizes...>,
                                 DuplexChannelConfigs> {
private:
    static_assert(sizeof...(Sizes) == Router::NumLogicalChannels * 2,
                  "Provide thread + ISR buffer size for each logical channel");

    using ConfigBuilder = detail::RttConfigBuilder<Mode, DuplexChannelConfigs, Sizes...>;
    using RttConfig     = typename ConfigBuilder::Config;
    using RttType       = rtt::ControlBlock<RttConfig>;

    // gnu::used: gcc LTO otherwise drops the section attribute and the buffer lands in .bss
    [[gnu::section(".noInit"), gnu::used]] static inline constinit
      typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    template<LogLevel Level>
    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            constexpr std::size_t threadBuf = Router::template logicalChannel<Level> * 2;
            constexpr std::size_t isrBuf    = threadBuf + 1;

            auto get_IPSR = []() {
                std::uint32_t result{};
                asm("mrs %0, ipsr" : "=r"(result));
                return result;
            };
            if(get_IPSR() == 0) {
                rttControlBlock.template write<threadBuf>(span);
            } else {
                rttControlBlock.template write<isrBuf>(span);
            }
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
