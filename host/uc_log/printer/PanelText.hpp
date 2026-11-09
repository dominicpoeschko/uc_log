#pragma once

// What a log window's panels show, for any front end: the messages, the build output, the details of one line and
// the help as rows (PanelRows.hpp), each cut or wrapped to the panel's width; a line as plain text (copy, export). A
// panel's rows are built only for what it shows, like the log's (LogScreen.hpp).

#include "uc_log/LogLevel.hpp"
#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LineText.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/RowText.hpp"
#include "uc_log/printer/Tabs.hpp"
#include "uc_log/printer/TimeText.hpp"
#include "uc_log/printer/Tone.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

inline Tone toneOf(MessageLevel level) {
    switch(level) {
    case MessageLevel::fatal:
    case MessageLevel::error:     return Tone::bad;
    case MessageLevel::status:    return Tone::plain;
    case MessageLevel::toolError: return Tone::warn;
    case MessageLevel::tool:      return Tone::info;
    }
    return Tone::plain;
}

/// The messages panel's rows for `entries` (the ones it shows): time, level, text (its own colours), one row a message.
inline std::vector<PanelRow> messageRows(std::span<MessageLog::Entry const> entries) {
    std::vector<PanelRow> rows;
    rows.reserve(entries.size());
    for(auto const& e : entries) {
        PanelRow row;
        row.push_back({RunKind::hostTime, 0, {}, localTimeOfDay(e.time) + ' '});
        row.push_back(toned(toneOf(e.level), fmt::format("{:<10} ", wireName(e.level))));
        for(auto& run : ansiRuns(e.text)) {
            row.push_back({RunKind::message, 0, run.style, std::move(run.text)});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// The build panel's rows for `entries` (the ones it shows): time and line, an error red, the tool's own lines in its
/// colour.
inline std::vector<PanelRow> buildRows(std::span<BuildEntry const> entries) {
    std::vector<PanelRow> rows;
    rows.reserve(entries.size());
    for(auto const& e : entries) {
        PanelRow row;
        row.push_back({RunKind::hostTime, 0, {}, localTimeOfDay(e.time) + ' '});
        auto const tone = e.isError ? Tone::bad : (e.fromTool ? Tone::info : Tone::plain);
        bool       any  = false;
        // a progress line rewritten in place (ninja's "\r[3/7] ..."): what a terminal shows of it, the last state
        std::string_view line = e.line;
        if(auto const cr = line.find_last_of('\r');
           cr != std::string_view::npos && cr + 1 < line.size())
        {
            line.remove_prefix(cr + 1);
        }
        for(auto& run : ansiRuns(line)) {
            any = true;
            if(run.style.plain()) {
                row.push_back(toned(tone, std::move(run.text)));
            } else {
                row.push_back({RunKind::message, 0, run.style, std::move(run.text)});
            }
        }
        if(!any) { row.push_back(toned(tone, "")); }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// The details of one line: its whole message (every line of it) first, then where it comes from (location,
/// function, module) and what it is (level, channel, the two times), wrapped to `width`.
template<typename CellWidth>
std::vector<PanelRow> detailRows(StoredLine const&         line,
                                 EntryStore::Mirror const& names,
                                 ShownTextOptions const&   text,
                                 std::size_t               width,
                                 CellWidth const&          cells) {
    auto const&           c = *line.common;
    std::vector<PanelRow> rows;
    auto                  field = [&](std::string_view name, std::vector<PanelPart> value) {
        std::vector<PanelPart> parts{label(fmt::format("{:<10}", name))};
        parts.insert(parts.end(), value.begin(), value.end());
        for(auto& r : wrapParts(parts, width, 10, cells)) { rows.push_back(std::move(r)); }
    };
    // the message first: what a person opens the details for
    std::string_view message = c.message;
    bool             first   = true;
    while(true) {
        auto const             end = message.find('\n');
        std::vector<PanelPart> parts;
        for(auto& run : ansiRuns(shownText(message.substr(0, end), text))) {
            parts.push_back({RunKind::message, 0, run.style, std::move(run.text)});
        }
        field(first ? "message" : "", std::move(parts));
        first = false;
        if(end == std::string_view::npos) { break; }
        message.remove_prefix(end + 1);
    }
    field("location",
          {
            {RunKind::location,
             0, {},
             fmt::format("{}:{}",
             names.fileNameOf(c.locationKey),
             EntryStore::Mirror::lineOf(c.locationKey))}
    });
    field("function",
          {
            {RunKind::function, 0, {}, names.functionSignatureOf(c.functionId)}
    });
    auto const module = names.moduleNameOf(c.moduleId);
    if(!module.empty()) {
        field("module",
              {
                {RunKind::module, 0, {}, module}
        });
    }
    field("line",
          {
            {RunKind::level,
             static_cast<std::uint8_t>(c.logLevel),
             {},
             std::string{levelName(c.logLevel)}},
            label(" · channel "),
            {RunKind::channel, c.channel, {}, fmt::format("{}", c.channel)},
            label(" · target "),
            {RunKind::targetTime, 0, {}, fmt::format("{}", c.ucTime)},
            label(" · host "),
            {RunKind::hostTime, 0, {}, localTimeOfDay(c.recvTime)},
            {RunKind::unparsed,
             0, {},
             c.parsedOk ? "" : " · ⚠ unparsed: the parser gave up, the fields are defaults"}
    });
    return rows;
}

/// The help: every command with its keys, as the command table has them.
inline std::vector<PanelRow> helpRows() {
    std::vector<PanelRow> rows;
    rows.push_back({label(
      "keys and commands - : runs any command by name; the mouse: a click on a tab, a badge, a "
      "button or a hint runs it, the wheel scrolls, a drag on a panel's title sizes it")});
    for(auto const& t : tabTable) {
        auto const& opens = infoOf(t.opens);
        // the tab's digit, then the command's keys unless they are the digit again
        std::string keys;
        if(t.key != 0) { keys = std::string(1, static_cast<char>(t.key)); }
        if(!opens.keys.empty() && opens.keys != keys) {
            keys += (keys.empty() ? "" : " ") + std::string{opens.keys};
        }
        rows.push_back({
          toned(Tone::warn, fmt::format("  {:<10} ", keys)),
          PanelPart{RunKind::message, 0, {}, fmt::format("tab {} {}", t.icon, t.name)}
        });
    }
    for(auto const& c : commandTable) {
        rows.push_back({
          toned(Tone::warn, fmt::format("  {:<10} ", c.keys.empty() ? "(palette)" : c.keys)),
          PanelPart{RunKind::message, 0, {}, std::string{c.name}}
        });
    }
    return rows;
}

/// A line as plain text: "<host time> <channel> <target time> <level>| <text> ([module] <file>:<line> <function>)",
/// its colours and markers taken out - what a copy holds and an export writes.
inline std::string plainText(StoredLine const&         line,
                             EntryStore::Mirror const& names,
                             ShownTextOptions const&   text,
                             bool                      padLevel) {
    auto const& c = *line.common;
    std::string shown;
    for(auto const& run : ansiRuns(shownText(line.lineText(), text))) { shown += run.text; }
    auto const module = names.moduleNameOf(c.moduleId);
    auto const prefix = module.empty() ? std::string{} : fmt::format("[{}] ", module);
    auto const recv   = localTimeOfDay(c.recvTime);
    auto const uc     = fmt::format("{}", c.ucTime);
    auto const file   = names.fileNameOf(c.locationKey);
    auto const fn     = names.functionNameOf(c.functionId);
    return plainRow(RowFields{.recvTime     = recv,
                              .channel      = c.channel,
                              .ucTime       = uc,
                              .level        = levelName(c.logLevel),
                              .text         = shown,
                              .modulePrefix = prefix,
                              .file         = file,
                              .line         = EntryStore::Mirror::lineOf(c.locationKey),
                              .function     = fn},
                    padLevel);
}

}   // namespace uc_log::printer
