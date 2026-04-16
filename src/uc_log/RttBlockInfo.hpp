#pragma once

#include <cstdint>

struct RttBlockInfo {
    std::uint32_t address{};
    std::uint32_t numUpBuffers{};
};
