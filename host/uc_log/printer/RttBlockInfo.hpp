#pragma once

#include <cstdint>

namespace uc_log::printer {

struct RttBlockInfo {
    std::uint32_t address{};
    std::uint32_t totalBuffers{};   // up + down, the map file cannot tell them apart
    // the map names no control block at all: an image without RTT (a `release` build); a session then reads no ring
    // and serves only the target requests (reads, writes, halts) - no RAM search finds the block a previous image left in RAM
    bool none{};
};

}   // namespace uc_log::printer
