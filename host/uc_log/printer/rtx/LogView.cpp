#include "uc_log/printer/rtx/LogView.hpp"

#include "uc_log/printer/PanelRows.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace uc_log::printer::rtx {

LogLines::LogLines() { Import("spans", &LogLines::spans); }

LogView::LogView() : lines{::rtxui::Ref<LogLines>::New()} {
    Import("log-lines", ::rtxui::ComponentFactory{[c = lines] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("legend", &LogView::legend);
    Import("scrollbar", &LogView::scrollbar);
    Import("title", &LogView::title);
    Import("hint", &LogView::hint);
    Import("ring", &LogView::ring);
    Import("tools", &LogView::tools);
    Import("Press", [this](std::string const& arg) {
        if(pressed) { pressed(argIndex(arg)); }
    });
}

void LogView::show(EntryStore::Mirror const& mirror,
                   std::size_t               rows,
                   std::size_t               width) {
    rows_  = std::max<std::size_t>(rows, 1);
    width_ = std::max<std::size_t>(width, 2);
    LogScreen<RtxuiWidth>::Frame frame;
    frame.storeVersion = mirror.seenDisplayVersion;
    frame.window       = viewport_.place(SnapshotLines{&mirror.entries}, rows_);
    frame.width        = width_ - 1;   // the scrollbar's column
    frame.hOffset      = hOffset_;
    frame.columns      = columns_;
    frame.needle       = mirror.searchNeedleLower;
    frame.namesSeen    = mirror.moduleNamesById.size();
    auto bar = scrollbarText(rows_, frame.window.first, frame.window.count, mirror.entries.size());
    if(bar != scrollbar) { scrollbar = std::move(bar); }
    if(!screen_.build(frame, mirror.entries, mirror)) { return; }
    auto next = spansOf(screen_.runs());
    if(next != lines->spans) { lines->spans = std::move(next); }
}

std::optional<std::size_t> LogView::current(EntryStore::Mirror const& mirror) const {
    auto const w = viewport_.place(SnapshotLines{&mirror.entries}, rows_);
    if(w.selected) { return w.selected; }
    if(w.count == 0) { return std::nullopt; }
    return w.first + w.count - 1;
}

}   // namespace uc_log::printer::rtx
