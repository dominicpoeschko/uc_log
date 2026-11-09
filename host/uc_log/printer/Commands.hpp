#pragma once

// What a log window can be told to do, for any front end: one table that the key map, the help and the command
// palette all read, so a key, its help line and its palette entry cannot drift apart. A front end maps a key it
// receives (commandForKey) and the special keys it knows itself (arrows, pages) to a Command and runs it.

#include "uc_log/printer/TextMatch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

enum class Command : std::uint8_t {
    // moving around the log (keys; not offered by the palette)
    lineUp,
    lineDown,
    pageUp,
    pageDown,
    shiftLeft,
    shiftRight,
    oldest,
    follow,
    // the log
    search,
    jumpToTime,
    copyLine,
    exportText,
    // panels
    details,
    messages,
    buildOutput,
    help,
    palette,
    closePanel,
    splitPanel,
    settings,
    debugger,
    health,
    inspect,
    // the filter
    filter,
    hideLocation,
    onlyLocation,
    hideModule,
    clearFilter,
    hideNoisy,
    saveFilter,
    loadFilter,
    // metrics
    metrics,
    clearMetrics,
    // columns
    toggleHostTime,
    toggleTargetTime,
    toggleChannel,
    toggleLevel,
    toggleLocation,
    toggleModule,
    toggleFunction,
    // the target and the printer
    clearMessages,
    reset,
    flash,
    build,
    buildAndFlash,
    quit,
};

struct CommandInfo {
    Command          id;
    std::string_view name;   // what the palette matches and shows
    std::string_view keys;   // how the help writes the keys that run it
    char32_t key;   // the character that runs it, 0: none (a special key the front end maps itself)
    bool     palette;   // offered by the command palette
};

inline constexpr std::array commandTable{
  CommandInfo{          Command::lineUp,"line up",      "↑ k",U'k', false                                                                                                    },
  CommandInfo{        Command::lineDown,                                    "line down",      "↓ j", U'j', false},
  CommandInfo{          Command::pageUp,                                      "page up",     "PgUp",    0, false},
  CommandInfo{        Command::pageDown,                                    "page down",     "PgDn",    0, false},
  CommandInfo{       Command::shiftLeft,                       "shift the message left",      "← h", U'h', false},
  CommandInfo{      Command::shiftRight,                      "shift the message right",      "→ l", U'l', false},
  CommandInfo{          Command::oldest,                        "go to the oldest line",   "Home g", U'g',  true},
  CommandInfo{          Command::follow,                       "follow the newest line",    "End G", U'G',  true},
  CommandInfo{          Command::search,                  "search (text or re:pattern)",        "/", U'/',  true},
  CommandInfo{      Command::jumpToTime,                        "jump to a target time",        "T", U'T',  true},
  CommandInfo{        Command::copyLine,                                "copy the line",        "y", U'y',  true},
  CommandInfo{      Command::exportText,               "export the shown lines as text",        "e", U'e',  true},
  CommandInfo{         Command::details,                          "details of the line",  "Enter d", U'd',  true},
  CommandInfo{        Command::messages,                                     "messages",        "s", U's',  true},
  CommandInfo{     Command::buildOutput,                                 "build output",        "o", U'o',  true},
  CommandInfo{            Command::help,                                         "help",        "?", U'?',  true},
  CommandInfo{         Command::palette,                              "command palette", ": Ctrl+P", U':', false},
  CommandInfo{      Command::closePanel,                              "close the panel",      "Esc",    0,  true},
  CommandInfo{      Command::splitPanel,
              "split: the page under the log, or the whole window again",        "z",
              U'z',  true                                                                                       },
  CommandInfo{        Command::settings,
              "settings: columns, log file, sockets, duplex channels",        "4",
              0,  true                                                                                          },
  CommandInfo{        Command::debugger,
              "debugger: the link, the core, the probe, the connection",        "5",
              0,  true                                                                                          },
  CommandInfo{          Command::health, "health: crash records, stacks, raise a panic",        "8",    0,  true},
  CommandInfo{         Command::inspect,       "inspect: trace rings, watches, profile",        "9",    0,  true},
  CommandInfo{          Command::filter,
              "filter: levels, channels, time, modules, locations",        "|",
              U'|',  true                                                                                       },
  CommandInfo{    Command::hideLocation,                     "hide the line's location",        "-", U'-',  true},
  CommandInfo{    Command::onlyLocation,                "show only the line's location",        "+", U'+',  true},
  CommandInfo{      Command::hideModule,                       "hide the line's module",        "M", U'M',  true},
  CommandInfo{     Command::clearFilter,            "clear the filter: show everything",        "=", U'=',  true},
  CommandInfo{       Command::hideNoisy,                     "hide the noisy locations",         "",    0,  true},
  CommandInfo{      Command::saveFilter,                              "save the filter",         "",    0,  true},
  CommandInfo{      Command::loadFilter,                                "load a filter",         "",    0,  true},
  CommandInfo{         Command::metrics,                  "metrics: the values, a plot",        "n", U'n',  true},
  CommandInfo{    Command::clearMetrics,                            "clear the metrics",         "",    0,  true},
  CommandInfo{  Command::toggleHostTime,                            "column: host time",        "t", U't',  true},
  CommandInfo{Command::toggleTargetTime,                          "column: target time",        "u", U'u',  true},
  CommandInfo{   Command::toggleChannel,                              "column: channel",        "c", U'c',  true},
  CommandInfo{     Command::toggleLevel,                                "column: level",        "v", U'v',  true},
  CommandInfo{  Command::toggleLocation,                             "column: location",        "L", U'L',  true},
  CommandInfo{    Command::toggleModule,                               "column: module",        "m", U'm',  true},
  CommandInfo{  Command::toggleFunction,                             "column: function",        "p", U'p',  true},
  CommandInfo{   Command::clearMessages,          "clear the messages and their counts",         "",    0,  true},
  CommandInfo{           Command::reset,                             "reset the target",        "r", U'r',  true},
  CommandInfo{           Command::flash,                             "flash the target",        "f", U'f',  true},
  CommandInfo{           Command::build,                                        "build",        "b", U'b',  true},
  CommandInfo{   Command::buildAndFlash,                              "build and flash",        "F", U'F',  true},
  CommandInfo{            Command::quit,                                         "quit",        "q", U'q',  true},
};

