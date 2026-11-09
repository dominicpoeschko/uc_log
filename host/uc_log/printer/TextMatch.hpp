#pragma once

// Matching what a person types against a list's texts, for any front end: case aside (ASCII: names, paths and
// commands are), every space-separated word of the query part of the text, in any order.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

inline char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string asciiLower(std::string_view s) {
    std::string out{s};
    for(auto& c : out) { c = asciiLower(c); }
    return out;
}

/// The space-separated words of `query`, in lower case.
inline std::vector<std::string> queryWords(std::string_view query) {
    std::vector<std::string> words;
    for(std::size_t pos = 0; pos < query.size();) {
        auto const start = query.find_first_not_of(' ', pos);
        if(start == std::string_view::npos) { break; }
        auto const end = std::min(query.find(' ', start), query.size());
        words.push_back(asciiLower(query.substr(start, end - start)));
        pos = end;
    }
    return words;
}

/// Whether every one of `words` (queryWords) is part of `lowerText` (in lower case already).
inline bool containsWords(std::string_view                lowerText,
                          std::vector<std::string> const& words) {
    return std::ranges::all_of(words, [&](std::string const& w) { return lowerText.contains(w); });
}

/// Whether every space-separated word of `query` is part of `text`, case aside; an empty query matches everything.
inline bool matchesWords(std::string_view text,
                         std::string_view query) {
    return containsWords(asciiLower(text), queryWords(query));
}

}   // namespace uc_log::printer
