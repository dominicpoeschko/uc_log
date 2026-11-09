#pragma once

// A panel's rows for any front end: a row is a list of styled parts (a part may be a button), cut or padded to the
// panel's width as screen runs (LogScreen.hpp), or wrapped onto more rows; the scrollbar beside a list. What a panel
// shows is built in PanelText.hpp, a page's boxes in PageForm.hpp.

#include "uc_log/printer/LineText.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/LogViewport.hpp"
#include "uc_log/printer/Tone.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// One piece of a panel row.
struct PanelPart {
    RunKind       kind{RunKind::message};
    std::uint8_t  detail{};
    AnsiStyle     style;
    std::string   text;
    bool          selected{};   // drawn as the marked one (a cursor)
    std::uint16_t action{};     // 0 none, i + 1: a click does its page's actions[i] (PageForm.hpp)
};

using PanelRow = std::vector<PanelPart>;

/// A panel's list as LogViewport's Lines: row i is id {i, 0}.
struct IndexLines {
    std::size_t count{};

    [[nodiscard]] std::size_t size() const { return count; }

    [[nodiscard]] LineId idAt(std::size_t i) const { return {i, 0}; }
};

inline PanelPart toned(Tone        tone,
                       std::string text) {
    return {RunKind::toned, static_cast<std::uint8_t>(tone), {}, std::move(text)};
}

inline PanelPart label(std::string text) { return {RunKind::label, 0, {}, std::move(text)}; }

/// Rows [first, first + count) of `rows` as runs `width` cells wide: a row longer than that is cut with "…", a
/// shorter one padded; rows end with '\n' but the last.
template<typename CellWidth>
std::vector<ScreenRun> panelRuns(std::vector<PanelRow> const& rows,
                                 std::size_t                  first,
                                 std::size_t                  count,
                                 std::size_t                  width,
                                 CellWidth const&             cells) {
    std::vector<ScreenRun> out;
    auto const             last = std::min(rows.size(), first + count);
    for(std::size_t r = first; r < last; ++r) {
        std::size_t used  = 0;
        bool        cutAt = false;
        for(auto const& part : rows[r]) {
            if(part.text.empty()) { continue; }
            if(used >= width) {
                cutAt = true;
                break;
            }
            auto const w = cells(part.text);
            if(used + w <= width) {
                out.push_back(ScreenRun{part.kind,
                                        part.detail,
                                        part.selected,
                                        false,
                                        part.style,
                                        part.text,
                                        part.action});
                used += w;
                continue;
            }
            // the cut: as much as fits before the "…"
            auto const room         = width - used - 1;   // used < width here
            auto const [bytes, got] = prefixWithin(part.text, room, cells);
            if(bytes > 0) {
                out.push_back(ScreenRun{part.kind,
                                        part.detail,
                                        part.selected,
                                        false,
                                        part.style,
                                        part.text.substr(0, bytes),
                                        part.action});
            }
            used += got;
            cutAt = true;
            break;
        }
        if(cutAt) {
            if(used < width) {
                out.push_back(ScreenRun{RunKind::cut,
                                        0,
                                        false,
                                        false,
                                        {},
                                        std::string(width - used - 1, ' ') + "…"});
            }
        } else if(used < width) {
            out.push_back(
              ScreenRun{RunKind::message, 0, false, false, {}, std::string(width - used, ' ')});
        }
        if(r + 1 < last && !out.empty()) { out.back().text += '\n'; }
    }
    return out;
}

/// `parts` broken into rows of at most `width` cells (a row break where a part does not fit; a part wider than a row
/// split at the cell), every row after the first starting with `indent` spaces.
template<typename CellWidth>
std::vector<PanelRow> wrapParts(std::vector<PanelPart> const& parts,
                                std::size_t                   width,
                                std::size_t                   indent,
                                CellWidth const&              cells) {
    std::vector<PanelRow> rows(1);
    std::size_t           used = 0;
    width                      = std::max<std::size_t>(width, indent + 8);
    for(auto const& part : parts) {
        std::string_view rest = part.text;
        while(!rest.empty()) {
            if(used >= width) {
                rows.emplace_back();
                rows.back().push_back({RunKind::message, 0, {}, std::string(indent, ' ')});
                used = indent;
            }
            auto const [bytes, got] = prefixWithin(rest, width - used, cells);
            if(bytes == 0) {   // one character wider than the room: the next row
                used = width;
                continue;
            }
            rows.back().push_back(
              {part.kind, part.detail, part.style, std::string{rest.substr(0, bytes)}});
            used += got;
            rest.remove_prefix(bytes);
        }
    }
    return rows;
}

/// The scrollbar beside a view of `rows` rows showing [first, first + count) of `total`: one character a row ("│"
/// the track, "█" the thumb), rows separated by '\n'; blank when everything fits.
inline std::string scrollbarText(std::size_t rows,
                                 std::size_t first,
                                 std::size_t count,
                                 std::size_t total) {
    std::string out;
    if(rows == 0) { return out; }
    bool const fits  = total <= rows;
    auto const thumb = fits ? rows : std::max<std::size_t>(1, rows * count / total);
    auto       top   = fits ? 0 : rows * first / total;
    // the thumb reaches the bottom exactly when the view does
    if(!fits && first + count >= total) { top = rows - thumb; }
    top = std::min(top, rows - thumb);
    for(std::size_t r = 0; r < rows; ++r) {
        if(r != 0) { out += '\n'; }
        out += fits ? " " : (r >= top && r < top + thumb ? "█" : "│");
    }
    return out;
}

}   // namespace uc_log::printer
