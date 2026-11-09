#pragma once

// A page of a window as a front end draws it: its tools, its boxes of rows, its buttons and what each does. The
// pages themselves are built in PrinterPages.hpp, InspectorPages.hpp and FilterPanel.hpp; a front end lays the boxes
// out and maps a button's look to its own kind of button.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/Tone.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// How a page's button looks (a front end draws each kind as its own kind of button).
enum class ButtonLook : std::uint8_t {
    action,   // does something: build, connect, read now
    toggle,   // a switch or a choice: ✔ / ✘, ◉ / ○ - on, it is filled
    chip,     // something hidden or chosen, a click undoes it: "area0.part3 ✕"
    danger,   // halts, raises, clears
    go,       // starts: flash, go, start
    row,      // a row of a list a click marks: the whole width, the marked one with the bar
};

/// A button run's detail (ScreenRun::detail, PanelPart::detail): bit 0 on, bits 1-3 its tone, bits 4-6 its look.
constexpr std::uint8_t buttonDetail(bool       on,
                                    Tone       tone,
                                    ButtonLook look) {
    return static_cast<std::uint8_t>((on ? 1U : 0U) | (static_cast<unsigned>(tone) << 1U)
                                     | (static_cast<unsigned>(look) << 4U));
}

constexpr bool buttonOn(std::uint8_t detail) { return (detail & 1U) != 0; }

constexpr Tone buttonTone(std::uint8_t detail) { return static_cast<Tone>((detail >> 1U) & 7U); }

constexpr ButtonLook buttonLook(std::uint8_t detail) {
    return static_cast<ButtonLook>((detail >> 4U) & 7U);
}

/// A box whose rows scroll: its first `skip` rows stay (a heading), the others are rows [first, first + n) of `total`
/// (n: how many it has after them) - a front end draws a scrollbar beside them, a press on it scrolls there.
struct PageScroll {
    std::size_t skip{};
    std::size_t first{};
    std::size_t total{};
};

/// A box of a page: its title (in its frame's border) and its rows.
struct PageBox {
    std::string           legend;
    std::vector<PanelRow> rows;
    bool                  wide{};   // the whole width (a table, a plot), not a column beside others
    std::optional<PageScroll> scroll;   // its rows are a part of more
};

/// A page of a window: the actions on the whole page (its tools: buttons in its head), boxes (a section with a title,
/// as a framed group) of rows, and what a click on a button of them does - a part's `action` i + 1 is `actions[i]`.
/// `Action` is the front end's (printer/WindowActions.hpp, a FilterItem, a function); the rows stay text any front end
/// draws.
template<typename Action>
struct PageForm {
    std::vector<PanelPart> tools;   // the page's own actions, for its head
    std::vector<PageBox>   boxes;
    std::vector<Action>    actions;

    /// A new box; its rows to fill (until the next box: the reference does not survive it).
    std::vector<PanelRow>& box(std::string legend,
                               bool        wide = false) UC_LOG_LIFETIMEBOUND {
        boxes.push_back({std::move(legend), {}, wide, std::nullopt});
        return boxes.back().rows;
    }

    /// The last box's rows scroll (PageScroll).
    void scrolling(PageScroll scroll) {
        if(!boxes.empty()) { boxes.back().scroll = scroll; }
    }

    /// A button a click runs `action` on.
    PanelPart button(std::string text,
                     Action      action,
                     ButtonLook  look = ButtonLook::action,
                     bool        on   = false,
                     Tone        tone = Tone::plain) {
        actions.push_back(std::move(action));
        return {RunKind::button,
                buttonDetail(on, tone, look),
                {},
                std::move(text),
                false,
                static_cast<std::uint16_t>(actions.size())};
    }

    /// One choice of a set: ◉ chosen, ○ not.
    PanelPart option(std::string_view text,
                     bool             chosen,
                     Action           action) {
        return button(fmt::format("{} {}", chosen ? "◉" : "○", text),
                      std::move(action),
                      ButtonLook::toggle,
                      chosen);
    }

    /// A switch: ✔ set, ✘ not (a front end fills a set one: on or off must show from across the room).
    PanelPart check(std::string_view text,
                    bool             set,
                    Action           action) {
        return button(fmt::format("{} {}", set ? "✔" : "✘", text),
                      std::move(action),
                      ButtonLook::toggle,
                      set);
    }

    /// An action on the whole page: a button in its head.
    void tool(std::string text,
              Action      action,
              ButtonLook  look = ButtonLook::action) {
        tools.push_back(button(std::move(text), std::move(action), look));
    }

    /// `other`'s tools and boxes after these, its buttons still doing what they did.
    void append(PageForm other) {
        auto const offset = static_cast<std::uint16_t>(actions.size());
        for(auto& t : other.tools) {
            t.action = static_cast<std::uint16_t>(t.action + offset);
            tools.push_back(std::move(t));
        }
        for(auto& box : other.boxes) {
            for(auto& row : box.rows) {
                for(auto& part : row) {
                    if(part.action != 0) {
                        part.action = static_cast<std::uint16_t>(part.action + offset);
                    }
                }
            }
            boxes.push_back(std::move(box));
        }
        for(auto& a : other.actions) { actions.push_back(std::move(a)); }
    }
};

}   // namespace uc_log::printer