namespace Detail {
    consteval bool commandTableSound() {
        for(std::size_t i = 0; i < commandTable.size(); ++i) {
            // in enum order: infoOf() indexes by the enum
            if(static_cast<std::size_t>(commandTable[i].id) != i) { return false; }
            for(std::size_t j = i + 1; j < commandTable.size(); ++j) {
                if(commandTable[i].key != 0 && commandTable[i].key == commandTable[j].key) {
                    return false;
                }
            }
        }
        return true;
    }
}   // namespace Detail

static_assert(Detail::commandTableSound(),
              "commandTable: in Command's order, and no key twice");
static_assert(commandTable.size() == static_cast<std::size_t>(Command::quit) + 1,
              "commandTable: every Command");

constexpr CommandInfo const& infoOf(Command command) {
    return commandTable[static_cast<std::size_t>(command)];
}

constexpr std::optional<Command> commandForKey(char32_t key) {
    if(key == 0) { return std::nullopt; }
    for(auto const& c : commandTable) {
        if(c.key == key) { return c.id; }
    }
    return std::nullopt;
}

/// The palette's entries for `query`: every word of it (case does not matter) is part of the name; names that start
/// with the first word come first, then those with a word starting with it, then the rest - each group in the
/// table's order. An empty query offers every palette entry.
inline std::vector<Command> matchCommands(std::string_view query) {
    auto const                          words = queryWords(query);
    std::array<std::vector<Command>, 3> ranked;
    for(auto const& c : commandTable) {
        if(!c.palette) { continue; }
        auto const name = asciiLower(c.name);
        if(!containsWords(name, words)) { continue; }
        std::size_t rank = 2;
        if(words.empty() || name.starts_with(words.front())) {
            rank = 0;
        } else if(name.contains(" " + words.front())) {
            rank = 1;
        }
        ranked[rank].push_back(c.id);
    }
    std::vector<Command> out;
    for(auto const& r : ranked) { out.insert(out.end(), r.begin(), r.end()); }
    return out;
}

}   // namespace uc_log::printer
