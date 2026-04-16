#pragma once

#include "LogLevel.hpp"

#include <cstddef>

namespace uc_log {

struct SingleChannelRouter {
    static constexpr std::size_t NumLogicalChannels = 1;

    template<LogLevel Level>
    static constexpr std::size_t logicalChannel = 0;
};

template<LogLevel... HighPriorityLevels>
struct LevelSplitRouter {
    static constexpr std::size_t NumLogicalChannels = 2;

    template<LogLevel Level>
    static constexpr std::size_t logicalChannel = ((Level == HighPriorityLevels) || ...) ? 1 : 0;
};

}   // namespace uc_log
