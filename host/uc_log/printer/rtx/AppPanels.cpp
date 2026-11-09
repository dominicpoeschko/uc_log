// The App: which tab is open and how it shows - a page of boxes (AppPages.cpp) or a text panel (messages, build
// output, the marked line, help) in its frame, only the rows it shows built.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/PanelText.hpp"
#include "uc_log/printer/rtx/HeadFit.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

std::size_t App::panelLines() const {
    switch(panel_) {
    case Tab::messages:  return messageEntries.size();
    case Tab::build:     return buildEntries.size();
    case Tab::details:   return detailLines_;
    case Tab::help:      return helpRows().size();
    case Tab::filter:
    case Tab::settings:
    case Tab::debugger:
    case Tab::health:
    case Tab::inspect:
    case Tab::metrics:   // a page: it scrolls itself
    case Tab::logs:      return 0;
    }
    return 0;
}

void App::showPanel() {
    bool const text = panel_ != Tab::logs && !isPage(panel_);
    bool const form = isPage(panel_);
    if(textOpen != text) { textOpen = text; }
    if(formOpen != form) { formOpen = form; }
    if(form) { showPage(); }
    if(text) { showTextPanel(); }
}

void App::showTextPanel() {
    showButtons();
    // in its frame, the last column of it the scrollbar
    auto height = fmt::format("height: {};", panelRows());
    if(panelView->height != height) { panelView->height = std::move(height); }
    auto const body  = panelBody();
    auto const width = static_cast<std::size_t>(
      std::max(width_ - 4, 1));   // the frame's borders, the scrollbar, the gap on the left
    auto&                  shown = panelViews_[static_cast<std::size_t>(panel_)];
    std::string            title;
    std::string            hint;
    std::string            legend;
    std::vector<ScreenRun> runs;
    switch(panel_) {
    case Tab::messages:
        {
            auto const w = shown.place(IndexLines{messageEntries.size()}, body);
            auto const part
              = std::span<MessageLog::Entry const>{messageEntries}.subspan(w.first, w.count);
            runs  = messageEntries.empty()
                    ? panelRuns(
                        std::vector<PanelRow>{{label(
                          "no message yet: what the printer does and what goes wrong comes here")}},
                        0,
                        1,
                        width,
                        RtxuiWidth{})
                    : panelRuns(messageRows(part), 0, w.count, width, RtxuiWidth{});
            title = fmt::format("💬 Messages · {}", messageEntries.size());
            hint  = " · what the printer says, the newest at the bottom";
            legend = "📜 The printer's messages";
            break;
        }
    case Tab::build:
        {
            auto const w    = shown.place(IndexLines{buildEntries.size()}, body);
            auto const part = std::span<BuildEntry const>{buildEntries}.subspan(w.first, w.count);
            runs = buildEntries.empty()
                   ? panelRuns(std::vector<PanelRow>{{label(
                                 "no build yet: ▶ Build (b) builds, ⏩ Build + flash (F) "
                                 "builds and flashes")}},
                               0,
                               1,
                               width,
                               RtxuiWidth{})
                   : panelRuns(buildRows(part), 0, w.count, width, RtxuiWidth{});
            std::string_view state;
            switch(buildStatus) {
            case BuildStatus::idle:    state = "idle"; break;
            case BuildStatus::running: state = "running"; break;
            case BuildStatus::success: state = "done"; break;
            case BuildStatus::failed:  state = "FAILED"; break;
            }
            title  = fmt::format("🔨 Build · {} · {} lines", state, buildEntries.size());
            hint   = " · the build command's output";
            legend = "📜 The last build's output";
            break;
        }
    case Tab::details:
        {
            auto const index = log->current(mirror_);
            if(index) {
                auto const all = detailRows(mirror_.entries[*index],
                                            mirror_,
                                            log->columns().text,
                                            width,
                                            RtxuiWidth{});
                detailLines_   = all.size();
                auto const w   = shown.place(IndexLines{all.size()}, body);
                runs           = panelRuns(all, w.first, w.count, width, RtxuiWidth{});
                title          = fmt::format("🔎 Line · {}", *index + 1);
            } else {
                detailLines_ = 0;
                title        = "🔎 Line · no line yet";
            }
            hint   = " · the marked line, all it holds";
            legend = "📜 Its fields";
            break;
        }
    case Tab::help:
        {
            auto const all = helpRows();
            auto const w   = shown.place(IndexLines{all.size()}, body);
            runs           = panelRuns(all, w.first, w.count, width, RtxuiWidth{});
            title          = "❓ Help";
            hint           = " · : runs any command by its name, the wheel scrolls";
            legend         = "⌨ Keys and commands";
            break;
        }
    case Tab::logs:
    case Tab::filter:
    case Tab::settings:
    case Tab::debugger:
    case Tab::metrics:
    case Tab::health:
    case Tab::inspect:  break;
    }
    auto const total = panelLines();
    auto const w     = shown.place(IndexLines{total}, body);
    auto       bar   = scrollbarText(body, w.first, w.count, total);
    if(panelView->scrollbar != bar) { panelView->scrollbar = std::move(bar); }
    if(panelView->title != title) { panelView->title = std::move(title); }
    hint = fitHint(panelView->title, std::move(hint), panelView->buttons, width_);
    if(panelView->hint != hint) { panelView->hint = std::move(hint); }
    if(panelView->legend != legend) { panelView->legend = std::move(legend); }
    auto spans = spansOf(runs);
    if(panelView->spans != spans) { panelView->spans = std::move(spans); }
}

void App::togglePanel(Tab kind) {
    if(panel_ == kind) {
        openPanel(Tab::logs);
        return;
    }
    openPanel(kind);
}

void App::openPanel(Tab kind) {
    if(kind == Tab::logs) {
        panel_ = Tab::logs;   // split or not, the log has the window again
        return;
    }
    if(panel_ == kind) { return; }
    panel_ = kind;
    if(kind == Tab::debugger && !pages_.probesAsked) {
        // the probes on USB: listed the first time the page opens (the reader asks the DLL only on request)
        pages_.probesAsked = true;
        act(WindowAction{action::ListProbes{}});
    }
    auto& pv = panelViews_[static_cast<std::size_t>(kind)];
    if(kind == Tab::details || kind == Tab::help || kind == Tab::settings || kind == Tab::debugger
       || kind == Tab::health || kind == Tab::inspect)
    {
        pv = LogViewport{};
        pv.home(IndexLines{1});   // from the top
    } else {
        pv.follow();   // the newest at the bottom
    }
}

}   // namespace uc_log::printer::rtx
