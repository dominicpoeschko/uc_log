#pragma once

// The filter page's content, for any front end: the things a person can switch - levels, channels, the target-time
// window, modules (with what the firmware compiled out), source locations by how much they log - as a page of boxes
// (filterForm) and what switching an item does to a FilterState. There can be hundreds of modules and locations: the
// page has a button that opens a combo box for each (their rows: FilterPicks.hpp) and a chip for every one hidden.
// The store compiles and applies the state (EntryStore::setFilterState); nothing here scans lines.

#include "uc_log/LogFilter.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LogFilterState.hpp"
#include "uc_log/printer/ModulePath.hpp"
#include "uc_log/printer/Outliers.hpp"
#include "uc_log/printer/PageForm.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/ToggleSet.hpp"
#include "uc_log/printer/Tone.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

inline constexpr std::array allLogLevels{uc_log::LogLevel::trace,
                                         uc_log::LogLevel::debug,
                                         uc_log::LogLevel::info,
                                         uc_log::LogLevel::warn,
                                         uc_log::LogLevel::error,
                                         uc_log::LogLevel::crit};

/// The channels a filter offers: 0 to 5.
inline constexpr std::size_t filterChannels = 6;

enum class FilterAction : std::uint8_t {
    level,
    channel,
    timeAll,
    timeLast10,
    timeLast60,
    timeFrom,
    timeTo,
    module,          // name: a node of the module tree (moduleNodes())
    location,        // where: a location (one of locationsByCount())
    pickModules,     // the window's: opens the modules' combo box
    pickLocations,   // ... the locations'
    unhideModule,    // name: shown again (with its submodules)
    showLocation,    // where: neither hidden nor the only one any more
    showAll,
    hideNoisy,
    save,
    load,
};

/// What a click switches. A module or a location by what it is, never by its place in a list: the locations' order
/// (by count) and the module tree (a new module) change while the log runs.
struct FilterItem {
    FilterAction   action{};
    std::size_t    value{};   // a level, a channel
    std::string    text;
    bool           on{};   // shown as switched on (a check, "only")
    Tone           tone{Tone::plain};
    std::string    name{};    // module, unhideModule
    SourceLocation where{};   // location, showLocation

    bool operator==(FilterItem const&) const = default;
};

/// What the panel lists besides the state.
struct FilterSources {
    std::vector<std::string> modules;   // seen (the mirror's moduleNamesById)
    std::vector<std::pair<SourceLocation, std::size_t>>
                                       locations;   // with their counts (the mirror's locationList)
    uc_log::Detail::FilterTable const* compiled{};   // the firmware's filter file, if known
    EntryStore::UcTimeWindow           time{};
};

/// The module tree's nodes: every module seen or named by the firmware's filter, and every prefix of one, sorted.
inline std::vector<std::string> moduleNodes(FilterSources const& s) {
    std::set<std::string> nodes;
    auto                  add = [&](std::string_view name) {
        if(name.empty()) { return; }
        for(auto dot = name.find('.'); dot != std::string_view::npos; dot = name.find('.', dot + 1))
        {
            nodes.emplace(name.substr(0, dot));
        }
        nodes.emplace(name);
    };
    for(auto const& m : s.modules) { add(m); }
    if(s.compiled != nullptr) {
        for(std::size_t i = 0; i < s.compiled->count; ++i) { add(s.compiled->rules[i].name()); }
    }
    return {nodes.begin(), nodes.end()};
}

/// The locations, the busiest first (ties: by name).
inline std::vector<std::pair<SourceLocation,
                             std::size_t>>
locationsByCount(FilterSources const& s) {
    auto out = s.locations;
    std::ranges::stable_sort(out, [](auto const& a, auto const& b) { return a.second > b.second; });
    return out;
}

/// "a.cpp:12" or "a.cpp" (line 0: the whole file).
inline std::string locationText(SourceLocation const& l) {
    return l.second == 0 ? l.first : fmt::format("{}:{}", l.first, l.second);
}

/// A short account of what the filter hides, "" for nothing: the panel's title and the status line say it.
namespace Detail {
    inline std::string counted(std::size_t      n,
                               std::string_view one,
                               std::string_view more) {
        return fmt::format("{} {}", n, n == 1 ? one : more);
    }
}   // namespace Detail

