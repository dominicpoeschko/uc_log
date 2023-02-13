#pragma once

#include "ComBackend.hpp"
#include "Tag.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace uc_log {

template<
  typename DebuggerPresentFunction,
  std::size_t     MainBufferSize,
  std::size_t     IsrBufferSize = MainBufferSize,
  rtt::BufferMode Mode          = rtt::BufferMode::block>
struct DefaultRttComBackend {
private:
    using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<Mode, MainBufferSize, IsrBufferSize>;
    using RttType   = rtt::ControlBlock<RttConfig>;

    [[gnu::section(".noInit")]] static inline constinit typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

public:
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
};
}   // namespace uc_log
