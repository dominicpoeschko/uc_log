#include "uc_log/printer/rtx/TabBar.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

TabBar::TabBar() {
    Import("tabs", &TabBar::tabs);
    Import("Pick", [this](std::string const& arg) {
        auto const i = argIndex(arg);
        if(i < tabTable.size() && picked) { picked(tabTable[i].id); }
    });
}

void TabBar::show(Tab         open,
                  std::size_t width) {
    // what fits: icon and name; the names (the open tab keeps its icon); the icons (the open tab keeps its name)
    enum class Fit : std::uint8_t { full, names, icons };
    auto const chips = [&](Fit fit) {
        std::vector<Chip> out;
        out.reserve(tabTable.size());
        for(auto const& t : tabTable) {
            bool const  on = t.id == open;
            std::string text;
            if(on || fit == Fit::full) {
                text = fmt::format(" {} {} ", t.icon, t.name);
            } else if(fit == Fit::names) {
                text = fmt::format(" {} ", t.name);
            } else {
                text = fmt::format(" {} ", t.icon);
            }
            out.push_back(
              {on ? "on" : "",
               t.key == 0 ? std::string{" "} : fmt::format(" {}", static_cast<char>(t.key)),
               std::move(text)});
        }
        return out;
    };
    auto const cells = [](std::vector<Chip> const& c) {
        std::size_t n = 0;
        for(auto const& chip : c) {
            n += cellWidth(chip.key) + cellWidth(chip.text) + 1;
        }   // a cell between
        return n;
    };
    auto next = chips(Fit::full);
    if(cells(next) > width) { next = chips(Fit::names); }
    if(cells(next) > width) { next = chips(Fit::icons); }
    if(next != tabs) { tabs = std::move(next); }
}

}   // namespace uc_log::printer::rtx
