#pragma once

#include "ComBackend.hpp"
#include "LogLevel.hpp"
#include "Tag.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace uc_log {

template<std::size_t... Sizes>
struct ChannelSizes {};

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         typename SizeConfig>
struct MultiChannelRttComBackend;

template<typename DebuggerPresentFunction,
         typename Router,
         rtt::BufferMode Mode,
         std::size_t... Sizes>
struct MultiChannelRttComBackend<DebuggerPresentFunction, Router, Mode, ChannelSizes<Sizes...>> {
private:
    static_assert(sizeof...(Sizes) == Router::NumLogicalChannels * 2,
                  "Provide thread + ISR buffer size for each logical channel");

    using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<Mode, Sizes...>;
    using RttType   = rtt::ControlBlock<RttConfig>;

    [[gnu::section(".noInit")]] static inline constinit typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
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
};
}   // namespace uc_log
