#pragma once

// Words in bytes read from the target. Every target the printer reads is a little-endian Cortex-M.

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>

namespace uc_log::detail {

/// The little-endian word at `offset` in `bytes` (which must hold offset + 4 bytes).
inline std::uint32_t le32(std::span<std::byte const> bytes,
                          std::size_t                offset = 0) {
    return std::to_integer<std::uint32_t>(bytes[offset])
         | std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U
         | std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U
         | std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U;
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

}   // namespace uc_log::detail
