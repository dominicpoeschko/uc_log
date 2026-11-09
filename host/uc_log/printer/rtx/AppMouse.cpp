// The App: the mouse - a click marks a log row, the scrollbars, a drag on a page's head splits, a right click
// opens the line's menu.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/PanelRows.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

bool App::mouse(::rtxui::Event::Mouse const& m) {
    using Mouse = ::rtxui::Event::Mouse;
    // rows from the top: the status line, the tabs, the log (unless a page has the window), the panel or the page,
    // the mode line. The log and a text panel: a head, then their rows inside a frame (two rows and a column in)
    constexpr std::size_t logTop   = 2;
    auto const            lines    = SnapshotLines{&mirror_.entries};
    auto const            rows     = logRows();
    auto const            body     = logBody();
    auto const            y        = static_cast<std::size_t>(std::max(0, m.y() - 1));
    auto const            x        = static_cast<std::size_t>(std::max(0, m.x() - 1));
    auto const            panelTop = logHidden() ? logTop : logTop + rows;   // its head's row
    // the log's lines: under its head and its frame's top
    bool const inLog  = !logHidden() && y > logTop + 1 && y <= logTop + 1 + body;
    auto const logRow = inLog ? y - logTop - 2 : 0;
    bool const onBar = x + 2 == static_cast<std::size_t>(width_);   // the column left of the border
    bool const inPanel = panel_ != Tab::logs && y >= panelTop && y < panelTop + panelRows();
    bool const inText
      = inPanel && !isPage(panel_) && y > panelTop + 1 && y + 1 < panelTop + panelRows();
    auto const textRow = inText ? y - panelTop - 2 : 0;   // under its head and its frame's top

    // a drag: the page's or panel's top (the log above sizes with it), the log's scrollbar thumb. The mouse is the
    // App's meanwhile: RTXUI's screen would hand the motion to a text selection, not to us
    if(dragging_ || logDrag_) {
        if(m.motion() == Mouse::Released) {
            dragging_ = false;
            logDrag_  = false;
            ReleaseMouse();
            return false;
        }
        if(m.motion() == Mouse::Moved) {
            if(logDrag_) {
                auto const row
                  = std::clamp<long>(static_cast<long>(y) - static_cast<long>(logTop) - 2,
                                     0,
                                     static_cast<long>(body) - 1);
                log->viewport().show(lines,
                                     body,
                                     lines.size() * static_cast<std::size_t>(row)
                                       / std::max<std::size_t>(body, 1));
                return true;
            }
            auto const height
              = height_ - 1 - static_cast<int>(y);   // from that row down to the mode line
            if(height != static_cast<int>(panelRows())) {
                split_       = true;   // a page that had the window goes under the log
                panelHeight_ = height;
            }
            return true;
        }
    }
    if(m.button() == Mouse::WheelUp || m.button() == Mouse::WheelDown) {
        long const delta = m.button() == Mouse::WheelUp ? -3 : 3;
        if(inPanel && panel_ == Tab::metrics) {
            scrollMetrics(
              delta);   // the table scrolls by itself, the plot stays the marked metric's
            return true;
        }
        if(inPanel && isPage(panel_)) { return false; }   // a page scrolls itself
        if(inPanel) {
            panelViews_[static_cast<std::size_t>(panel_)].scroll(IndexLines{panelLines()},
                                                                 panelBody(),
                                                                 delta);
        } else {
            log->viewport().scroll(lines, body, delta);
        }
        return true;
    }
    if(m.motion() != Mouse::Pressed) { return false; }
    if(m.button() == Mouse::Right && inLog) {
        // a right click on a line: marked, and what can be done with it
        log->viewport().selectRow(lines, body, logRow);
        relayout();
        openMenu(m.x(), m.y());
        return true;
    }
    if(m.button() != Mouse::Left) { return false; }
    if(inPanel && y == panelTop) {
        dragging_ = true;
        CaptureMouse();
        return false;   // the press goes on to the title's buttons
    }
    if(inText && onBar) {
        // the panel's scrollbar: that far into its rows
        auto const shown = panelBody();
        auto const total = panelLines();
        panelViews_[static_cast<std::size_t>(panel_)].show(IndexLines{total},
                                                           shown,
                                                           total * textRow / shown);
        return true;
    }
    if(inLog) {
        if(onBar && lines.size() > 0) {
            // the scrollbar: that far into the log, and the thumb follows a drag
            log->viewport().show(lines,
                                 body,
                                 lines.size() * logRow / std::max<std::size_t>(body, 1));
            logDrag_ = true;
            CaptureMouse();
            return true;
        }
        log->viewport().selectRow(lines, body, logRow);
        relayout();
        // the event goes on: RTXUI's own text selection still starts
    }
    return false;   // the tabs, the badges, the hints, the buttons: their own onclick
}

void App::openMenu(int x,
                   int y) {
    auto const index = log->current(mirror_);
    if(!index) { return; }
    auto const& c     = *mirror_.entries[*index].common;
    auto const  where = locationText(
      SourceLocation{mirror_.fileNameOf(c.locationKey), EntryStore::Mirror::lineOf(c.locationKey)});
    std::vector<MenuEntry> entries;
    auto const add = [&](std::string text, std::string key, std::function<void()> action) {
        entries.push_back({std::move(text), std::move(key), std::move(action)});
    };
    auto const command = [this](Command cmd) { return [this, cmd] { run(cmd); }; };
    add("🔎 Details", "d", command(Command::details));
    add("📋 Copy the line", "y", command(Command::copyLine));
    add(fmt::format("🙈 Hide {}", where), "-", command(Command::hideLocation));
    add(fmt::format("🎯 Only {}", where), "+", command(Command::onlyLocation));
    if(auto const module = mirror_.moduleNameOf(c.moduleId); !module.empty()) {
        add(fmt::format("🧩 Hide the module {}", module), "M", command(Command::hideModule));
    }
    add("🕑 Show from this line's time on",
        "",
        [this, seconds = std::chrono::duration<double>(c.ucTime.time).count()] {
            auto w    = timeWindow();
            w.enabled = true;
            w.live    = false;
            w.minSec  = seconds;
            w.maxSec  = std::numeric_limits<double>::infinity();
            if(hooks_.setTime) { hooks_.setTime(w); }
            notify(fmt::format("showing {}", filterSummary(filter_, w)));
        });
    add("🔍 Filter …", "|", command(Command::filter));
    menu->openAt(fmt::format("line {}", *index + 1), std::move(entries), x, y, width_, height_);
}

}   // namespace uc_log::printer::rtx
