#pragma once

// A log line's text as a window shows it, for any front end: the markers a firmware puts into a message
// (@METRIC(scope::name[unit]=value), @TYPENAME(...)) shown or hidden, and the terminal styling a message may carry
// (SGR colours and attributes, OSC 8 hyperlinks) split into styled runs. Every other escape sequence and every
// control character but a tab is dropped: a log line never moves a window's cursor.

#include "uc_log/printer/metric_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

struct ShownTextOptions {
    bool metricMarkers{false};     // true: @METRIC(...) as written; false: "value unit"
    bool typenameMarkers{false};   // true: @TYPENAME(x) as x; false: hidden

    bool operator==(ShownTextOptions const&) const = default;
};

/// The text of a stored line with its markers replaced.
inline std::string shownText(std::string_view        line,
                             ShownTextOptions const& options) {
    std::string text{line};
    std::size_t pos = 0;
    if(!options.metricMarkers) {
        while(auto const marker = nextMetricMarker(text, pos)) {
            std::string shown{marker->value};
            if(!marker->unit.empty()) {
                shown += ' ';
                shown += marker->unit;
            }
            text.replace(marker->begin, marker->end - marker->begin, shown);
            pos = marker->begin + shown.size();
        }
    }
    static constexpr std::string_view typenameStart = "@TYPENAME(";
    pos                                             = 0;
    while((pos = text.find(typenameStart, pos)) != std::string::npos) {
        auto const end = text.find(')', pos + typenameStart.size());
        if(end == std::string::npos) { break; }
        if(options.typenameMarkers) {
            auto const inner
              = text.substr(pos + typenameStart.size(), end - pos - typenameStart.size());
            text.replace(pos, end - pos + 1, inner);
            pos += inner.size();
        } else {
            text.erase(pos, end - pos + 1);
        }
    }
    return text;
}

/// A terminal colour as SGR names it.
struct AnsiColor {
    enum class Kind : std::uint8_t { none, palette, rgb };

    Kind kind{Kind::none};
    std::uint8_t
      index{};   // palette: 0..7 normal, 8..15 bright, 16..255 the 256-colour cube and greys
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};

    bool operator==(AnsiColor const&) const = default;
};

struct AnsiStyle {
    AnsiColor   fg;
    AnsiColor   bg;
    bool        bold{};
    bool        dim{};
    bool        italic{};
    bool        underline{};
    bool        blink{};
    bool        reverse{};
    bool        strike{};
    std::string hyperlink;

    [[nodiscard]] bool plain() const { return *this == AnsiStyle{}; }

    bool operator==(AnsiStyle const&) const = default;
};

struct AnsiRun {
    std::string text;
    AnsiStyle   style;

    bool operator==(AnsiRun const&) const = default;
};

namespace Detail {
    // one SGR parameter list ("1;38;5;208"), applied to `style`
    inline void applySgr(std::string_view params,
                         AnsiStyle&       style) {
        std::vector<int> codes;
        int              value = 0;
        bool             any   = false;
        for(char const c : params) {
            if(c >= '0' && c <= '9') {
                value = std::min(value * 10 + (c - '0'), 1'000'000);
                any   = true;
            } else if(c == ';' || c == ':') {
                codes.push_back(any ? value : 0);
                value = 0;
                any   = false;
            }
        }
        codes.push_back(any ? value : 0);

        auto extended = [&](std::size_t& i, AnsiColor& color) {
            // 38;5;n / 38;2;r;g;b - a short list leaves the colour as it was
            if(i + 2 < codes.size() && codes[i + 1] == 5) {
                color = {AnsiColor::Kind::palette,
                         static_cast<std::uint8_t>(codes[i + 2] & 0xFF),
                         0,
                         0,
                         0};
                i += 2;
            } else if(i + 1 < codes.size() && codes[i + 1] == 2 && i + 4 < codes.size()) {
                color = {AnsiColor::Kind::rgb,
                         0,
                         static_cast<std::uint8_t>(codes[i + 2] & 0xFF),
                         static_cast<std::uint8_t>(codes[i + 3] & 0xFF),
                         static_cast<std::uint8_t>(codes[i + 4] & 0xFF)};
                i += 4;
            } else {
                i = codes.size();
            }
        };
        auto palette = [](int index) {
            return AnsiColor{AnsiColor::Kind::palette, static_cast<std::uint8_t>(index), 0, 0, 0};
        };

        for(std::size_t i = 0; i < codes.size(); ++i) {
            int const code = codes[i];
            switch(code) {
            case 0:
                // everything but the hyperlink: that one is OSC 8's
                {
                    auto link       = std::move(style.hyperlink);
                    style           = AnsiStyle{};
                    style.hyperlink = std::move(link);
                }
                break;
            case 1:  style.bold = true; break;
            case 2:  style.dim = true; break;
            case 3:  style.italic = true; break;
            case 4:  style.underline = true; break;
            case 5:
            case 6:  style.blink = true; break;
            case 7:  style.reverse = true; break;
            case 9:  style.strike = true; break;
            case 21: style.underline = true; break;
            case 22: style.bold = style.dim = false; break;
            case 23: style.italic = false; break;
            case 24: style.underline = false; break;
            case 25: style.blink = false; break;
            case 27: style.reverse = false; break;
            case 29: style.strike = false; break;
            case 38: extended(i, style.fg); break;
            case 39: style.fg = {}; break;
            case 48: extended(i, style.bg); break;
            case 49: style.bg = {}; break;
            default:
                if(code >= 30 && code <= 37) {
                    style.fg = palette(code - 30);
                } else if(code >= 40 && code <= 47) {
                    style.bg = palette(code - 40);
                } else if(code >= 90 && code <= 97) {
                    style.fg = palette(code - 90 + 8);
                } else if(code >= 100 && code <= 107) {
                    style.bg = palette(code - 100 + 8);
                }
                break;
            }
        }
    }
}   // namespace Detail

