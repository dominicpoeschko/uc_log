#pragma once

// A filter over a fixed set of values (log levels, channels): an empty set means every value is shown. Toggling one
// value of an empty set shows all the others; a set that holds every value collapses back to empty, so a value
// added later (none here, but the meaning stays the same) is shown too.

#include <ranges>
#include <set>

namespace uc_log::printer {

template<typename T>
bool shownIn(std::set<T> const& set,
             T                  v) {
    return set.empty() || set.contains(v);
}

template<typename T,
         typename All>
void toggleIn(std::set<T>& set,
              All const&   all,
              T            v) {
    if(set.empty()) {
        set.insert(std::ranges::begin(all), std::ranges::end(all));
        set.erase(v);
    } else if(set.contains(v)) {
        set.erase(v);
    } else {
        set.insert(v);
    }
    if(set.size() == static_cast<std::size_t>(std::ranges::distance(all))) { set.clear(); }
}

}   // namespace uc_log::printer
