#include "uc_log/printer/rtx/ContextMenu.hpp"

#include "uc_log/printer/rtx/Runs.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void Fields(MenuItem const&     item,
            ::rtxui::FieldSink& out) {
    out("text", item.text);
    out("key", item.key);
}

ContextMenu::ContextMenu() {
    Import("open", &ContextMenu::open);
    Import("place", &ContextMenu::place);
    Import("title", &ContextMenu::title);
    Import("items", &ContextMenu::items);
    Import("area", &ContextMenu::area);
    Import("Pick", [this](std::string const& arg) {
        auto const i = argIndex(arg);
        open         = false;
        if(i >= actions_.size()) { return; }
        auto const action = actions_[i];
        action();
        if(changed) { changed(); }
    });
    Import("Close", [this] { open = false; });   // a click (either button) beside the menu
}

void ContextMenu::openAt(std::string            heading,
                         std::vector<MenuEntry> entries,
                         int                    x,
                         int                    y,
                         int                    width,
                         int                    height) {
    items.clear();
    actions_.clear();
    for(auto& e : entries) {
        items.push_back({std::move(e.text), std::move(e.key)});
        actions_.push_back(std::move(e.action));
    }
    // where the mouse is, as far as it fits: 40 cells wide, a row an item, the frame and the title
    constexpr int menuWidth = 40;
    auto const    menuRows  = static_cast<int>(items.size()) + 3;
    auto const    left      = std::clamp(x - 1, 0, std::max(0, width - menuWidth));
    auto const    top       = std::clamp(y - 1, 0, std::max(0, height - menuRows));
    place                   = fmt::format("top: {}; left: {};", top, left);
    area                    = fmt::format("width: {}; height: {};", width, height);
    title                   = std::move(heading);
    open                    = true;
}

}   // namespace uc_log::printer::rtx
