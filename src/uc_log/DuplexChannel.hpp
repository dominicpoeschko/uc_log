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

template<ChannelName     Name_,
         std::size_t     UpSize_,
         std::size_t     DownSize_ = UpSize_,
         rtt::BufferMode UpMode_   = rtt::BufferMode::trim,
         rtt::BufferMode DownMode_ = rtt::BufferMode::trim>
struct DuplexChannelConfig {
    static constexpr auto            Name     = Name_;
    static constexpr std::size_t     UpSize   = UpSize_;
    static constexpr std::size_t     DownSize = DownSize_;
    static constexpr rtt::BufferMode UpMode   = UpMode_;
    static constexpr rtt::BufferMode DownMode = DownMode_;
};

template<typename... Configs>
struct DuplexChannels {};

}   // namespace uc_log
