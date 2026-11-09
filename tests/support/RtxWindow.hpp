#pragma once
// The RTXUI window's App for a test, drawn by RTXUI without a terminal: a store of numbered lines, the App on it with
// hooks that count what the window asked for, the screen's text, and clicks and mouse reports on that text. Text
// checks only; the cost is counted (rows formatted), never timed; no clock is read - RTXUI's TimelineScreen, whose
// clock moves only when told (Screen below; HeadlessScreen waits out a button's hover transition on the real clock).
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/Environment.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LogFilterState.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/WindowActions.hpp"
#include "uc_log/printer/rtx/App.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <rtxui/headless.hpp>
#include <rtxui/task.hpp>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <variant>
#include <vector>

namespace uc_log::test::rtx_window {

using namespace uc_log::printer;

inline LogEntry entry(std::string              msg,
                      std::chrono::nanoseconds uc = std::chrono::nanoseconds{}) {
    LogEntry e{1, {}};
    e.logLevel     = uc_log::LogLevel::info;
    e.fileName     = "main.cpp";
    e.line         = 7;
    e.logMsg       = std::move(msg);
    e.functionName = "fn";
    e.ucTime.time  = uc;
    e.parsedOk     = true;
    return e;
}

inline std::vector<std::string> rowsOf(std::string const& text) {
    std::vector<std::string> rows;
    std::size_t              start = 0;
    while(start <= text.size()) {
        auto const end = text.find('\n', start);
        rows.push_back(
          text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if(end == std::string::npos) { break; }
        start = end + 1;
    }
    return rows;
}

inline bool contains(std::string const& text,
                     std::string_view   part) {
    return text.contains(part);
}

// RTXUI's TimelineScreen as the tests use a screen: each input handled, then its clock moved on until what the input
// started is over - a button's hover transition (0.1 s), a scrollbar's (0.2 s), a lone Escape that might begin a key
// sequence - on the screen's own clock, never the real one
class Screen {
public:
    Screen(::rtxui::Ref<::rtxui::ComponentBase> const& app,
           int                                         width,
           int                                         height)
      : timeline_{app,
                  width,
                  height} {}

    void input(std::string_view bytes) {
        timeline_.Input(bytes);
        settle();
    }

    void click(int x,
               int y) {
        timeline_.Click(x, y);
        settle();
    }

    void resize(int width,
                int height) {
        timeline_.Resize(width, height);
        settle();
    }

    [[nodiscard]] std::string text() const { return timeline_.Text(); }

private:
    ::rtxui::TimelineScreen timeline_;

    // what the test changed beside the input (lines, badges) is drawn as the window's wake-up task draws it; then
    // 250 ms of the screen's clock: every transition and an Escape's wait are over
    void settle() {
        ::rtxui::PostTask([] {});
        timeline_.Advance(250);
    }
};

struct Window {
    EntryStore                              store;
    EntryStore::Mirror                      mirror;
    std::string                             searched;
    int                                     flashes{0};
    int                                     exports{0};
    int                                     builds{0};
    int                                     filters{0};
    FilterState                             lastFilter;
    std::optional<EntryStore::UcTimeWindow> lastTime;
    std::vector<WindowAction>               acts;   // what the pages asked the printer for
    ::rtxui::Ref<rtx::App>                  app;
    std::unique_ptr<Screen>                 screen;

    explicit Window(std::size_t lines) {
        for(std::size_t i = 0; i < lines; ++i) {
            store.addEntry(std::chrono::system_clock::time_point{},
                           entry("line " + std::to_string(i), std::chrono::seconds{i}));
        }
        store.refreshMirror(mirror);
        app = ::rtxui::Ref<rtx::App>::New(
          mirror,
          rtx::App::Hooks{.quit          = {},
                          .reset         = {},
                          .flash         = [this] { ++flashes; },
                          .build         = [this](bool) { ++builds; },
                          .search        = [this](std::string const& text) { searched = text; },
                          .exportText    = [this] { ++exports; },
                          .clearMessages = {},
                          .setFilter =
                            [this](FilterState const& f) {
                                lastFilter = f;
                                ++filters;
                            },
                          .setTime = [this](EntryStore::UcTimeWindow const& t) { lastTime = t; },
                          .later   = {},
                          .act     = [this](WindowAction const& a) { acts.push_back(a); },
                          .complete =
                            [](std::string const& query, std::size_t) {
                                TargetInspector::Completions c;
                                if(std::string_view{"counter"}.contains(query)) {
                                    c.best.push_back({"counter", 0x2000'0000, 4});
                                    c.total = 1;
                                }
                                return c;
                            }});
        app->setMetricSource(&store);
        app->setSize(80, 12);
        app->log->columns().hostTime   = false;   // the host's clock: not in a text check
        app->log->columns().targetTime = false;
        frame();
        screen = std::make_unique<Screen>(app, 80, 12);
    }

    // a bigger window: RTXUI lays it out anew, the App is told as the window's size poll tells it
    void resize(int width,
                int height) {
        screen->resize(width, height);
        app->setSize(width, height);
        screen->input("");
    }

    void add(std::string                           msg,
             std::chrono::system_clock::time_point received = {}) {
        store.addEntry(received, entry(std::move(msg)));
    }

    void frame(std::size_t errors = 0) {
        store.refreshMirror(mirror);
        StatusFacts facts;
        facts.logs   = mirror.originalLogCount;
        facts.errors = errors;
        app->show(statusBadges(facts));
    }

    // UC_LOG_TEST_SCREEN=1: every screen a check reads, printed (what a failing check saw)
    [[nodiscard]] std::string text() const {
        auto t = screen->text();
        if(uc_log::printer::environment("UC_LOG_TEST_SCREEN") != nullptr) {
            std::printf("----\n%s\n", t.c_str());
        }
        return t;
    }
};

// the cell (0-based) where `part` starts in `row`, measured as RTXUI lays text out (an emoji takes two)
inline std::size_t cellOf(std::string const& row,
                          std::string_view   part) {
    auto const at = row.find(part);
    return rtx::cellWidth(
      std::string_view{row}.substr(0, at == std::string::npos ? row.size() : at));
}

// a click on the first cell of `part` in the first row that holds it; false: no row does
inline bool clickText(Window&          w,
                      std::string_view part) {
    auto const rows = rowsOf(w.text());
    for(std::size_t r = 0; r < rows.size(); ++r) {
        if(contains(rows[r], part)) {
            w.screen->click(static_cast<int>(cellOf(rows[r], part)), static_cast<int>(r));
            return true;
        }
    }
    return false;
}

// the last action the pages asked the printer for, if it is an `A`
template<typename A>
A const* lastAct(Window const& w) {
    return w.acts.empty() ? nullptr : std::get_if<A>(&w.acts.back());
}

// a terminal's SGR mouse report: button 0 (left), 32 (left held, moving); 1-based cells; M pressed or moved, m up
inline std::string sgrMouse(int  button,
                            int  x,
                            int  y,
                            bool up = false) {
    return "\x1b[<" + std::to_string(button) + ";" + std::to_string(x + 1) + ";"
         + std::to_string(y + 1) + (up ? "m" : "M");
}

// the row of `rows` that holds `part`, if any
inline std::optional<std::size_t> rowOf(std::vector<std::string> const& rows,
                                        std::string_view                part) {
    for(std::size_t r = 0; r < rows.size(); ++r) {
        if(contains(rows[r], part)) { return r; }
    }
    return std::nullopt;
}

}   // namespace uc_log::test::rtx_window