inline std::string filterSummary(FilterState const&              f,
                                 EntryStore::UcTimeWindow const& time) {
    std::vector<std::string> parts;
    if(!f.enabledLogLevels.empty()) {
        parts.push_back(fmt::format("{} of 6 levels", f.enabledLogLevels.size()));
    }
    if(!f.enabledChannels.empty()) {
        parts.push_back(fmt::format("{} of {} channels", f.enabledChannels.size(), filterChannels));
    }
    if(!f.excludedModules.empty()) {
        parts.push_back(Detail::counted(f.excludedModules.size(), "module", "modules") + " hidden");
    }
    if(!f.includedLocations.empty()) {
        parts.push_back("only "
                        + Detail::counted(f.includedLocations.size(), "location", "locations"));
    }
    if(!f.excludedLocations.empty()) {
        parts.push_back(Detail::counted(f.excludedLocations.size(), "location", "locations")
                        + " hidden");
    }
    if(time.enabled) {
        parts.push_back(time.live ? fmt::format("the last {:g} s", time.liveWindowSecs)
                                  : (std::isinf(time.maxSec)
                                       ? fmt::format("from {:g} s", time.minSec)
                                       : fmt::format("{:g} - {:g} s", time.minSec, time.maxSec)));
    }
    std::string out;
    for(auto const& p : parts) {
        if(!out.empty()) { out += " · "; }
        out += p;
    }
    return out;
}

namespace Detail {
    inline std::string compiledNote(uc_log::Detail::FilterTable const* compiled,
                                    std::string const&                 node) {
        if(compiled == nullptr) { return {}; }
        for(std::size_t i = 0; i < compiled->count; ++i) {
            auto const& rule = compiled->rules[i];
            if(rule.name() != node) { continue; }
            return rule.level == uc_log::Detail::levelOff
                   ? std::string{" (compiled out)"}
                   : fmt::format(" (compiled ≥ {})", uc_log::Detail::filterLevelName(rule.level));
        }
        return {};
    }
}   // namespace Detail

/// The filter as a page: levels, channels and the target-time window as switches and choices; modules and locations
/// each a button that opens their combo box (modulePicks / locationPicks) and a chip for every one hidden (a click
/// shows it again); show everything, hide the noisy locations, save and load.
inline PageForm<FilterItem> filterForm(FilterState const&   f,
                                       FilterSources const& s) {
    PageForm<FilterItem> p;
    auto const           item = [](FilterAction a, std::size_t value = 0) {
        FilterItem i;
        i.action = a;
        i.value  = value;
        return i;
    };

    auto&    levels = p.box("📊 Levels");
    PanelRow levelRow;
    for(auto const level : allLogLevels) {
        levelRow.push_back(p.check(levelName(level),
                                   shownIn(f.enabledLogLevels, level),
                                   item(FilterAction::level, static_cast<std::size_t>(level))));
    }
    levels.push_back(std::move(levelRow));

    auto&    channels = p.box("📡 Channels");
    PanelRow channelRow;
    for(std::size_t ch = 0; ch < filterChannels; ++ch) {
        channelRow.push_back(p.check(std::to_string(ch),
                                     shownIn(f.enabledChannels, ch),
                                     item(FilterAction::channel, ch)));
    }
    channels.push_back(std::move(channelRow));

    auto const& t     = s.time;
    bool const  last  = t.enabled && t.live;
    bool const  fixed = t.enabled && !t.live;
    auto&       time  = p.box("⏱ Target time");
    time.push_back(
      {p.option("all", !t.enabled, item(FilterAction::timeAll)),
       p.option("last 10 s", last && t.liveWindowSecs == 10.0, item(FilterAction::timeLast10)),
       p.option("last 60 s", last && t.liveWindowSecs == 60.0, item(FilterAction::timeLast60))});
    time.push_back({p.button(fixed ? fmt::format("from {:g} s …", t.minSec) : std::string{"from …"},
                             item(FilterAction::timeFrom),
                             ButtonLook::action,
                             fixed),
                    p.button(fixed && !std::isinf(t.maxSec) ? fmt::format("to {:g} s …", t.maxSec)
                                                            : std::string{"to …"},
                             item(FilterAction::timeTo),
                             ButtonLook::action,
                             fixed && !std::isinf(t.maxSec))});

    auto const nodes   = moduleNodes(s);
    auto&      modules = p.box("🧩 Modules", true);
    if(nodes.empty()) {
        modules.push_back({label("none seen yet")});
    } else {
        PanelRow row{p.button(
          fmt::format("🔎 Choose … ({})", Detail::counted(nodes.size(), "module", "modules")),
          item(FilterAction::pickModules))};
        if(f.excludedModules.empty()) { row.push_back(label(" all shown")); }
        for(auto const& hidden : f.excludedModules) {
            auto chip = item(FilterAction::unhideModule);
            chip.name = hidden;
            row.push_back(
              p.button(fmt::format("{}{} ✕", hidden, Detail::compiledNote(s.compiled, hidden)),
                       chip,
                       ButtonLook::chip,
                       false,
                       Tone::warn));
        }
        modules.push_back(std::move(row));
    }

    auto const located   = locationsByCount(s);
    auto&      locations = p.box("📍 Locations", true);
    if(located.empty()) {
        locations.push_back({label("none seen yet")});
    } else {
        PanelRow row{p.button(
          fmt::format("🔎 Choose … ({})", Detail::counted(located.size(), "location", "locations")),
          item(FilterAction::pickLocations))};
        if(f.includedLocations.empty() && f.excludedLocations.empty()) {
            row.push_back(label(" all shown"));
        }
        for(auto const& only : f.includedLocations) {
            auto chip  = item(FilterAction::showLocation);
            chip.where = only;
            row.push_back(p.button(fmt::format("only {} ✕", locationText(only)),
                                   chip,
                                   ButtonLook::chip,
                                   true,
                                   Tone::ok));
        }
        for(auto const& hidden : f.excludedLocations) {
            auto chip  = item(FilterAction::showLocation);
            chip.where = hidden;
            row.push_back(p.button(fmt::format("{} ✕", locationText(hidden)),
                                   chip,
                                   ButtonLook::chip,
                                   false,
                                   Tone::warn));
        }
        locations.push_back(std::move(row));
    }

    // the whole filter: the page's tools
    p.tool("🗑 Show everything", item(FilterAction::showAll), ButtonLook::danger);
    p.tool("🎯 Hide the noisy", item(FilterAction::hideNoisy));
    p.tool("💾 Save …", item(FilterAction::save));
    p.tool("📂 Load …", item(FilterAction::load));
    return p;
}

