#pragma once

// An inspector's values as text, for any front end: a symbol's name shortened (a template type runs to kilobytes), a
// watched value as numbers, a ratio as a bar. The pages built from them are in InspectorPages.hpp.

#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/SymbolMatch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

/// Template arguments nested deeper than one level become <…> once a name is longer than `max` - a trace ring's or a
/// driver's type runs to kilobytes.
inline std::string shortName(std::string_view name,
                             std::size_t      max = 100) {
    if(name.size() <= max) { return std::string{name}; }
    std::string out;
    int         depth = 0;
    for(char c : name) {
        if(c == '<') {
            ++depth;
            if(depth == 2) {
                out += "<…>";
                continue;
            }
        } else if(c == '>') {
            --depth;
            if(depth == 1) { continue; }
        }
        if(depth < 2) { out += c; }
    }
    if(out.size() > max + 20) {
        // the last component says which variable or function it is: the cut goes before it
        auto const leaf = std::string{out.substr(symbolLeafStart(out))};
        bool const keep = leaf.size() != out.size() && leaf.size() + 12 <= max;
        out.resize(keep ? max - leaf.size() - 2 : max);
        // not through the middle of a "…" put in above
        while(!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80U) != 0) {
            out.pop_back();
        }
        out += keep ? "…::" + leaf : std::string{"…"};
    }
    return out;
}

/// A watched value as numbers: by its size, little-endian; another size as bytes.
inline std::string watchValue(std::vector<std::byte> const& v) {
    if(v.empty()) { return "-"; }
    std::uint64_t const u = leValue(v);
    switch(v.size()) {
    case 1: return fmt::format("{} / {} / {:#04x}", u, static_cast<std::int8_t>(u), u);
    case 2: return fmt::format("{} / {} / {:#06x}", u, static_cast<std::int16_t>(u), u);
    case 4:
        {
            float f{};
            auto  w = static_cast<std::uint32_t>(u);
            std::memcpy(&f, &w, 4);
            return fmt::format("{} / {} / {:#010x} / {:g}f", u, static_cast<std::int32_t>(u), u, f);
        }
    case 8:  return fmt::format("{} / {} / {:#018x}", u, static_cast<std::int64_t>(u), u);
    default: break;
    }
    std::string hex;
    for(auto b : v) { hex += fmt::format("{:02x} ", std::to_integer<unsigned>(b)); }
    return hex;
}

/// `ratio` (0..1) as `cells` of a bar: full blocks, an eighth block, the rest light shade.
inline std::string gaugeText(double      ratio,
                             std::size_t cells) {
    static constexpr std::array<std::string_view, 8> eighths{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    auto const                                       eighthsShown
      = static_cast<std::size_t>(std::clamp(ratio, 0.0, 1.0) * static_cast<double>(cells * 8));
    std::string out;
    std::size_t used = 0;
    for(; used < eighthsShown / 8; ++used) { out += "█"; }
    if(used < cells && eighthsShown % 8 != 0) {
        out += eighths[eighthsShown % 8];
        ++used;
    }
    for(; used < cells; ++used) { out += "░"; }
    return out;
}

}   // namespace uc_log::printer
