#pragma once

#include "LogLevel.hpp"

#include <cstddef>

namespace uc_log {

// Every level on one logical channel (RingLayout's default, RttComBackend.hpp).
struct SingleChannelRouter {
    static constexpr std::size_t numLogicalChannels = 1;

    template<LogLevel Level>
    static constexpr std::size_t logicalChannel = 0;
};

// The levels listed on logical channel 1, every other level on channel 0.
template<LogLevel... HighPriorityLevels>
struct LevelSplitRouter {
    static constexpr std::size_t numLogicalChannels = 2;

    template<LogLevel Level>
    static constexpr std::size_t logicalChannel = ((Level == HighPriorityLevels) || ...) ? 1 : 0;
};

}   // namespace uc_log
