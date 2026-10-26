#pragma once

#include "rtt/rtt.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace uc_log {

template<std::size_t N>
struct ChannelName {
    std::array<char, N> value{};

    consteval ChannelName(char const (&s)[N]) {
        for(std::size_t i{}; i < N; ++i) { value[i] = s[i]; }
    }

    constexpr operator std::string_view() const { return std::string_view{value.data(), N - 1}; }
};

template<std::size_t N>
ChannelName(char const (&)[N]) -> ChannelName<N>;

template<ChannelName     Name,
         std::size_t     UpSize,
         std::size_t     DownSize = UpSize,
         rtt::BufferMode UpMode   = rtt::BufferMode::trim,
         rtt::BufferMode DownMode = rtt::BufferMode::trim>
struct DuplexChannelConfig {
    static constexpr auto            name     = Name;
    static constexpr std::size_t     upSize   = UpSize;
    static constexpr std::size_t     downSize = DownSize;
    static constexpr rtt::BufferMode upMode   = UpMode;
    static constexpr rtt::BufferMode downMode = DownMode;
};

template<typename... Configs>
struct DuplexChannels {};

}   // namespace uc_log
