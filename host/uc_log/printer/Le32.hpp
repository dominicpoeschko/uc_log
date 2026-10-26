#pragma once

// Words in bytes read from the target. Every target the printer reads is a little-endian Cortex-M.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <vector>

namespace uc_log::printer {

/// The little-endian word at `offset` in `bytes` (which must hold offset + 4 bytes).
inline std::uint32_t le32(std::span<std::byte const> bytes,
                          std::size_t                offset = 0) {
    return std::to_integer<std::uint32_t>(bytes[offset])
         | std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U
         | std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U
         | std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U;
}

/// The little-endian number in the first min(8, size) bytes of `bytes`: a value of its size (1, 2, 4 or 8 bytes), a
/// short span gives what it has.
constexpr std::uint64_t leValue(std::span<std::byte const> bytes) {
    std::uint64_t v{};
    for(std::size_t i = 0; i != std::min<std::size_t>(bytes.size(), 8); ++i) {
        v |= std::to_integer<std::uint64_t>(bytes[i]) << (8U * i);
    }
    return v;
}

/// `value` appended as four little-endian bytes.
inline void appendLe32(std::vector<std::byte>& out,
                       std::uint32_t           value) {
    for(unsigned i = 0; i != 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
}

/// One word read from the target; nullopt when the read fails (what an unreadable word means is the caller's call).
template<typename Transport>
std::optional<std::uint32_t> tryReadWord(Transport&    target,
                                         std::uint32_t address) {
    std::array<std::byte, 4> raw{};
    try {
        target.readMemory(address, raw);
    } catch(std::exception const&) { return std::nullopt; }
    return le32(raw);
}

}   // namespace uc_log::printer
