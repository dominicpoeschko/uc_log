#pragma once

// The window's tabs, for any front end: what each shows, the key that picks it and the command that opens it. A tab
// other than the log is a panel under the log (or the whole window, zoomed). The keys: 1 the log, 2 the build, 3 the
// filter, 4 the settings, 5 the debugger, 6 the metrics, 7 the messages, 8 the health, 9 inspect, 0 the help; a tab
// without a key (the marked line's details) opens by its command alone.

#include "uc_log/printer/Commands.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace uc_log::printer {

enum class Tab : std::uint8_t {
    logs,
    build,
    filter,
    settings,
    debugger,
    metrics,
    messages,
    health,
    inspect,
    details,
    help
};

struct TabInfo {
    Tab              id;
    std::string_view icon;   // what a narrow tab bar shows of a tab that is not open
    std::string_view name;
    char32_t         key;     // the character that picks it, 0: none
    Command          opens;   // the command that opens it (or, run again, closes it)
};

inline constexpr std::array tabTable{
  TabInfo{    Tab::logs, "📄",     "Logs", U'1',  Command::closePanel},
  TabInfo{   Tab::build, "🔨",    "Build", U'2', Command::buildOutput},
  TabInfo{  Tab::filter, "🔍",   "Filter", U'3',      Command::filter},
  TabInfo{Tab::settings, "🔧", "Settings", U'4',    Command::settings},
  TabInfo{Tab::debugger, "🐛", "Debugger", U'5',    Command::debugger},
  TabInfo{ Tab::metrics, "📈",  "Metrics", U'6',     Command::metrics},
  TabInfo{Tab::messages, "💬", "Messages", U'7',    Command::messages},
  TabInfo{  Tab::health, "🩺",   "Health", U'8',      Command::health},
  TabInfo{ Tab::inspect, "🔬",  "Inspect", U'9',     Command::inspect},
  TabInfo{ Tab::details, "🔎",     "Line",    0,     Command::details},
  TabInfo{    Tab::help, "❓",     "Help", U'0',        Command::help},
};

inline constexpr std::size_t tabCount = tabTable.size();

namespace Detail {
    consteval bool tabTableSound() {
        for(std::size_t i = 0; i < tabTable.size(); ++i) {
            if(static_cast<std::size_t>(tabTable[i].id) != i) { return false; }
            if(tabTable[i].key != 0 && commandForKey(tabTable[i].key)) { return false; }
            for(std::size_t j = i + 1; j < tabTable.size(); ++j) {
                if(tabTable[i].key != 0 && tabTable[i].key == tabTable[j].key) { return false; }
            }
        }
        return true;
    }
}   // namespace Detail

static_assert(Detail::tabTableSound(),
              "tabTable: in Tab's order, no key twice and none a command's");
static_assert(tabCount == static_cast<std::size_t>(Tab::help) + 1,
              "tabTable: every Tab");

constexpr TabInfo const& infoOf(Tab tab) { return tabTable[static_cast<std::size_t>(tab)]; }

/// A page of boxes (filter, settings, debugger, metrics, health, inspect) - not a text panel, a list that scrolls
/// (messages, build output, the line, help) - and not the log.
constexpr bool isPage(Tab tab) {
    return tab == Tab::filter || tab == Tab::settings || tab == Tab::debugger || tab == Tab::metrics
        || tab == Tab::health || tab == Tab::inspect;
}

constexpr std::optional<Tab> tabForKey(char32_t key) {
    if(key == 0) { return std::nullopt; }
    for(auto const& t : tabTable) {
        if(t.key == key) { return t.id; }
    }
    return std::nullopt;
}

}   // namespace uc_log::printer
