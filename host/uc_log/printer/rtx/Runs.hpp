#pragma once

// The styled pieces of the RTXUI window: a run of the log or a panel as a span (a class of the style sheet, a
// message's own ANSI style inline), a clickable piece of a bar (a tab, a tool, a hint), the width of text in cells as
// RTXUI lays it out, and the index a click hands its handler.

#include "uc_log/printer/LineText.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

/// RTXUI's own measure of text (its layout lays text out with it): the log's padding ends exactly at the right edge.
std::size_t cellWidth(std::string_view text);

/// The index a click's handler gets (its argument, `Pick({$index})`).
std::size_t argIndex(std::string const& arg);

struct RtxuiWidth {
    std::size_t operator()(std::string_view text) const { return cellWidth(text); }
};

/// One styled piece of text: a class of the component's style sheet, an inline style (a message's own colours).
struct Span {
    std::string cls;
    std::string style;
    std::string text;

    bool operator==(Span const&) const = default;
};

void Fields(Span const&         span,
            ::rtxui::FieldSink& out);

/// CSS for a message's own ANSI style ("" for a plain one).
std::string inlineStyle(AnsiStyle const& style);

/// The class list of a run: its kind, its level, channel or tone, "sel" on the marked row, "hit" on a search match.
std::string runClass(ScreenRun const& run);

/// Runs as spans, neighbours of the same class and style merged: fewer elements for RTXUI to reconcile and lay out.
std::vector<Span> spansOf(std::vector<ScreenRun> const& runs);

/// A clickable piece of a bar: a tab, a panel's button, a hint.
struct Chip {
    std::string cls;   // "on": the open tab, the chosen range
    std::string key;   // the key that does the same ("" none)
    std::string text;

    bool operator==(Chip const&) const = default;
};

void Fields(Chip const&         chip,
            ::rtxui::FieldSink& out);

}   // namespace uc_log::printer::rtx
