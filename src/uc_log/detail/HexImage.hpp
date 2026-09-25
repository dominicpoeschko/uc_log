#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <istream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// An Intel HEX image, to check that the board runs the build the log is decoded with.
namespace uc_log::detail {

struct HexSegment {
    std::uint32_t          address{};
    std::vector<std::byte> data{};
};

/// zlib's CRC-32, the same as Python's zlib.crc32.
inline std::uint32_t crc32(std::span<std::byte const> data,
                           std::uint32_t              crc = 0) {
    static constexpr auto Table = [] {
        std::array<std::uint32_t, 256> t{};
        for(std::uint32_t i = 0; i != 256; ++i) {
            std::uint32_t c = i;
            for(int k = 0; k != 8; ++k) { c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U; }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for(auto const b : data) {
        crc = Table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

/// Contiguous records become one segment; start address records (03, 05) are skipped.
inline std::expected<std::vector<HexSegment>,
                     std::string>
parseIntelHex(std::istream& in) {
    std::vector<HexSegment> segments;
    std::uint32_t           base = 0;
    std::string             line;
    std::size_t             lineNumber = 0;
    auto const              fail       = [&](std::string_view why) {
        return std::unexpected{"hex file line " + std::to_string(lineNumber) + ": "
                               + std::string{why}};
    };
    while(std::getline(in, line)) {
        ++lineNumber;
        while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
        if(line.empty()) { continue; }
        if(line.front() != ':' || line.size() < 11 || line.size() % 2 == 0) {
            return fail("not a record");
        }
        std::vector<std::uint8_t> bytes((line.size() - 1) / 2);
        for(std::size_t i = 0; i != bytes.size(); ++i) {
            auto const pair = std::string_view{line}.substr(1 + i * 2, 2);
            if(std::from_chars(pair.begin(), pair.end(), bytes[i], 16).ptr != pair.end()) {
                return fail("not hex");
            }
        }
        std::uint8_t sum = 0;
        for(auto const b : bytes) { sum = static_cast<std::uint8_t>(sum + b); }
        if(sum != 0) { return fail("checksum"); }
        std::size_t const count = bytes[0];
        if(bytes.size() != count + 5) { return fail("length"); }
        std::uint32_t const offset = (std::uint32_t{bytes[1]} << 8U) | bytes[2];
        auto const          type   = bytes[3];
        if(type == 1) { break; }
        if(type == 2 && count == 2) {
            base = ((std::uint32_t{bytes[4]} << 8U) | bytes[5]) << 4U;
        } else if(type == 4 && count == 2) {
            base = ((std::uint32_t{bytes[4]} << 8U) | bytes[5]) << 16U;
        } else if(type == 0) {
            std::uint32_t const address = base + offset;
            if(segments.empty() || segments.back().address + segments.back().data.size() != address)
            {
                segments.push_back({address, {}});
            }
            for(std::size_t i = 0; i != count; ++i) {
                segments.back().data.push_back(static_cast<std::byte>(bytes[4 + i]));
            }
        }
    }
    return segments;
}

inline std::uint32_t imageCrc(std::span<HexSegment const> segments) {
    std::uint32_t crc = 0;
    for(auto const& s : segments) { crc = crc32(s.data, crc); }
    return crc;
}

struct ImageCheck {
    enum class Result { match, different, unreadable };

    Result        result{Result::unreadable};
    std::uint32_t firstDifference{};   // an address, for Result::different
    std::size_t   comparedBytes{};
    std::string   error{};
};

/// Only below the Cortex-M SRAM base: RAM changes as soon as the firmware runs.
template<typename ReadF>
ImageCheck compareImage(std::span<HexSegment const> segments,
                        ReadF&&                     read) {
    static constexpr std::uint32_t SramBase = 0x2000'0000U;
    static constexpr std::size_t   Chunk    = 4096;
    ImageCheck                     check{};
    std::vector<std::byte>         buffer(Chunk);
    for(auto const& s : segments) {
        if(s.address >= SramBase) { continue; }
        for(std::size_t done = 0; done < s.data.size(); done += Chunk) {
            std::size_t const n   = std::min(Chunk, s.data.size() - done);
            auto const        out = std::span{buffer}.first(n);
            auto const        at  = s.address + static_cast<std::uint32_t>(done);
            if(!read(at, out)) {
                check.result = ImageCheck::Result::unreadable;
                check.error  = "cannot read target memory";
                return check;
            }
            for(std::size_t i = 0; i != n; ++i) {
                if(out[i] != s.data[done + i]) {
                    check.result          = ImageCheck::Result::different;
                    check.firstDifference = at + static_cast<std::uint32_t>(i);
                    return check;
                }
            }
            check.comparedBytes += n;
        }
    }
    check.result
      = check.comparedBytes != 0 ? ImageCheck::Result::match : ImageCheck::Result::unreadable;
    if(check.comparedBytes == 0) { check.error = "the image has nothing below the SRAM"; }
    return check;
}
}   // namespace uc_log::detail
