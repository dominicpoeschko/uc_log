#pragma once

// The TUI's small building blocks, one definition each: the status badges of the control socket, the duplex channels
// and the log file (the Settings tab's words and the status line's icons), the colour of a message level, buttons by
// style, titled sections, labelled rows, spacers, the tab bars, port and number inputs, the toggle-set filters
// (levels, channels) and the included / excluded location lists.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"
#include "uc_log/printer/ToggleSet.hpp"
#include "uc_log/printer/tui/FTXUI_Utils.hpp"
#include "uc_log/printer/tui/theme.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uc_log::printer::tui {

struct Badge {
    std::string  text;
    ftxui::Color colour;
};

inline ftxui::Element render(Badge const& b) {
    return ftxui::text(b.text) | ftxui::color(b.colour) | ftxui::bold;
}

/// A socket's state in words (Settings: control, duplex channels).
inline Badge badgeOf(SocketPortStatus status,
                     bool             enabled) {
    using T = uc_log::printer::tui::Theme;
    if(!enabled) { return {"Disabled ○", T::Text::normal()}; }
    switch(status) {
    case SocketPortStatus::active:       return {"Active ●", T::Status::success()};
    case SocketPortStatus::portOccupied: return {"Port Occupied ⚠", T::Status::error()};
    case SocketPortStatus::notStarted:   return {"Not Started ○", T::Text::normal()};
    }
    return {"Not Started ○", T::Text::normal()};
}

/// The log file's state in words (Settings).
inline Badge badgeOf(LogFileStatus status,
                     bool          enabled) {
    using T = uc_log::printer::tui::Theme;
    if(!enabled) { return {"Disabled ○", T::Text::normal()}; }
    switch(status) {
    case LogFileStatus::active:     return {"Active ●", T::Status::success()};
    case LogFileStatus::error:      return {"Error ✘", T::Status::error()};
    case LogFileStatus::notStarted: return {"Not Started ○", T::Text::normal()};
    }
    return {"Not Started ○", T::Text::normal()};
}

/// The status line's icon of the log file.
inline Badge iconOf(LogFileStatus status,
                    bool          enabled) {
    using T = uc_log::printer::tui::Theme;
    if(!enabled) { return {"📄 ○", T::Text::normal()}; }
    switch(status) {
    case LogFileStatus::active:     return {"📄 ●", T::Status::success()};
    case LogFileStatus::error:      return {"📄 ✘", T::Status::error()};
    case LogFileStatus::notStarted: return {"📄 ○", T::Text::normal()};
    }
    return {"📄 ○", T::Text::normal()};
}

/// The status line's icon of the control socket (● also while it starts).
inline Badge iconOf(SocketPortStatus status,
                    bool             enabled) {
    using T = uc_log::printer::tui::Theme;
    if(!enabled) { return {"🌐 ○", T::Text::normal()}; }
    switch(status) {
    case SocketPortStatus::active:       return {"🌐 ●", T::Status::success()};
    case SocketPortStatus::portOccupied: return {"🌐 ⚠", T::Status::error()};
    case SocketPortStatus::notStarted:   return {"🌐 ●", T::Text::normal()};
    }
    return {"🌐 ●", T::Text::normal()};
}

/// The Status tab's colour of a message.
inline ftxui::Color colourOf(MessageLevel level) {
    using T = uc_log::printer::tui::Theme;
    switch(level) {
    case MessageLevel::fatal:     return T::Message::fatal();
    case MessageLevel::error:     return T::Message::error();
    case MessageLevel::status:    return T::Message::status();
    case MessageLevel::toolError: return T::Message::toolError();
    case MessageLevel::tool:      return T::Message::tool();
    }
    return T::Message::status();
}

// ---- buttons -------------------------------------------------------------------------------------------------

/// The button colours of the theme (Theme::Button::Background), by what the button does.
enum class Style : std::uint8_t { positive, destructive, reset, build, debug, settings, danger };

