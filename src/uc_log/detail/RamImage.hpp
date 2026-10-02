#pragma once

#include "jlink/JLink.hpp"
#include "uc_log/detail/HexImage.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

/// Where an image that lives in RAM (a Kvasir RAM_ONLY target) starts, for the printer's flash
/// and reset of it: the vector table's address and its first two words, from the same build the
/// printer downloads.
namespace uc_log::detail {

/// The linker symbol at the vector table's start (Kvasir_SDK linker/common_vectors_body.inc.ld);
/// the SDK's J-Link script takes the table from it too (cmake/tools/ram_image_jlink.py).
inline constexpr std::string_view VectorTableSymbol{"_LINKER_vectors_start_"};

/// The symbol's address from a linker map. Both linkers write it as an assignment whose line
/// starts with the address: lld "<vma> <lma> <size> <align> _LINKER_vectors_start_ = .", GNU ld
/// "0x<address> _LINKER_vectors_start_ = .".
inline std::expected<std::uint32_t,
                     std::string>
vectorTableFromMap(std::istream& map) {
    std::string line;
    while(std::getline(map, line)) {
        std::string_view const text{line};
        auto const             at = text.find(VectorTableSymbol);
        if(at == std::string_view::npos || at == 0 || (text[at - 1] != ' ' && text[at - 1] != '\t'))
        {
            continue;
        }
        auto rest = text.substr(at + VectorTableSymbol.size());
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        if(!rest.starts_with('=')) { continue; }
        auto first = text.substr(0, at);
        first.remove_prefix(std::min(first.find_first_not_of(" \t"), first.size()));
        if(first.starts_with("0x") || first.starts_with("0X")) { first.remove_prefix(2); }
        std::uint32_t address{};
        auto const [ptr, ec]
          = std::from_chars(first.data(), std::to_address(first.end()), address, 16);
        if(ec != std::errc{} || ptr == first.data()) { continue; }
        return address;
    }
    return std::unexpected{std::format("no {} in the map file", VectorTableSymbol)};
}

/// The table's address from the map, its words from the hex image - the bytes the download
/// writes. Checked the way ram_image_jlink.py checks them: SP word-aligned, reset vector a
/// Thumb address.
inline std::expected<JLink::RamImageStart,
                     std::string>
ramImageStart(std::istream&               map,
              std::span<HexSegment const> image) {
    auto const table = vectorTableFromMap(map);
    if(!table) { return std::unexpected{table.error()}; }
    for(auto const& s : image) {
        if(s.address > *table || *table - s.address + 8 > s.data.size()) { continue; }
        auto const word = [&](std::size_t at) {
            std::uint32_t v{};
            for(std::size_t i = 0; i != 4; ++i) {
                v |= std::to_integer<std::uint32_t>(s.data[*table - s.address + at + i]) << (8 * i);
            }
            return v;
        };
        JLink::RamImageStart const start{*table, word(0), word(4)};
        if((start.initialSp & 3U) != 0 || (start.resetVector & 1U) == 0) {
            return std::unexpected{std::format(
              "implausible vector table at {:#010x}: SP {:#010x}, reset {:#010x} (SP must be "
              "word-aligned, the reset vector a Thumb address)",
              start.vectorTable,
              start.initialSp,
              start.resetVector)};
        }
        return start;
    }
    return std::unexpected{
      std::format("the hex file has no vector table at {:#010x} ({})", *table, VectorTableSymbol)};
}

/// From the files: what the printer's flash and reset of a RAM image start.
inline std::expected<JLink::RamImageStart,
                     std::string>
ramImageStart(std::string const& mapFile,
              std::string const& hexFile) {
    std::ifstream map{mapFile};
    if(!map) { return std::unexpected{std::format("RAM image: cannot open {:?}", mapFile)}; }
    std::ifstream hex{hexFile};
    if(!hex) { return std::unexpected{std::format("RAM image: cannot open {:?}", hexFile)}; }
    auto const image = parseIntelHex(hex);
    if(!image) { return std::unexpected{"RAM image: " + image.error()}; }
    auto start = ramImageStart(map, *image);
    if(!start) {
        return std::unexpected{
          std::format("RAM image ({}, {}): {}", mapFile, hexFile, start.error())};
    }
    return start;
}
}   // namespace uc_log::detail
