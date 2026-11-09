// The App: the filter - what the store applies, the filter page's actions, a line's location or module hidden.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/FilterFile.hpp"

#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>

namespace uc_log::printer::rtx {

FilterSources App::filterSources() const {
    FilterSources s;
    s.modules   = mirror_.moduleNamesById;
    s.locations = mirror_.locationList;
    s.compiled  = compiled_;
    s.time      = timeWindow();
    return s;
}

EntryStore::UcTimeWindow App::timeWindow() const {
    EntryStore::UcTimeWindow w;
    w.enabled        = mirror_.ucTimeFilterEnabled;
    w.live           = mirror_.ucTimeLiveMode;
    w.liveWindowSecs = mirror_.ucTimeLiveWindowSecs;
    w.minSec         = mirror_.minUcTimeSec;
    w.maxSec         = mirror_.maxUcTimeSec;
    return w;
}

void App::applyFilter(FilterState      next,
                      std::string_view said) {
    filter_ = std::move(next);
    if(hooks_.setFilter) { hooks_.setFilter(filter_); }
    auto const what = filterSummary(filter_, timeWindow());
    notify(
      fmt::format("{}{}", said, what.empty() ? std::string{" - everything shown"} : " - " + what));
}

void App::filterAction(FilterItem const& item) {
    auto time = timeWindow();
    switch(item.action) {
    case FilterAction::timeAll:
        time.enabled = false;
        time.live    = false;
        if(hooks_.setTime) { hooks_.setTime(time); }
        return;
    case FilterAction::timeLast10:
    case FilterAction::timeLast60:
        time.enabled        = true;
        time.live           = true;
        time.liveWindowSecs = item.action == FilterAction::timeLast10 ? 10.0 : 60.0;
        if(hooks_.setTime) { hooks_.setTime(time); }
        return;
    case FilterAction::timeFrom: ask(Field::timeFrom); return;
    case FilterAction::timeTo:   ask(Field::timeTo); return;
    case FilterAction::save:     ask(Field::saveFilter); return;
    case FilterAction::load:     ask(Field::loadFilter); return;
    case FilterAction::showAll:
        time.enabled = false;
        time.live    = false;
        if(hooks_.setTime) { hooks_.setTime(time); }
        applyFilter(FilterState{}, "filter cleared");
        return;
    case FilterAction::hideNoisy:
        applyFilter(switchFilterItem(filter_, item, filterSources()), "the noisy locations hidden");
        return;
    case FilterAction::pickModules:   openDialog(Modal::modules); return;
    case FilterAction::pickLocations: openDialog(Modal::locations); return;
    case FilterAction::unhideModule:
        applyFilter(switchFilterItem(filter_, item, filterSources()),
                    fmt::format("module {} shown", item.name));
        return;
    case FilterAction::showLocation:
        applyFilter(switchFilterItem(filter_, item, filterSources()),
                    fmt::format("{} shown", locationText(item.where)));
        return;
    case FilterAction::level:
    case FilterAction::channel:
    case FilterAction::module:
    case FilterAction::location:
        applyFilter(switchFilterItem(filter_, item, filterSources()), item.text);
        return;
    }
}

void App::filterByLine(Command command) {
    auto const index = log->current(mirror_);
    if(!index) { return; }
    auto const& c     = *mirror_.entries[*index].common;
    auto        next  = filter_;
    auto const  where = SourceLocation{mirror_.fileNameOf(c.locationKey),
                                       EntryStore::Mirror::lineOf(c.locationKey)};
    if(command == Command::hideLocation) {
        next.includedLocations.erase(where);
        next.excludedLocations.insert(where);
        applyFilter(std::move(next), fmt::format("{} hidden", locationText(where)));
    } else if(command == Command::onlyLocation) {
        next.excludedLocations.erase(where);
        next.includedLocations.insert(where);
        applyFilter(std::move(next), fmt::format("only {}", locationText(where)));
    } else if(command == Command::hideModule) {
        auto const module = mirror_.moduleNameOf(c.moduleId);
        if(module.empty()) {
            notify("the line has no module");
            return;
        }
        std::erase_if(next.excludedModules,
                      [&](std::string const& e) { return moduleUnder(e, module); });
        next.excludedModules.insert(module);
        applyFilter(std::move(next), fmt::format("module {} hidden", module));
    }
}

}   // namespace uc_log::printer::rtx
