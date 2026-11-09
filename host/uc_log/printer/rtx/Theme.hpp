#pragma once

// The RTXUI window's colours, and how a component's template is made - at compile time.
//
// A style names a colour by what it is for, `var(--action)`, `var(--lv-warn)`, and the template gets the colour from
// `theme` when it compiles. Not RTXUI's own custom properties: RTXUI copies those into every element that inherits
// them, and with the log's runs that took a frame's digest from 1.1 to 4.0 ms. A name the theme does not have does
// not compile (tests theme_*). A template is joined from parts - its own text and the shared looks of Look.hpp - and
// `viewOf<parts...>` is what a component hands RTXUI.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace uc_log::printer::rtx::Detail {

struct ThemeColour {
    std::string_view name;   // as a template writes it: var(--name)
    std::string_view colour;
};

inline constexpr std::array<ThemeColour, 46> theme{
  {
   // surfaces, the darkest first
    {"bg", "rgb(18, 20, 25)"},             // the window, under every frame
    {"surface", "rgb(24, 27, 34)"},        // a head, the mode line, a dialog, the menu
    {"raised", "rgb(30, 34, 43)"},         // a tab, a badge, a switch that is off
    {"field", "rgb(40, 44, 54)"},          // a dialog's field
    {"marked", "rgb(44, 58, 92)"},         // the marked row: of the log, a list, a dialog
    {"hover-row", "rgb(60, 66, 80)"},      // a row or a tab under the mouse
    {"border", "rgb(62, 70, 88)"},         // the frames
    {"backdrop", "rgba(0, 0, 0, 0.45)"},   // the window behind a dialog
    // text
    {"bright", "white"},   // on a fill
    {"text", "rgb(205, 210, 218)"},
   {"muted", "rgb(160, 166, 178)"},   // tabs, badges, what a dialog is about
    {"dim", "rgb(110, 116, 128)"},     // hints, labels, a switch that is off
    {"faint", "rgb(80, 86, 98)"},      // separators, the scrollbar, a cut
    {"ink", "rgb(16, 18, 23)"},        // on a light fill: under the mouse, on, a key cap
    // what a colour says
    {"accent",
     "rgb(110, 190, 230)"},   // titles, legends, the open tab, a switch or a tool that is on
    {"hover", "rgb(235, 190, 90)"},   // a button, an item under the mouse
    {"key", "rgb(235, 190, 90)"},     // a key that does the same
    {"focus", "rgb(59, 130, 246)"},   // the focus, while Tab moves it
    {"action", "rgb(42, 78, 124)"},   // a button that does something
    {"go", "rgb(30, 96, 60)"},        // ... that starts something
    {"danger", "rgb(120, 36, 36)"},   // ... that stops, clears or destroys; a badge with trouble
    {"ok", "rgb(110, 205, 130)"},
   {"warn", "rgb(235, 190, 90)"},
   {"bad", "rgb(245, 95, 85)"},
   {"ok-fill", "rgb(26, 64, 42)"},
   {"ok-text", "rgb(140, 230, 160)"},
   {"warn-fill", "rgb(92, 72, 24)"},   // a chip: something hidden, a click undoes it
    {"warn-text", "rgb(255, 220, 130)"},
   {"info-fill", "rgb(28, 56, 88)"},
   {"info-text", "rgb(150, 205, 245)"},
   // the log's columns
    {"host-time", "rgb(95, 175, 190)"},
   {"target-time", "rgb(190, 130, 210)"},
   {"module", "rgb(235, 190, 90)"},
   {"function", "rgb(100, 140, 245)"},
   {"lv-trace", "rgb(225, 205, 95)"},
   {"lv-debug", "rgb(110, 205, 130)"},
   {"lv-info", "rgb(110, 175, 255)"},
   {"lv-warn", "rgb(215, 120, 215)"},
   {"lv-error", "rgb(245, 95, 85)"},
   {"lv-crit", "rgb(205, 60, 60)"},
   {"ch0", "rgb(110, 205, 130)"},
   {"ch1", "rgb(245, 95, 85)"},
   {"ch2", "rgb(100, 140, 245)"},
   {"ch3", "rgb(205, 110, 215)"},
   {"ch4", "rgb(230, 230, 230)"},
   {"ch5", "rgb(235, 205, 90)"},
   }
};

