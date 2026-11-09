#pragma once

// The filter's combo boxes, for any front end: the modules' (the module tree, a submodule under its parent) and the
// locations' (the busiest first), the rows a query matches with each one's mark and what it adds, and how an open
// box keeps its rows in place while the counts change. A click on a row switches its FilterItem (switchFilterItem).

#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/FilterPanel.hpp"
#include "uc_log/printer/LogFilterState.hpp"
#include "uc_log/printer/ModulePath.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/TextMatch.hpp"
#include "uc_log/printer/Tone.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// A row of a combo box (the modules' or the locations'): `item` what switchFilterItem takes for it, its mark
/// (✔ shown, ✘ hidden, (✘) hidden by a parent module; · ✘ ✔ for a location), its text, what it adds.
struct PickRow {
    FilterItem  item;   // what a click on it switches
    std::string mark;
    std::string text;
    std::string detail;
    Tone        tone{Tone::plain};

    bool operator==(PickRow const&) const = default;
};

/// Whether `a` and `b` are the same item: what it switches and which (a module, a location), not how it reads.
inline bool sameItem(FilterItem const& a,
                     FilterItem const& b) {
    return a.action == b.action && a.name == b.name && a.where == b.where;
}

/// The place in `rows` of the row that is `item`, if any.
inline std::optional<std::size_t> placeOf(std::vector<PickRow> const& rows,
                                          FilterItem const&           item) {
    auto const it
      = std::ranges::find_if(rows, [&](PickRow const& r) { return sameItem(r.item, item); });
    if(it == rows.end()) { return std::nullopt; }
    return static_cast<std::size_t>(it - rows.begin());
}

/// `fresh` (the locations counted anew) in the order `shown` had them, while their box is open: a row must not move
/// under the mouse or the cursor as the counts change. A location seen since comes after them.
inline std::vector<PickRow> keepPlaces(std::vector<PickRow>        fresh,
                                       std::vector<PickRow> const& shown) {
    std::map<SourceLocation, PickRow> byWhere;
    std::vector<SourceLocation>       freshOrder;   // the keys, before the rows move into the map
    freshOrder.reserve(fresh.size());
    for(auto& row : fresh) {
        freshOrder.push_back(row.item.where);
        byWhere.emplace(row.item.where, std::move(row));
    }
    std::vector<PickRow> ordered;
    ordered.reserve(byWhere.size());
    auto const take = [&](SourceLocation const& where) {
        if(auto const it = byWhere.find(where); it != byWhere.end()) {
            ordered.push_back(std::move(it->second));
            byWhere.erase(it);
        }
    };
    for(auto const& old : shown) { take(old.item.where); }
    for(auto const& where : freshOrder) { take(where); }
    return ordered;
}

/// The modules' combo box: the module tree in order, a submodule indented under its parent, those `query` matches.
inline std::vector<PickRow> modulePicks(FilterState const&   f,
                                        FilterSources const& s,
                                        std::string_view     query) {
    auto const           nodes = moduleNodes(s);
    std::vector<PickRow> out;
    for(auto const& node : nodes) {
        if(!matchesWords(node, query)) { continue; }
        bool const self   = f.excludedModules.contains(node);
        bool const hidden = moduleHiddenBy(node, f.excludedModules);
        auto const depth  = static_cast<std::size_t>(std::ranges::count(node, '.'));
        auto       note   = Detail::compiledNote(s.compiled, node);
        if(note.starts_with(' ')) { note.erase(0, 1); }
        FilterItem item;
        item.action = FilterAction::module;
        item.name   = node;
        out.push_back({std::move(item),
                       self ? "✘" : (hidden ? "(✘)" : "✔"),
                       std::string(depth * 2, ' ') + node,
                       std::move(note),
                       hidden ? Tone::warn : Tone::plain});
    }
    return out;
}

/// The locations' combo box: the busiest first, those `query` matches; a click goes round shown, hidden, only.
inline std::vector<PickRow> locationPicks(FilterState const&   f,
                                          FilterSources const& s,
                                          std::string_view     query) {
    auto const           located = locationsByCount(s);
    std::vector<PickRow> out;
    for(auto const& [where, count] : located) {
        auto const text = locationText(where);
        if(!matchesWords(text, query)) { continue; }
        bool const only   = f.includedLocations.contains(where);
        bool const hidden = f.excludedLocations.contains(where);
        FilterItem item;
        item.action = FilterAction::location;
        item.where  = where;
        out.push_back({std::move(item),
                       only ? " ✔ " : (hidden ? " ✘ " : " · "),
                       text,
                       fmt::format("×{}", shortCount(count)),
                       only ? Tone::ok : (hidden ? Tone::warn : Tone::plain)});
    }
    return out;
}

}   // namespace uc_log::printer