/// `text` (one display line: no '\n') split into runs of one style each; empty runs are left out, so plain text is
/// one run with a plain style and an empty text none at all.
inline std::vector<AnsiRun> ansiRuns(std::string_view text) {
    std::vector<AnsiRun> runs;
    AnsiStyle            style;
    std::string          current;
    auto                 flush = [&] {
        if(current.empty()) { return; }
        if(!runs.empty() && runs.back().style == style) {
            runs.back().text += current;
        } else {
            runs.push_back({std::move(current), style});
        }
        current.clear();
    };

    std::size_t i = 0;
    while(i < text.size()) {
        char const c = text[i];
        if(c == '\x1B') {
            if(i + 1 >= text.size()) { break; }
            char const kind = text[i + 1];
            if(kind == '[') {
                // CSI: parameters 0x30..0x3F, intermediates 0x20..0x2F, final 0x40..0x7E; only 'm' is kept
                std::size_t j = i + 2;
                while(j < text.size()
                      && (static_cast<unsigned char>(text[j]) < 0x40
                          || static_cast<unsigned char>(text[j]) > 0x7E))
                {
                    ++j;
                }
                if(j >= text.size()) { break; }
                if(text[j] == 'm') {
                    flush();
                    Detail::applySgr(text.substr(i + 2, j - i - 2), style);
                }
                i = j + 1;
                continue;
            }
            if(kind == ']') {
                // OSC ... BEL or ST (ESC \); OSC 8 ; params ; uri is a hyperlink, an empty uri ends it
                std::size_t j   = i + 2;
                std::size_t end = std::string_view::npos;
                std::size_t len = 0;
                while(j < text.size()) {
                    if(text[j] == '\x07') {
                        end = j;
                        len = 1;
                        break;
                    }
                    if(text[j] == '\x1B' && j + 1 < text.size() && text[j + 1] == '\\') {
                        end = j;
                        len = 2;
                        break;
                    }
                    ++j;
                }
                if(end == std::string_view::npos) { break; }
                auto const body = text.substr(i + 2, end - i - 2);
                if(body.starts_with("8;")) {
                    auto const uriStart = body.find(';', 2);
                    flush();
                    style.hyperlink = uriStart == std::string_view::npos
                                      ? std::string{}
                                      : std::string{body.substr(uriStart + 1)};
                }
                i = end + len;
                continue;
            }
            // any other escape (ESC ( B, ESC 7, ...): ESC, its intermediates 0x20..0x2F, its final byte
            std::size_t j = i + 1;
            while(j < text.size() && static_cast<unsigned char>(text[j]) >= 0x20
                  && static_cast<unsigned char>(text[j]) <= 0x2F)
            {
                ++j;
            }
            i = j + 1;
            continue;
        }
        if(static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            ++i;
            continue;
        }
        if(c == '\x7F') {
            ++i;
            continue;
        }
        current += c;
        ++i;
    }
    flush();
    return runs;
}

}   // namespace uc_log::printer