// not constexpr: a template naming a colour the theme does not have fails to compile here
void noSuchThemeColour();

// where the next `c` at or after `from` is, else text.size(). Not string_view::find: it tests the pointer
// char_traits::find returns, and with -fsanitize=null (gcc's address config adds undefined) gcc cannot
// constant-evaluate that test for text in a namespace-scope array (joinedOf below): "... != 0 is not a constant
// expression".
consteval std::size_t indexOf(std::string_view text,
                              char             c,
                              std::size_t      from) {
    for(std::size_t i = from; i < text.size(); ++i) {
        if(text[i] == c) { return i; }
    }
    return text.size();
}

// `text` with each var(--name) the colour the theme gives it: how long it gets, and the text
consteval std::size_t themedSize(std::string_view text) {
    constexpr std::string_view open{"var(--"};
    std::size_t                size = 0;
    for(std::size_t i = 0; i < text.size();) {
        if(text.substr(i, open.size()) != open) {
            ++size;
            ++i;
            continue;
        }
        auto const end  = indexOf(text, ')', i);
        auto const name = text.substr(i + open.size(), end - i - open.size());
        auto const it   = std::ranges::find(theme, name, &ThemeColour::name);
        // a name the theme does not have counts as it is: themed() stops at it, and says so
        size += it == theme.end() ? end + 1 - i : it->colour.size();
        i = end + 1;
    }
    return size;
}

template<std::size_t Size>
struct Themed {
    std::array<char, Size> text{};

    [[nodiscard]] constexpr std::string_view view() const { return {text.data(), Size}; }
};

template<std::size_t Size>
consteval Themed<Size> themed(std::string_view text) {
    constexpr std::string_view open{"var(--"};
    Themed<Size>               out;
    std::size_t                at = 0;
    for(std::size_t i = 0; i < text.size();) {
        if(text.substr(i, open.size()) != open) {
            out.text[at++] = text[i++];
            continue;
        }
        auto const end  = indexOf(text, ')', i);
        auto const name = text.substr(i + open.size(), end - i - open.size());
        auto const it   = std::ranges::find(theme, name, &ThemeColour::name);
        if(it == theme.end()) { noSuchThemeColour(); }
        for(char const c : it->colour) { out.text[at++] = c; }
        i = end + 1;
    }
    return out;
}

// a component's template as RTXUI reads it: the source joined (a char array, its NUL last), themed
template<std::size_t N>
consteval std::string_view sourceOf(std::array<char,
                                               N> const& joined) {
    return {joined.data(), N - 1};
}

// a component's template from pieces, joined at compile time (LogView and Panel share the runs' classes)
template<std::size_t... N>
consteval auto joinTexts(char const (&... parts)[N]) {
    std::array<char, (N + ...) - sizeof...(N) + 1> out{};
    std::size_t                                    at  = 0;
    auto const                                     add = [&](auto const& part, std::size_t size) {
        for(std::size_t i = 0; i + 1 < size; ++i) { out.at(at++) = part[i]; }
    };
    (add(parts, N), ...);
    return out;
}

// the parts joined, then themed: each a variable, so the view RTXUI keeps points into static storage
template<auto const&... Parts>
inline constexpr auto joinedOf = joinTexts(Parts...);

template<auto const&... Parts>
inline constexpr auto themedOf
  = themed<themedSize(sourceOf(joinedOf<Parts...>))>(sourceOf(joinedOf<Parts...>));

/// A component's template as RTXUI reads it: `Parts` (char arrays: the template, then the looks it uses, the
/// last one closing its style) joined, the theme's colours put in.
template<auto const&... Parts>
inline constexpr std::string_view viewOf = themedOf<Parts...>.view();

}   // namespace uc_log::printer::rtx::Detail
