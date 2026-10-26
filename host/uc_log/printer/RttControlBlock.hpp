#pragma once

// The SEGGER RTT control block as rtt.hpp lays it out (SEGGER's layout): a 16-byte id ("SEGGER RTT"), the number of
// up buffers, the number of down buffers, then one 24-byte descriptor per buffer, up buffers first: name pointer,
// buffer, size, write offset, read offset, flags. The host owns an up buffer's read offset and a down buffer's write
// offset; the target owns the other one.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Le32.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace uc_log::printer::rtt_block {

inline constexpr std::string_view id{"SEGGER RTT"};
inline constexpr std::uint32_t    headerSize     = 24;
inline constexpr std::uint32_t    descriptorSize = 24;
// offsets inside a descriptor
inline constexpr std::uint32_t bufferField      = 4;
inline constexpr std::uint32_t writeOffsetField = 12;
inline constexpr std::uint32_t readOffsetField  = 16;

/// Bytes of a block with `buffers` descriptors.
constexpr std::size_t bytesFor(std::uint32_t buffers) {
    return headerSize + std::size_t{descriptorSize} * buffers;
}

/// Where descriptor `index` starts, from the block's address.
constexpr std::uint32_t descriptorAt(std::size_t index) {
    return headerSize + descriptorSize * static_cast<std::uint32_t>(index);
}

/// Bytes waiting in a ring (offsets checked by the caller).
constexpr std::uint32_t filled(std::uint32_t size,
                               std::uint32_t writeOffset,
                               std::uint32_t readOffset) {
    return writeOffset >= readOffset ? writeOffset - readOffset : size - readOffset + writeOffset;
}

/// Bytes a ring can still take: one slot stays free, so full and empty differ (offsets checked by the caller).
constexpr std::uint32_t space(std::uint32_t size,
                              std::uint32_t writeOffset,
                              std::uint32_t readOffset) {
    return size - 1 - filled(size, writeOffset, readOffset);
}

struct Descriptor {
    std::uint32_t name;
    std::uint32_t buffer;
    std::uint32_t size;
    std::uint32_t writeOffset;
    std::uint32_t readOffset;
    std::uint32_t flags;

    [[nodiscard]] bool offsetsInRange() const {
        return size != 0 && writeOffset < size && readOffset < size;
    }
};

/// A block's bytes as read from the target: the header and as many descriptors as were read (hasId() alone needs only
/// the id's bytes).
class View {
public:
    explicit View(std::span<std::byte const> raw UC_LOG_LIFETIMEBOUND) : raw_{raw} {}

    [[nodiscard]] bool hasId() const {
        return raw_.size() >= id.size()
            && std::ranges::equal(raw_.first(id.size()), id, [](std::byte b, char c) {
                   return std::to_integer<char>(b) == c;
               });
    }

    /// The id field up to its NUL, unprintable bytes as '?' (for a dump of a block that may be damaged).
    [[nodiscard]] std::string printableId() const {
        std::string text;
        for(std::size_t i = 0; i != 16 && i != raw_.size() && raw_[i] != std::byte{}; ++i) {
            auto const c = std::to_integer<unsigned char>(raw_[i]);
            text += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
        }
        return text;
    }

    [[nodiscard]] std::uint32_t numUp() const { return le32(raw_, 16); }

    [[nodiscard]] std::uint32_t numDown() const { return le32(raw_, 20); }

    [[nodiscard]] std::size_t descriptorsRead() const {
        return raw_.size() < headerSize ? 0 : (raw_.size() - headerSize) / descriptorSize;
    }

    [[nodiscard]] Descriptor descriptor(std::size_t index) const {
        auto const at = descriptorAt(index);
        return Descriptor{le32(raw_, at),
                          le32(raw_, at + bufferField),
                          le32(raw_, at + 8),
                          le32(raw_, at + writeOffsetField),
                          le32(raw_, at + readOffsetField),
                          le32(raw_, at + 20)};
    }

private:
    std::span<std::byte const> raw_;
};

}   // namespace uc_log::printer::rtt_block
