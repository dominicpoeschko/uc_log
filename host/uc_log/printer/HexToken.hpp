#pragma once

// Hex numbers in the printer's text inputs: linker-map columns and the command line's word lists. One reader for
// all of them: a number is a whole token, `0x`/`0X` optional, and fits 32 bits.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace uc_log::printer {

/// The hex number in `token` (all of it, `0x` optional), or nullopt.
inline std::optional<std::uint32_t> hexValue(std::string_view token) {
    if(token.starts_with("0x") || token.starts_with("0X")) { token.remove_prefix(2); }
    if(token.empty()) { return std::nullopt; }
    std::uint32_t value{};
    auto const [ptr, ec]
      = std::from_chars(std::to_address(token.begin()), std::to_address(token.end()), value, 16);
    if(ec != std::errc{} || ptr != std::to_address(token.end())) { return std::nullopt; }
    return value;
}

/// The first blank-separated token of `text` as a hex number. On success `text` moves past it; on failure `text`
/// is left as it was.
inline std::optional<std::uint32_t> hexToken(std::string_view& text) {
    auto const begin = text.find_first_not_of(" \t");
    if(begin == std::string_view::npos) { return std::nullopt; }
    auto const end   = std::min(text.find_first_of(" \t", begin), text.size());
    auto const value = hexValue(text.substr(begin, end - begin));
    if(value) { text.remove_prefix(end); }
    return value;
}

/// Up to `count` leading hex tokens of `text`; stops at the first token that is none.
inline std::vector<std::uint32_t> hexTokens(std::string_view text,
                                            std::size_t      count) {
    std::vector<std::uint32_t> out;
    while(out.size() != count) {
        auto const v = hexToken(text);
        if(!v) { break; }
        out.push_back(*v);
    }
    return out;
}

/// "<a>,<b>,..." of hex words, at most `count`; empty when one of them is malformed or there are more (a
/// command-line value with a word too many is refused, not cut short).
inline std::vector<std::uint32_t> hexWords(std::string_view text,
                                           std::size_t      count) {
    std::vector<std::uint32_t> out;
    if(text.empty()) { return out; }
    while(true) {
        auto const comma = text.find(',');
        auto const v     = hexValue(text.substr(0, comma));
        if(!v || out.size() == count) { return {}; }
        out.push_back(*v);
        if(comma == std::string_view::npos) { return out; }
        text = text.substr(comma + 1);
    }
}

}   // namespace uc_log::printer
