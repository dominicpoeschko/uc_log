#include "uc_log/printer/rtx/Runs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

std::size_t cellWidth(std::string_view text) {
    return static_cast<std::size_t>(std::max(0, ::string_width(text)));
}

std::size_t argIndex(std::string const& arg) {
    std::size_t i{};
    std::from_chars(std::to_address(arg.begin()), std::to_address(arg.end()), i);
    return i;
}

void Fields(Span const&         span,
            ::rtxui::FieldSink& out) {
    out("cls", span.cls);
    out("style", span.style);
    out("text", span.text);
}

void Fields(Chip const&         chip,
            ::rtxui::FieldSink& out) {
    out("cls", chip.cls);
    out("key", chip.key);
    out("text", chip.text);
}

namespace {
    struct Rgb {
        std::uint8_t r, g, b;
    };

    // the 16 colours as a dark terminal shows them, then the 256-colour cube and the greys
    Rgb rgbOf(AnsiColor const& c) {
        static constexpr std::array<Rgb, 16> basic{
          {{0, 0, 0},
           {205, 49, 49},
           {13, 188, 121},
           {229, 229, 16},
           {36, 114, 200},
           {188, 63, 188},
           {17, 168, 205},
           {229, 229, 229},
           {102, 102, 102},
           {241, 76, 76},
           {35, 209, 139},
           {245, 245, 67},
           {59, 142, 234},
           {214, 112, 214},
           {41, 184, 219},
           {255, 255, 255}}
        };
        if(c.kind == AnsiColor::Kind::rgb) { return {c.r, c.g, c.b}; }
        auto const i = c.index;
        if(i < 16) { return basic[i]; }
        if(i < 232) {
            static constexpr std::array<std::uint8_t, 6> level{0, 95, 135, 175, 215, 255};
            auto const                                   n = static_cast<std::size_t>(i - 16);
            return {level[n / 36], level[(n / 6) % 6], level[n % 6]};
        }
        auto const grey = static_cast<std::uint8_t>(8 + 10 * (i - 232));
        return {grey, grey, grey};
    }

    std::string css(Rgb c) { return fmt::format("rgb({}, {}, {})", c.r, c.g, c.b); }

    constexpr Rgb textColour{205, 210, 218};   // .log's colour
    constexpr Rgb backColour{18, 20, 25};      // the window's background

}   // namespace

std::string inlineStyle(AnsiStyle const& s) {
    // a hyperlink alone is no style here: a span cannot be a link
    auto unlinked = s;
    unlinked.hyperlink.clear();
    if(unlinked.plain()) { return {}; }
    std::string out;
    auto        fg
      = s.fg.kind == AnsiColor::Kind::none ? std::optional<Rgb>{} : std::optional<Rgb>{rgbOf(s.fg)};
    auto bg
      = s.bg.kind == AnsiColor::Kind::none ? std::optional<Rgb>{} : std::optional<Rgb>{rgbOf(s.bg)};
    if(s.reverse) {
        auto const f = fg.value_or(textColour);
        auto const b = bg.value_or(backColour);
        fg           = b;
        bg           = f;
    }
    if(fg) { out += fmt::format("color: {}; ", css(*fg)); }
    if(bg) { out += fmt::format("background-color: {}; ", css(*bg)); }
    if(s.bold) { out += "font-weight: bold; "; }
    if(s.dim) { out += "opacity: 0.6; "; }
    if(s.italic) { out += "font-style: italic; "; }
    if(s.underline || s.strike) {
        out += fmt::format("text-decoration: {}{}; ",
                           s.underline ? "underline" : "",
                           s.strike ? (s.underline ? " line-through" : "line-through") : "");
    }
    if(!out.empty()) { out.pop_back(); }
    return out;
}

std::string runClass(ScreenRun const& run) {
    std::string cls;
    switch(run.kind) {
    case RunKind::hostTime:   cls = "hostTime"; break;
    case RunKind::channel:    cls = fmt::format("ch{}", run.detail % 6); break;
    case RunKind::targetTime: cls = "targetTime"; break;
    case RunKind::level:      cls = fmt::format("lv{}", std::min<unsigned>(run.detail, 5)); break;
    case RunKind::separator:  cls = "sep"; break;
    case RunKind::unparsed:   cls = "unparsed"; break;
    case RunKind::message:    break;
    case RunKind::cut:        cls = "cut"; break;
    case RunKind::module:     cls = "module"; break;
    case RunKind::function:   cls = "function"; break;
    case RunKind::location:   cls = "location"; break;
    case RunKind::toned:      cls = fmt::format("t{}", std::min<unsigned>(run.detail, 4)); break;
    case RunKind::label:      cls = "label"; break;
    case RunKind::button:
        {
            cls = (run.detail & 1U) != 0 ? "btn on" : "btn";
            if(auto const tone = run.detail >> 1U; tone != 0) {
                cls += fmt::format(" t{}", std::min(tone, 4));
            }
            break;
        }
    }
    if(run.selected) { cls += cls.empty() ? "sel" : " sel"; }
    if(run.match) { cls += cls.empty() ? "hit" : " hit"; }
    return cls;
}

std::vector<Span> spansOf(std::vector<ScreenRun> const& runs) {
    std::vector<Span> out;
    out.reserve(runs.size());
    for(auto const& run : runs) {
        auto cls   = runClass(run);
        auto style = run.kind == RunKind::message ? inlineStyle(run.style) : std::string{};
        if(!out.empty() && out.back().cls == cls && out.back().style == style) {
            out.back().text += run.text;
        } else {
            out.push_back({std::move(cls), std::move(style), run.text});
        }
    }
    return out;
}

}   // namespace uc_log::printer::rtx