inline ftxui::Color backgroundOf(Style style) {
    using B = uc_log::printer::tui::Theme::Button::Background;
    switch(style) {
    case Style::positive:    return B::positive();
    case Style::destructive: return B::destructive();
    case Style::reset:       return B::reset();
    case Style::build:       return B::build();
    case Style::debug:       return B::debug();
    case Style::settings:    return B::settings();
    case Style::danger:      return B::danger();
    }
    return B::settings();
}

inline ftxui::ButtonOption buttonStyle(Style style) {
    return uc_log::printer::tui::createButtonStyle(backgroundOf(style),
                                                   uc_log::printer::tui::Theme::Button::text());
}

inline ftxui::Component button(ftxui::ConstStringRef label,
                               Style                 style,
                               std::function<void()> onClick) {
    return ftxui::Button(std::move(label), std::move(onClick), buttonStyle(style));
}

// ---- layout ----------------------------------------------------------------------------------------------------

/// A separator line between the rows of a container.
inline ftxui::Component divider() {
    return ftxui::Renderer([] { return ftxui::separator(); });
}

/// " | " in the separator colour between the buttons of a row.
inline ftxui::Component barDivider() {
    return ftxui::Renderer([] {
        return ftxui::text(" | ") | ftxui::color(uc_log::printer::tui::Theme::UI::separator());
    });
}

/// `width` blanks between two components of a row.
inline ftxui::Component spacer(int width = 2) {
    return ftxui::Renderer(
      [width] { return ftxui::text(std::string(static_cast<std::size_t>(width), ' ')); });
}

/// A bold coloured title line above the `body` components.
inline ftxui::Component section(std::string       title,
                                ftxui::Color      colour,
                                ftxui::Components body) {
    body.insert(body.begin(), ftxui::Renderer([text = std::move(title), colour] {
                    return ftxui::text(text) | ftxui::bold | ftxui::color(colour);
                }));
    return ftxui::Container::Vertical(std::move(body));
}

/// A heading line of a text page (Help): bold, in the accent colour.
inline ftxui::Element heading(std::string_view text) {
    return ftxui::text(text) | ftxui::bold
         | ftxui::color(uc_log::printer::tui::Theme::Header::accent());
}

/// A tab bar: the names as a bold toggle, a separator, the selected page. The three bars of the TUI (the main tabs,
/// Metrics, Inspect) are this; `names` are shown as they are.
inline ftxui::Component tabs(std::vector<std::string>      names,
                             std::vector<ftxui::Component> pages,
                             int*                          selected) {
    auto toggle = ftxui::Toggle(std::move(names), selected) | ftxui::bold;
    return ftxui::Container::Vertical(
      {toggle, divider(), ftxui::Container::Tab(std::move(pages), selected) | ftxui::flex});
}

// ---- inputs ----------------------------------------------------------------------------------------------------

/// Only digits (and a dot when `allowDot`) reach the input.
inline ftxui::ComponentDecorator numericFilter(bool allowDot) {
    return ftxui::CatchEvent([allowDot](ftxui::Event const& e) {
        if(!e.is_character()) { return false; }
        auto const& s = e.character();
        if(s.size() != 1) { return true; }
        char const c = s[0];
        return !((c >= '0' && c <= '9') || (allowDot && c == '.'));
    });
}

/// A one-line text input over `text` (which outlives the component); `onChange` after every edit, `onEnter` on Enter.
inline ftxui::Component textInput(std::string&          text,
                                  std::string           placeholder,
                                  std::function<void()> onChange = {},
                                  std::function<void()> onEnter  = {}) {
    ftxui::InputOption o;
    o.multiline = false;
    if(onChange) { o.on_change = std::move(onChange); }
    if(onEnter) { o.on_enter = std::move(onEnter); }
    return ftxui::Input(&text, std::move(placeholder), o);
}

/// A port: digits, at most 5 of them. Whether it is one (1024..65535) the model decides (printer::parsePort).
// `text` outlives the component (a Gui member).
inline ftxui::Component portInput(std::string& text,
                                  std::string  placeholder) {
    return textInput(text, std::move(placeholder)) | numericFilter(false)
         | ftxui::CatchEvent(
             [&text](ftxui::Event const& e) { return e.is_character() && text.size() >= 5; });
}

