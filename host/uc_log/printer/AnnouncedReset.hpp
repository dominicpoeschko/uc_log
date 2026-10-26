#pragma once

#include "uc_log/printer/Le32.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

/// The host's side of an announced reset (Kvasir_SDK src/kvasir/Runtime/AnnouncedReset.hpp has the
/// firmware's side and the why): the firmware bumps `request` in a 16-byte block right before it
/// resets itself, the printer writes `ack` and keeps the probe off the chip until the reset is
/// over. An RP2040 touched by a J-Link while it resets is "recovered" by SEGGER's DLL through the
/// Rescue DP, which parks the core in the boot ROM.
namespace uc_log::printer::announced_reset {

/// The block's symbol: C linkage, so both linkers' maps carry this plain name.
inline constexpr std::string_view symbol{"kvasir_announced_reset"};

/// Kvasir::AnnouncedReset::Magic / hostArmed - the same numbers.
inline constexpr std::uint32_t magicValue = 0x5253'5441U;
inline constexpr std::uint32_t hostArmed  = 0x4D52'4148U;

/// Word offsets in the block: magic, armed, request, ack.
inline constexpr std::uint32_t armedOffset = 4;
inline constexpr std::uint32_t ackOffset   = 12;
inline constexpr std::uint32_t blockSize   = 16;

/// How long the probe stays off the chip after the ack (the RP2040's reboot into its boot ROM or the next image
/// takes a few ms; the firmware's grace after the ack is ~0.1 s), and the longest stay-away a firmware's request is
/// granted (its field holds up to 6553.5 s).
inline constexpr std::chrono::milliseconds printerPause{1500};
inline constexpr std::chrono::milliseconds printerPauseMax{600'000};   // 10 min

struct Block {
    std::uint32_t magic{};
    std::uint32_t armed{};
    std::uint32_t request{};
    std::uint32_t ack{};

    [[nodiscard]] bool valid() const { return magic == magicValue; }

    /// The firmware waits for an ack: a request the host has not answered.
    [[nodiscard]] bool pending() const { return valid() && request != 0 && request != ack; }

    /// How long the firmware asks the probe to stay off the chip: `request` bits 31:16 in 100 ms
    /// units, 0 = the printer's default (Kvasir::AnnouncedReset::nextRequest). A watchdog left to
    /// run out resets later than the default pause.
    [[nodiscard]] std::uint32_t stayAwayMs() const { return (request >> 16) * 100U; }
};

inline Block decode(std::span<std::byte const,
                              blockSize> raw) {
    return Block{le32(raw, 0), le32(raw, 4), le32(raw, 8), le32(raw, 12)};
}

}   // namespace uc_log::printer::announced_reset
