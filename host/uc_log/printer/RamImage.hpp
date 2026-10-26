#pragma once

#include "jlink/JLink.hpp"
#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/MapSymbols.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

/// Where an image that lives in RAM (a Kvasir RAM_ONLY target) starts, for the printer's flash
/// and reset of it: the vector table's address and its first two words, from the same build the
/// printer downloads.
namespace uc_log::printer {

/// The linker symbol at the vector table's start (Kvasir_SDK linker/common_vectors_body.inc.ld);
/// the SDK's J-Link script takes the table from it too (cmake/tools/ram_image_jlink.py).
inline constexpr std::string_view vectorTableSymbol{"_LINKER_vectors_start_"};

/// The symbol's address from the map: both linkers write it as an assignment (lld "<vma> <lma> <size> <align>
/// _LINKER_vectors_start_ = .", GNU ld "0x<address> _LINKER_vectors_start_ = .").
inline std::expected<std::uint32_t,
                     std::string>
vectorTableFromMap(MapSymbols const& map) {
    if(auto const v = map.value(vectorTableSymbol)) { return *v; }
    return std::unexpected{std::format("no {} in the map file", vectorTableSymbol)};
}

/// The table's address from the map, its words from the hex image - the bytes the download
/// writes. Checked the way ram_image_jlink.py checks them: SP word-aligned, reset vector a
/// Thumb address.
inline std::expected<JLink::RamImageStart,
                     std::string>
ramImageStart(MapSymbols const&           map,
              std::span<HexSegment const> image) {
    auto const table = vectorTableFromMap(map);
    if(!table) { return std::unexpected{table.error()}; }
    for(auto const& s : image) {
        if(s.address > *table || *table - s.address + 8 > s.data.size()) { continue; }
        auto const vectors = std::span<std::byte const>{s.data}.subspan(*table - s.address);
        JLink::RamImageStart const start{*table, le32(vectors, 0), le32(vectors, 4)};
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
      std::format("the hex file has no vector table at {:#010x} ({})", *table, vectorTableSymbol)};
}

/// From the map MapFacts read and the hex file: what the printer's flash and reset of a RAM image start.
inline std::expected<JLink::RamImageStart,
                     std::string>
ramImageStart(MapSnapshot const& map,
              std::string const& hexFile) {
    if(!map.error.empty()) { return std::unexpected{"RAM image: " + map.error}; }
    std::ifstream hex{hexFile};
    if(!hex) { return std::unexpected{std::format("RAM image: cannot open {:?}", hexFile)}; }
    auto const image = parseIntelHex(hex);
    if(!image) { return std::unexpected{"RAM image: " + image.error()}; }
    auto start = ramImageStart(map.symbols, *image);
    if(!start) {
        return std::unexpected{std::format("RAM image ({}): {}", hexFile, start.error())};
    }
    return start;
}
}   // namespace uc_log::printer
