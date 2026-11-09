#pragma once

// The visible part of a log view as styled runs, for any front end that draws text: exactly the rows of a
// LogViewport window, each `width` cells wide - host time, channel, target time, level, the message (its own ANSI
// runs, cut with "…" or shifted by a horizontal offset), and the module / function / location at the right edge on a
// group's last row. A front end only maps a run's kind and style to its colours; it never formats a line itself.
//
// Cells are measured by the front end's own width function (the one its renderer lays text out with), so the
// padding puts the location exactly at the right edge. Only the rows of the window are formatted, and only when what
// they show changed: `formattedRows()` counts them for the tests, which count work instead of timing it.

#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LineText.hpp"
#include "uc_log/printer/LogViewport.hpp"
#include "uc_log/printer/TimeText.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

enum class RunKind : std::uint8_t {
    hostTime,
    channel,   // detail: the channel
    targetTime,
    level,       // detail: the LogLevel
    separator,   // "| " after the level, the gaps
    unparsed,    // "⚠ unparsed ": a line the parser gave up on
    message,     // style: the message's own ANSI style
    cut,         // the "…" of a cut message
    module,
    function,
    location,
    toned,    // panels: detail = the Tone
    label,    // panels: a field's name
    button,   // panels: a click does something (ScreenRun::action); detail: PanelText's buttonDetail
};

struct ScreenRun {
    RunKind       kind{RunKind::message};
    std::uint8_t  detail{};
    bool          selected{};   // on the marked row
    bool          match{};      // a message run of a row the search matches
    AnsiStyle     style;
    std::string   text;       // a row's last run ends with '\n', except the last row's
    std::uint16_t action{};   // panels: 0 none, i + 1 what a click does (PanelBody::actions[i])
    bool          operator==(ScreenRun const&) const = default;
};

struct LogColumns {
    bool             hostTime{true};
    bool             channel{true};
    bool             targetTime{true};
    bool             level{true};
    bool             location{true};
    bool             function{false};
    bool             module{false};
    ShownTextOptions text{};

    bool operator==(LogColumns const&) const = default;
};

inline LineId lineIdOf(StoredLine const& line) {
    return {line.multilineGroupId(), line.lineOffset};
}

/// EntryChunkList's snapshot as LogViewport's Lines.
struct SnapshotLines {
    EntryChunkList::Snapshot const* lines{};

    [[nodiscard]] std::size_t size() const { return lines->size(); }

    [[nodiscard]] LineId idAt(std::size_t i) const { return lineIdOf((*lines)[i]); }
};

inline std::string_view levelName(uc_log::LogLevel level) {
    static constexpr std::array<std::string_view, 6> names{"trace",
                                                           "debug",
                                                           "info",
                                                           "warn",
                                                           "error",
                                                           "crit"};
    auto const                                       i = static_cast<std::size_t>(level);
    return i < names.size() ? names[i] : names[0];
}

