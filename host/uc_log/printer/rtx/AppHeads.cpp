// The App: every tab's head - its tools (the tab's pages, its own actions, split and close, in groups) as they
// fit, its hint if it fits whole; the log's head the same.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/rtx/HeadFit.hpp"

#include <array>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void App::addButton(std::vector<Chip>&    buttons,
                    std::string           text,
                    std::string_view      key,
                    bool                  on,
                    std::function<void()> action) {
    buttons.push_back(
      {on ? "on" : "", key.empty() ? std::string{} : fmt::format(" {}", key), std::move(text)});
    buttonActions_.push_back(std::move(action));
}

void App::showButtons() {
    // every head the same: the tab's pages, its own actions, then the window's (split, close) - each group after a gap
    std::vector<Chip> buttons;
    buttonActions_.clear();
    auto const command = [this](Command c) { return [this, c] { run(c); }; };
    if(panel_ == Tab::inspect) {
        static constexpr std::array<std::pair<InspectPage, std::string_view>, 3> pages{
          {{InspectPage::trace, "🧵 Trace"},
           {InspectPage::watch, "👁 Watch"},
           {InspectPage::profile, "⏱ Profile"}}
        };
        for(auto const& [which, name] : pages) {
            addButton(buttons, std::string{name}, "", pages_.inspect == which, [this, which] {
                pages_.inspect = which;
            });
        }
    }
    auto const actionsFrom = buttons.size();
    switch(panel_) {
    case Tab::build:
        addButton(buttons, "▶ Build", "b", false, command(Command::build));
        addButton(buttons, "⏩ Build + flash", "F", false, command(Command::buildAndFlash));
        addButton(buttons, "⚡ Flash", "f", false, command(Command::flash));
        addButton(buttons, "🔄 Reset", "r", false, command(Command::reset));
        break;
    case Tab::messages:
        addButton(buttons, "🗑 Clear", "", false, command(Command::clearMessages));
        break;
    case Tab::details:
        addButton(buttons, "📋 Copy", "y", false, command(Command::copyLine));
        addButton(buttons, "🙈 Hide", "-", false, command(Command::hideLocation));
        addButton(buttons, "🎯 Only", "+", false, command(Command::onlyLocation));
        addButton(buttons, "🧩 Module", "M", false, command(Command::hideModule));
        break;
    case Tab::logs:
    case Tab::filter:
    case Tab::metrics:
    case Tab::settings:
    case Tab::debugger:
    case Tab::health:
    case Tab::inspect:
    case Tab::help:     break;
    }
    if(isPage(panel_)) {
        for(std::size_t i = 0; i < page->formTools().size(); ++i) {
            buttons.push_back(page->formTools()[i]);
            buttonActions_.emplace_back([this, i] { page->pressFormTool(i); });
        }
    }
    auto const windowFrom = buttons.size();
    addButton(buttons, split_ ? "▣ Full" : "▤ Split", "z", false, command(Command::splitPanel));
    addButton(buttons, "✕", "Esc", false, command(Command::closePanel));
    if(actionsFrom > 0 && actionsFrom < windowFrom) { buttons[actionsFrom].cls += " gap"; }
    if(windowFrom > 0) { buttons[windowFrom].cls += " gap"; }
    fitTools(buttons, width_);
    if(isPage(panel_)) {
        if(page->tools != buttons) { page->tools = std::move(buttons); }
    } else if(panelView->buttons != buttons) {
        panelView->buttons = std::move(buttons);
    }
}

void App::showLogHead() {
    // the log's head as every tab's: what it shows, a hint, its tools (the one that is on filled)
    std::vector<Chip> tools;
    logActions_.clear();
    auto const add = [&](std::string text, std::string_view key, bool on, Command c) {
        tools.push_back(
          {on ? "on" : "", key.empty() ? std::string{} : fmt::format(" {}", key), std::move(text)});
        logActions_.emplace_back([this, c] { run(c); });
    };
    auto const place = log->viewport().place(SnapshotLines{&mirror_.entries}, logBody());
    add("🔎 Search", "/", !mirror_.searchText.empty(), Command::search);
    add("🔍 Filter", "|", false, Command::filter);
    add("📋 Copy", "y", false, Command::copyLine);
    add("💾 Export", "e", false, Command::exportText);
    add("▼ Follow", "G", place.follow, Command::follow);
    fitTools(tools, width_);
    if(log->tools != tools) { log->tools = std::move(tools); }
    auto const  hidden = filterSummary(filter_, timeWindow());
    std::string title
      = fmt::format("📄 Logs · {}", hidden.empty() ? std::string{"everything shown"} : hidden);
    if(log->title != title) { log->title = std::move(title); }
    auto hint
      = fitHint(log->title, " · a click marks a line, a right click: its menu", log->tools, width_);
    if(log->hint != hint) { log->hint = std::move(hint); }
}

}   // namespace uc_log::printer::rtx
