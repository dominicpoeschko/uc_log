#pragma once

#include "ComBackend.hpp"
#include "DuplexChannel.hpp"
#include "Tag.hpp"
#include "detail/RttConfigBuilder.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uc_log {

template<typename DebuggerPresentFunction,
         std::size_t     MainBufferSize,
         std::size_t     IsrBufferSize = MainBufferSize,
         rtt::BufferMode Mode          = rtt::BufferMode::block,
         typename DuplexChannelConfigs = DuplexChannels<>>
struct DefaultRttComBackend {
private:
    using ConfigBuilder
      = detail::RttConfigBuilder<Mode, DuplexChannelConfigs, MainBufferSize, IsrBufferSize>;
    using RttConfig = typename ConfigBuilder::Config;
    using RttType   = rtt::ControlBlock<RttConfig>;

    [[gnu::section(".noInit")]] static inline constinit typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    static void write(std::span<std::byte const> span) {
        if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
            auto get_IPSR = []() {
                std::uint32_t result{};
                asm("mrs %0, ipsr" : "=r"(result));
                return result;
            };
            if(get_IPSR() == 0) {
                rttControlBlock.template write<0>(span);
            } else {
                rttControlBlock.template write<1>(span);
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
