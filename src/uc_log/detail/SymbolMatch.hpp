#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

/// Finding a symbol by a few typed characters (the Inspect tab's watch input): which names a
/// query matches and which of them the user most likely means.
namespace uc_log::detail {

inline char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string asciiLower(std::string_view s) {
    std::string out{s};
    for(auto& c : out) { c = asciiLower(c); }
    return out;
}

/// Where a name's last component starts: `Kvasir::Fault::lastFault` -> `lastFault`,
/// `f(int)::count` -> `count`. A `::` inside template arguments or a parameter list is none.
inline std::size_t symbolLeafStart(std::string_view name) {
    int         depth = 0;
    std::size_t start = 0;
    for(std::size_t i = 0; i != name.size(); ++i) {
        char const c = name[i];
        if(c == '<' || c == '(') {
            ++depth;
        } else if(c == '>' || c == ')') {
            if(depth != 0) { --depth; }
        } else if(c == ':' && depth == 0 && i + 1 != name.size() && name[i + 1] == ':') {
            start = i + 2;
            ++i;
        }
    }
    // `operator<`, an unbalanced name: no component found is better than an empty one
    return start < name.size() ? start : 0;
}

inline std::string_view symbolLeaf(std::string_view name) {
    return name.substr(symbolLeafStart(name));
}

/// A typed query: terms separated by spaces, every one of which a name has to contain. A term
/// is plain text without case (so a whole demangled name, parentheses and all, finds itself),
/// and a regex where the text is not found.
class SymbolQuery {
public:
    explicit SymbolQuery(std::string_view text) {
        std::size_t i = 0;
        while(i != text.size()) {
            if(text[i] == ' ') {
                ++i;
                continue;
            }
            auto const end = std::min(text.find(' ', i), text.size());
            Term       term;
            term.lower = asciiLower(text.substr(i, end - i));
            if(term.lower.find_first_of(".*+?[]{}|^$\\()") != std::string::npos) {
                try {
                    term.regex.emplace(std::string{text.substr(i, end - i)}, std::regex::icase);
                } catch(std::regex_error const&) {}
            }
            terms_.push_back(std::move(term));
            i = end;
        }
    }

    bool empty() const { return terms_.empty(); }

    /// The terms in lower case, to mark them in a shown name.
    std::vector<std::string> literals() const {
        std::vector<std::string> out;
        for(auto const& t : terms_) { out.push_back(t.lower); }
        return out;
    }

    /// No value: the name does not match. Otherwise how well, smaller is better: per term 0 it
    /// is the last component, 1 that starts with it, 2 a word of it does (`last_fault`,
    /// `lastFault`), 3 it is in there, 4 a component before it starts with it, 5 it is
    /// somewhere in the name, 6 only as a regex.
    std::optional<unsigned> rank(std::string_view name) const {
        if(terms_.empty()) { return std::nullopt; }
        auto const lower     = asciiLower(name);
        auto const leafStart = symbolLeafStart(name);
        unsigned   sum       = 0;
        for(auto const& term : terms_) {
            auto const r = rankTerm(term, name, lower, leafStart);
            if(!r) { return std::nullopt; }
            sum += *r;
        }
        return sum;
    }

private:
    struct Term {
        std::string               lower;
        std::optional<std::regex> regex;
    };

    static bool wordStart(std::string_view name,
                          std::size_t      pos) {
        if(pos == 0) { return true; }
        char const before      = name[pos - 1];
        char const here        = name[pos];
        bool const alnumBefore = (before >= 'a' && before <= 'z')
                              || (before >= 'A' && before <= 'Z')
                              || (before >= '0' && before <= '9');
        if(!alnumBefore) { return true; }
        return before >= 'a' && before <= 'z' && here >= 'A' && here <= 'Z';
    }

    static std::optional<unsigned> rankTerm(Term const&      term,
                                            std::string_view name,
                                            std::string_view lower,
                                            std::size_t      leafStart) {
        // the last component's own name: what its template arguments or parameters hold
        // counts like a scope (`values<Kvasir::I2C::Answer>` is no I2C variable)
        auto const whole = lower.substr(leafStart);
        auto const args  = whole.find_first_of("<(");
        auto const leaf  = args == 0 ? whole : whole.substr(0, args);
        if(leaf == term.lower || whole == term.lower) { return 0U; }
        if(leaf.starts_with(term.lower)) { return 1U; }
        bool inLeaf = false;
        for(auto pos = leaf.find(term.lower); pos != std::string_view::npos;
            pos      = leaf.find(term.lower, pos + 1))
        {
            if(wordStart(name.substr(leafStart), pos)) { return 2U; }
            inLeaf = true;
        }
        if(inLeaf) { return 3U; }
        bool inName = false;
        for(auto pos = lower.find(term.lower); pos != std::string_view::npos;
            pos      = lower.find(term.lower, pos + 1))
        {
            if(pos == 0 || (pos >= 2 && lower[pos - 1] == ':' && lower[pos - 2] == ':')) {
                return 4U;
            }
            inName = true;
        }
        if(inName) { return 5U; }
        if(term.regex && std::regex_search(name.begin(), name.end(), *term.regex)) { return 6U; }
        return std::nullopt;
    }

    std::vector<Term> terms_;
};

}   // namespace uc_log::detail