namespace Detail {
    // byte offsets of the code point starts of `text`, and its end
    inline std::vector<std::size_t> codePointBounds(std::string_view text) {
        std::vector<std::size_t> bounds;
        bounds.reserve(text.size() + 1);
        for(std::size_t i = 0; i < text.size(); ++i) {
            if((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U) { bounds.push_back(i); }
        }
        bounds.push_back(text.size());
        return bounds;
    }
}   // namespace Detail

/// The longest code-point prefix of `text` at most `cells` wide: its byte length and its width.
template<typename CellWidth>
std::pair<std::size_t,
          std::size_t>
prefixWithin(std::string_view text,
             std::size_t      cells,
             CellWidth const& width) {
    auto const whole = width(text);
    if(whole <= cells) { return {text.size(), whole}; }
    auto const  bounds = Detail::codePointBounds(text);
    std::size_t lo     = 0;   // index into bounds: width(prefix(bounds[lo])) <= cells
    std::size_t hi     = bounds.size() - 1;
    while(lo + 1 < hi) {
        auto const mid = lo + (hi - lo) / 2;
        if(width(text.substr(0, bounds[mid])) <= cells) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return {bounds[lo], width(text.substr(0, bounds[lo]))};
}

template<typename CellWidth>
class LogScreen {
public:
    explicit LogScreen(CellWidth width) : width_{std::move(width)} {}

    /// What the runs are made of: the frame is formatted again only when one of these changed.
    struct Frame {
        std::uint64_t       storeVersion{};   // EntryStore::Mirror::seenDisplayVersion
        LogViewport::Window window;
        std::size_t         width{};
        std::size_t         hOffset{};
        LogColumns          columns;
        std::string         needle;   // the search, lower case ("" none)
        std::size_t
          namesSeen{};   // file/function/module names known, a name arriving changes a row

        bool operator==(Frame const&) const = default;
    };

    /// The runs of `frame` over `lines` (the mirror's entries), `names` for file, function and module names. False:
    /// the same frame as last time, `runs` as it was.
    bool build(Frame const&                    frame,
               EntryChunkList::Snapshot const& lines,
               EntryStore::Mirror const&       names) {
        if(built_ && frame == last_) { return false; }
        last_  = frame;
        built_ = true;
        runs_.clear();
        auto const& w = frame.window;
        for(std::size_t i = w.first; i < w.first + w.count && i < lines.size(); ++i) {
            row(lines[i], frame, names, w.selected == i);
            if(i + 1 < w.first + w.count && !runs_.empty()) { runs_.back().text += '\n'; }
            ++formatted_;
        }
        return true;
    }

    [[nodiscard]] std::vector<ScreenRun> const& runs() const UC_LOG_LIFETIMEBOUND { return runs_; }

    /// Rows formatted since the start (tests: nothing new formats nothing).
    [[nodiscard]] std::size_t formattedRows() const { return formatted_; }

    /// The prefix width of `line`'s group: where its message starts (a click on the message, continuation rows).
    [[nodiscard]] std::size_t prefixCells(StoredLine const& line,
                                          LogColumns const& columns) const {
        auto const& c     = *line.common;
        std::size_t cells = 0;
        if(columns.hostTime) { cells += 13; }   // "HH:MM:SS.mmm "
        if(columns.channel) { cells += width_(fmt::format("{}", c.channel)) + 1; }
        if(columns.targetTime) { cells += width_(fmt::format("{}", c.ucTime)) + 1; }
        if(columns.level) { cells += 7; }   // "trace| "
        return cells;
    }

private:
    CellWidth              width_;
    std::vector<ScreenRun> runs_;
    Frame                  last_;
    bool                   built_{false};
    std::size_t            formatted_{0};

    void push(RunKind      kind,
              std::string  text,
              bool         selected,
              std::uint8_t detail = 0) {
        if(text.empty()) { return; }
        // a lone gap before a run without a background of its own is that run's first cell: one run less for the
        // front end to draw, the same cells
        bool const noBackground = kind == RunKind::targetTime || kind == RunKind::module
                               || kind == RunKind::function || kind == RunKind::location;
        if(noBackground && !runs_.empty() && runs_.back().kind == RunKind::separator
           && runs_.back().text == " ")
        {
            // the gap turned into the run in place: assigning a whole ScreenRun{...} temporary made gcc 16 at -O3
            // without LTO warn that the temporary's style.hyperlink "may be used uninitialized" (-Werror)
            auto& run    = runs_.back();
            run.kind     = kind;
            run.detail   = detail;
            run.selected = selected;
            run.match    = false;
            run.style    = AnsiStyle{};
            run.action   = 0;
            run.text += text;   // after the gap's ' '
            return;
        }
        runs_.push_back(ScreenRun{kind, detail, selected, false, {}, std::move(text)});
    }

    void row(StoredLine const&         line,
             Frame const&              frame,
             EntryStore::Mirror const& names,
             bool                      selected) {
        auto const& c        = *line.common;
        auto const& columns  = frame.columns;
        auto const  rowCells = frame.width;
        std::size_t used     = 0;

        if(line.startsGroup()) {
            if(columns.hostTime) {
                push(RunKind::hostTime, localTimeOfDay(c.recvTime) + ' ', selected);
                used += 13;
            }
            if(columns.channel) {
                auto text = fmt::format("{}", c.channel);
                used += width_(text) + 1;
                push(RunKind::channel, std::move(text), selected, c.channel);
                push(RunKind::separator, " ", selected);
            }
            if(columns.targetTime) {
                auto text = fmt::format("{} ", c.ucTime);
                used += width_(text);
                push(RunKind::targetTime, std::move(text), selected);
            }
            if(columns.level) {
                auto const name = levelName(c.logLevel);
                push(RunKind::level,
                     fmt::format("{:<5}", name),
                     selected,
                     static_cast<std::uint8_t>(c.logLevel));
                push(RunKind::separator, "| ", selected);
                used += 7;
            }
            if(!c.parsedOk) {
                static constexpr std::string_view mark = "⚠ unparsed ";
                push(RunKind::unparsed, std::string{mark}, selected);
                used += width_(mark);
            }
        } else {
            auto const indent = prefixCells(line, columns);
            push(RunKind::separator, std::string(indent, ' '), selected);
            used += indent;
        }

        // the right edge: module, function, location on a group's last row, one run each (own colours)
        std::vector<std::pair<RunKind, std::string>> meta;
        bool const last = line.lineType == LineType::singleLine || line.lineType == LineType::last;
        if(last) {
            if(columns.module) {
                auto const module = names.moduleNameOf(c.moduleId);
                if(!module.empty()) {
                    meta.emplace_back(RunKind::module, fmt::format("[{}]", module));
                }
            }
            if(columns.function) {
                meta.emplace_back(RunKind::function, names.functionNameOf(c.functionId));
            }
            if(columns.location) {
                meta.emplace_back(RunKind::location,
                                  fmt::format("{}:{}",
                                              names.fileNameOf(c.locationKey),
                                              EntryStore::Mirror::lineOf(c.locationKey)));
            }
        }
        std::size_t metaCells = 0;
        for(auto const& [kind, text] : meta) {
            metaCells += width_(text) + 1;
        }   // each after a space
        // a right edge that leaves the message less than a quarter of the row is left out
        if(used + metaCells + rowCells / 4 > rowCells) {
            meta.clear();
            metaCells = 0;
        }

        auto const room  = rowCells > used + metaCells ? rowCells - used - metaCells : 0;
        auto const text  = shownText(line.lineText(), columns.text);
        bool const match = !frame.needle.empty()
                        && CompiledFilter::containsCaseInsensitive(line.lineText(), frame.needle);
        message(ansiRuns(text), frame.hOffset, room, selected, match);

        for(auto& [kind, part] : meta) {
            push(RunKind::separator, " ", selected);
            push(kind, std::move(part), selected);
        }
    }

    // the message's runs from cell `offset` on, cut to `room` cells (the last one "…" when cut), padded to `room`
    void message(std::vector<AnsiRun> const& parts,
                 std::size_t                 offset,
                 std::size_t                 room,
                 bool                        selected,
                 bool                        match) {
        std::size_t total = 0;
        for(auto const& part : parts) { total += width_(part.text); }
        bool const  cut = total > offset + room && room > 0;
        auto const  fit = cut ? room - 1 : room;   // the cells for text when "…" takes the last one
        std::size_t skip    = offset;
        std::size_t written = 0;
        for(auto const& part : parts) {
            std::string_view text = part.text;
            if(skip > 0) {
                auto const [bytes, cells] = prefixWithin(text, skip, width_);
                skip -= std::min(skip, cells);
                text.remove_prefix(bytes);
                // a wide character the offset falls into is dropped, its cell shown as a space
                if(skip > 0 && !text.empty()) {
                    auto const bounds = Detail::codePointBounds(text);
                    text.remove_prefix(bounds.size() > 1 ? bounds[1] : text.size());
                    skip = 0;
                }
            }
            if(text.empty() || written >= fit) { continue; }
            auto const [bytes, cells] = prefixWithin(text, fit - written, width_);
            if(bytes == 0) { continue; }
            runs_.push_back(ScreenRun{RunKind::message,
                                      0,
                                      selected,
                                      match,
                                      part.style,
                                      std::string{text.substr(0, bytes)}});
            written += cells;
        }
        if(cut) {
            if(written < fit) {
                runs_.push_back(ScreenRun{RunKind::message,
                                          0,
                                          selected,
                                          false,
                                          {},
                                          std::string(fit - written, ' ')});
            }
            push(RunKind::cut, "…", selected);
        } else if(written < room) {
            runs_.push_back(ScreenRun{RunKind::message,
                                      0,
                                      selected,
                                      false,
                                      {},
                                      std::string(room - written, ' ')});
        }
    }
};

}   // namespace uc_log::printer