/// The number at the start of `text` (as std::stod / std::stoull read it), nullopt when there is none.
template<typename T>
std::optional<T> leadingNumber(std::string_view text) {
    T v{};
    auto const [ptr, ec]
      = std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), v);
    if(ec != std::errc{} || ptr == text.data()) { return std::nullopt; }
    return v;
}

/// A number typed in `text` (which outlives the component): every change that holds one calls `onValue`; an empty text calls it with `whenEmpty`
/// when there is one. Digits (and a dot for a floating-point T) only.
template<typename T>
ftxui::Component numberInput(std::string&           text,
                             std::string            placeholder,
                             std::function<void(T)> onValue,
                             std::optional<T>       whenEmpty = std::nullopt) {
    auto changed = [&text, call = std::move(onValue), whenEmpty] {
        if(text.empty()) {
            if(whenEmpty) { call(*whenEmpty); }
            return;
        }
        if(auto const v = leadingNumber<T>(text)) { call(*v); }
    };
    return textInput(text, std::move(placeholder), std::move(changed))
         | numericFilter(std::is_floating_point_v<T>);
}

// ---- filters -----------------------------------------------------------------------------------------------------

/// "Enable all" and one checkbox per value over a set where empty = all (printer/ToggleSet.hpp), in a border. `set`
/// outlives the component (the Gui's filter state).
template<typename T,
         typename All>
ftxui::Component toggleSetFilter(std::string                          allLabel,
                                 Style                                allStyle,
                                 std::set<T>&                         set,
                                 All                                  all,
                                 std::function<std::string(T)> const& label,
                                 std::function<void()> const&         changed) {
    std::vector<ftxui::Component> rows;
    rows.push_back(button(std::move(allLabel), allStyle, [&set, changed] {
        set.clear();
        changed();
    }));
    for(auto const v : all) {
        rows.push_back(uc_log::printer::tui::functionCheckbox(
          label(v),
          [&set, v] { return shownIn(set, v); },
          [&set, all, v, changed] {
              toggleIn(set, all, v);
              changed();
          }));
    }
    return ftxui::Container::Vertical(rows) | ftxui::border;
}

/// "file:line" (":*" for a whole file).
template<typename Location>
std::string locationText(Location const& location) {
    return location.first
         + (location.second != 0 ? ":" + std::to_string(location.second) : std::string{":*"});
}

/// The included or excluded locations: "<title> (n)", then one button per location that removes it ("(none)" when
/// there is none), rebuilt every frame, in a border.
template<typename Location>
ftxui::Component locationList(std::set<Location>&   set,
                              std::string           title,
                              ftxui::Color          titleColour,
                              std::string           icon,
                              std::function<void()> changed) {
    auto container = ftxui::Container::Vertical({});
    return container
         | ftxui::Renderer(
             [&set,
              container,
              heading = std::move(title),
              titleColour,
              mark     = std::move(icon),
              onRemove = std::move(changed)](ftxui::Element const&) mutable -> ftxui::Element {
                 using T = uc_log::printer::tui::Theme;
                 container->DetachAllChildren();
                 container->Add(ftxui::Renderer([&set, heading, titleColour] {
                     return ftxui::text(fmt::format("{} ({})", heading, set.size())) | ftxui::bold
                          | ftxui::color(titleColour);
                 }));
                 if(set.empty()) {
                     container->Add(ftxui::Renderer([] {
                         return ftxui::text("(none)") | ftxui::color(T::Status::inactive())
                              | ftxui::center;
                     }));
                 } else {
                     for(auto const& location : set) {
                         container->Add(button(mark + " " + locationText(location) + " ❌",
                                               Style::build,
                                               [&set, location, onRemove] {
                                                   set.erase(location);
                                                   onRemove();
                                               }));
                     }
                 }
                 return container->Render();
             })
         | ftxui::border;
}

}   // namespace uc_log::printer::tui
