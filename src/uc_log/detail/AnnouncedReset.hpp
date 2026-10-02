#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/// The host's side of an announced reset (Kvasir_SDK src/kvasir/Util/AnnouncedReset.hpp has the
/// firmware's side and the why): the firmware bumps `request` in a 16-byte block right before it
/// resets itself, the printer writes `ack` and keeps the probe off the chip until the reset is
/// over. An RP2040 touched by a J-Link while it resets is "recovered" by SEGGER's DLL through the
/// Rescue DP, which parks the core in the boot ROM.
namespace uc_log::detail::announced_reset {

/// The block's symbol: C linkage, so both linkers' maps carry this plain name.
inline constexpr std::string_view Symbol{"kvasir_announced_reset"};

/// Kvasir::AnnouncedReset::Magic / HostArmed - the same numbers.
inline constexpr std::uint32_t Magic     = 0x5253'5441U;
inline constexpr std::uint32_t HostArmed = 0x4D52'4148U;

/// Word offsets in the block: magic, armed, request, ack.
inline constexpr std::uint32_t ArmedOffset = 4;
inline constexpr std::uint32_t AckOffset   = 12;
inline constexpr std::uint32_t BlockSize   = 16;

struct Block {
    std::uint32_t magic{};
    std::uint32_t armed{};
    std::uint32_t request{};
    std::uint32_t ack{};

    bool valid() const { return magic == Magic; }

    /// The firmware waits for an ack: a request the host has not answered.
    bool pending() const { return valid() && request != 0 && request != ack; }

    /// How long the firmware asks the probe to stay off the chip: `request` bits 31:16 in 100 ms
    /// units, 0 = the printer's default (Kvasir::AnnouncedReset::nextRequest). A watchdog left to
    /// run out resets later than the default pause.
    std::uint32_t stayAwayMs() const { return (request >> 16) * 100U; }
};

inline Block decode(std::span<std::byte const,
                              BlockSize> raw) {
    auto const word = [&](std::size_t at) {
        std::uint32_t v{};
        for(std::size_t i = 0; i != 4; ++i) {
            v |= std::to_integer<std::uint32_t>(raw[at + i]) << (8 * i);
        }
        return v;
    };
    return Block{word(0), word(4), word(8), word(12)};
}

namespace detail {
    // the first hex number in `text` (with or without 0x) that is a whole token
    inline std::optional<std::uint32_t> firstHex(std::string_view text) {
        std::size_t pos = 0;
        while(pos < text.size()) {
            pos = text.find_first_not_of(" \t", pos);
            if(pos == std::string_view::npos) { break; }
            auto const end   = std::min(text.find_first_of(" \t", pos), text.size());
            auto       token = text.substr(pos, end - pos);
            if(token.starts_with("0x") || token.starts_with("0X")) { token.remove_prefix(2); }
            std::uint32_t value{};
            auto const [ptr, ec]
              = std::from_chars(token.data(), std::to_address(token.end()), value, 16);
            if(ec == std::errc{} && ptr == std::to_address(token.end()) && !token.empty()) {
                return value;
            }
            return std::nullopt;   // only the first token counts
        }
        return std::nullopt;
    }

    // the symbol as a name of its own: not a part of a longer identifier
    inline bool namesSymbol(std::string_view line) {
        for(auto at = line.find(Symbol); at != std::string_view::npos;
            at      = line.find(Symbol, at + 1))
        {
            auto const isIdent = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '_';
            };
            auto const after = at + Symbol.size();
            if((at == 0 || !isIdent(line[at - 1]))
               && (after == line.size() || !isIdent(line[after])))
            {
                return true;
            }
        }
        return false;
    }
}   // namespace detail

/// The block's address from a linker map. lld: "<vma> <lma> <size> <align> kvasir_announced_reset"
/// (and its input-section line "<vma> ... x.o:(.data.kvasir_announced_reset)"); GNU ld: "0x<addr>
/// kvasir_announced_reset", or - a symbol LTO made local - only the section line
/// ".data.kvasir_announced_reset" with "0x<addr> 0x<size> <file>" on it or on the next line.
/// Every line that names the symbol and starts with an address gives that address.
inline std::expected<std::uint32_t,
                     std::string>
addressFromMap(std::istream& map) {
    std::string line;
    bool        sectionLinePending = false;
    bool        discarded          = false;   // GNU ld's list of what --gc-sections dropped
    // address 0 is a discarded copy (GNU ld lists those at 0), never the block
    auto const found = [](std::optional<std::uint32_t> a) { return a && *a != 0; };
    while(std::getline(map, line)) {
        std::string_view const text{line};
        if(text.starts_with("Discarded input sections")) {
            discarded = true;
            continue;
        }
        if(text.starts_with("Memory Configuration")
           || text.starts_with("Linker script and memory map"))
        {
            discarded = false;
        }
        if(discarded) { continue; }
        if(sectionLinePending) {
            sectionLinePending = false;
            if(auto const a = detail::firstHex(text); found(a)) { return *a; }
        }
        if(!detail::namesSymbol(text)) { continue; }
        if(auto const a = detail::firstHex(text); found(a)) { return *a; }
        // GNU ld section line: the name first, the address after it or on the next line
        auto const at   = text.find(Symbol);
        auto const rest = text.substr(at + Symbol.size());
        if(auto const a = detail::firstHex(rest); found(a)) { return *a; }
        sectionLinePending = !detail::firstHex(rest).has_value();
    }
    return std::unexpected{std::format("no {} in the map file", Symbol)};
}

inline std::expected<std::uint32_t,
                     std::string>
addressFromMap(std::string const& mapFile) {
    std::ifstream map{mapFile};
    if(!map) { return std::unexpected{std::format("cannot open {:?}", mapFile)}; }
    return addressFromMap(map);
}

}   // namespace uc_log::detail::announced_reset