/// What switching a level, channel, module or location item does to `f` (the other actions are the window's: they
/// need the store, a dialog or a file). A module hides its submodules; a hidden ancestor is shown again first. A
/// location goes round: shown -> hidden -> only this -> shown.
inline FilterState switchFilterItem(FilterState          f,
                                    FilterItem const&    item,
                                    FilterSources const& s) {
    switch(item.action) {
    case FilterAction::level:
        toggleIn(f.enabledLogLevels, allLogLevels, static_cast<uc_log::LogLevel>(item.value));
        break;
    case FilterAction::channel:
        toggleIn(f.enabledChannels, std::views::iota(std::size_t{0}, filterChannels), item.value);
        break;
    case FilterAction::module:
        {
            auto const& node = item.name;
            if(node.empty()) { break; }
            auto& excluded = f.excludedModules;
            if(excluded.erase(node) != 0) { break; }
            if(moduleHiddenBy(node, excluded)) {
                // an ancestor hides it: the ancestors that do are shown again, and with them their subtrees
                std::erase_if(excluded, [&](std::string const& e) { return moduleUnder(node, e); });
                break;
            }
            std::erase_if(excluded, [&](std::string const& e) { return moduleUnder(e, node); });
            excluded.insert(node);
            break;
        }
    case FilterAction::location:
        {
            auto const& where = item.where;
            if(f.includedLocations.erase(where) != 0) { break; }   // only -> shown
            if(f.excludedLocations.erase(where) != 0) {            // hidden -> only
                f.includedLocations.insert(where);
                break;
            }
            f.excludedLocations.insert(where);   // shown -> hidden
            break;
        }
    case FilterAction::unhideModule: f.excludedModules.erase(item.name); break;
    case FilterAction::showLocation:
        f.includedLocations.erase(item.where);
        f.excludedLocations.erase(item.where);
        break;
    case FilterAction::showAll: f = FilterState{}; break;
    case FilterAction::hideNoisy:
        {
            auto const noisy = computeOutliers(s.locations, OutlierMethod::iqrTukey, 1.5, 0.0, 0);
            for(auto const& where : noisy.wouldExclude) { f.excludedLocations.insert(where); }
            break;
        }
    case FilterAction::timeAll:
    case FilterAction::timeLast10:
    case FilterAction::timeLast60:
    case FilterAction::timeFrom:
    case FilterAction::timeTo:
    case FilterAction::pickModules:
    case FilterAction::pickLocations:
    case FilterAction::save:
    case FilterAction::load:          break;
    }
    return f;
}

}   // namespace uc_log::printer
