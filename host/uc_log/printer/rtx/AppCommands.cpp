// The App: a command (printer/Commands.hpp), from a key, a click, the palette or a hint.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/FilterFile.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/PanelText.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>

namespace uc_log::printer::rtx {

void App::copyLine() {
    auto const index = log->current(mirror_);
    if(!index) { return; }
    SetClipboard(plainText(mirror_.entries[*index], mirror_, log->columns().text, false));
    notify(fmt::format("line {} copied", *index + 1));
}

void App::run(Command command) {
    if(logHidden() && !isPage(panel_)) {
        // zoomed: the keys that move in the log move the panel
        auto&      pv    = panelViews_[static_cast<std::size_t>(panel_)];
        auto const all   = IndexLines{panelLines()};
        auto const shown = panelBody();
        auto const step  = static_cast<long>(shown);
        bool       moved = true;
        if(command == Command::lineUp || command == Command::lineDown) {
            pv.scroll(all, shown, command == Command::lineUp ? -1 : 1);
        } else if(command == Command::pageUp || command == Command::pageDown) {
            pv.scroll(all, shown, command == Command::pageUp ? -step : step);
        } else if(command == Command::oldest) {
            pv.home(all);
        } else if(command == Command::follow) {
            pv.follow();
        } else {
            moved = false;
        }
        if(moved) {
            relayout();
            return;
        }
    }
    auto const lines   = SnapshotLines{&mirror_.entries};
    auto const rows    = logBody();
    auto&      at      = log->viewport();
    auto&      columns = log->columns();
    auto const step    = static_cast<long>(rows);
    switch(command) {
    case Command::lineUp:     at.moveSelection(lines, rows, -1); break;
    case Command::lineDown:   at.moveSelection(lines, rows, 1); break;
    case Command::pageUp:     at.moveSelection(lines, rows, -step); break;
    case Command::pageDown:   at.moveSelection(lines, rows, step); break;
    case Command::shiftLeft:  log->hOffset() -= std::min<std::size_t>(log->hOffset(), 8); break;
    case Command::shiftRight: log->hOffset() += 8; break;
    case Command::oldest:     at.home(lines); break;
    case Command::follow:     at.follow(); break;
    case Command::search:     ask(Field::search); break;
    case Command::jumpToTime: ask(Field::jumpToTime); break;
    case Command::copyLine:   copyLine(); break;
    case Command::exportText:
        if(hooks_.exportText) { hooks_.exportText(); }
        break;
    case Command::details:     togglePanel(Tab::details); break;
    case Command::messages:    togglePanel(Tab::messages); break;
    case Command::buildOutput: togglePanel(Tab::build); break;
    case Command::help:        togglePanel(Tab::help); break;
    case Command::palette:     openDialog(Modal::palette); break;
    case Command::closePanel:  openPanel(Tab::logs); break;
    case Command::splitPanel:
        if(panel_ == Tab::logs) {
            notify("no page to split: pick a tab");
        } else {
            split_ = !split_;
        }
        break;
    case Command::filter:       togglePanel(Tab::filter); break;
    case Command::settings:     togglePanel(Tab::settings); break;
    case Command::debugger:     togglePanel(Tab::debugger); break;
    case Command::health:       togglePanel(Tab::health); break;
    case Command::inspect:      togglePanel(Tab::inspect); break;
    case Command::hideLocation:
    case Command::onlyLocation:
    case Command::hideModule:   filterByLine(command); break;
    case Command::clearFilter:
        {
            auto time    = timeWindow();
            time.enabled = false;
            time.live    = false;
            if(hooks_.setTime) { hooks_.setTime(time); }
            applyFilter(FilterState{}, "filter cleared");
            break;
        }
    case Command::hideNoisy:
        applyFilter(switchFilterItem(filter_,
                                     FilterItem{FilterAction::hideNoisy, 0, {}, false, Tone::plain},
                                     filterSources()),
                    "the noisy locations hidden");
        break;
    case Command::metrics: togglePanel(Tab::metrics); break;
    case Command::clearMetrics:
        if(metrics_ != nullptr) { metrics_->clearMetrics(); }
        notify("metrics cleared");
        break;
    case Command::saveFilter:       ask(Field::saveFilter); break;
    case Command::loadFilter:       ask(Field::loadFilter); break;
    case Command::toggleHostTime:   columns.hostTime = !columns.hostTime; break;
    case Command::toggleTargetTime: columns.targetTime = !columns.targetTime; break;
    case Command::toggleChannel:    columns.channel = !columns.channel; break;
    case Command::toggleLevel:      columns.level = !columns.level; break;
    case Command::toggleLocation:   columns.location = !columns.location; break;
    case Command::toggleModule:     columns.module = !columns.module; break;
    case Command::toggleFunction:   columns.function = !columns.function; break;
    case Command::clearMessages:
        if(hooks_.clearMessages) { hooks_.clearMessages(); }
        notify("messages and counts cleared");
        break;
    case Command::reset:
        if(hooks_.reset) { hooks_.reset(); }
        notify("reset");
        break;
    case Command::flash:
        if(hooks_.flash) { hooks_.flash(); }
        notify("flashing");
        break;
    case Command::build:
        if(hooks_.build) { hooks_.build(false); }
        break;
    case Command::buildAndFlash:
        if(hooks_.build) { hooks_.build(true); }
        break;
    case Command::quit:
        if(hooks_.quit) { hooks_.quit(); }
        break;
    }
    relayout();
}

}   // namespace uc_log::printer::rtx
