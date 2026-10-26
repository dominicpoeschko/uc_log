#pragma once

#include <cstdint>

namespace uc_log::printer {

struct RttBlockInfo {
    std::uint32_t address{};
    std::uint32_t totalBuffers{};   // up + down, the map file cannot tell them apart
};

}   // namespace uc_log::printer
