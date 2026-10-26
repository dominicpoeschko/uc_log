#pragma once

// What the log view shows, as the user set it (the Filter tab, a filter file): levels, channels, source locations
// and modules. No FTXUI: printer/EntryStore.hpp compiles it for its scans, printer/FilterFile.hpp saves and loads it.

#include "uc_log/LogLevel.hpp"
#include "uc_log/printer/ModulePath.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace uc_log::printer {

/// A log line's place: file and line; line 0 stands for the whole file.
using SourceLocation = std::pair<std::string, std::size_t>;

struct FilterState {
    std::set<uc_log::LogLevel> enabledLogLevels;
    std::set<std::size_t>      enabledChannels;
    std::set<SourceLocation>   includedLocations;
    std::set<SourceLocation>   excludedLocations;
    // each hides its submodules too; an exclude list, so modules seen later are shown
    std::set<std::string> excludedModules;

    bool operator==(FilterState const&) const = default;
};

inline bool moduleHiddenBy(std::string_view             module,
                           std::set<std::string> const& excluded) {
    return std::ranges::any_of(excluded, [&](std::string const& p) {
        return uc_log::printer::moduleUnder(module, p);
    });
}

}   // namespace uc_log::printer
