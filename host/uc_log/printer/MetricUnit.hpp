#pragma once

// A metric's unit as the printer reads it, for every front end and the control socket. uc_log::metric of an mp-units
// quantity sends the number in the quantity's own unit, named by mp_units::unit_symbol: a prefix ("c℃", "µs"), a
// product with spaces ("mA s"), powers ("℃⁻¹ s⁻¹", "m/s²"), one quotient ("1/℃", "s/℃"), a scaled unit in
// parentheses ("(10⁻³ 1/℃)", "(1/48 × 10⁻⁶ s) mA", "(1/100)"); the inline (ASCII) spelling "mdegC", "us", "s^-1",
// "(1/48 x 10^-6 s) mA". metric<"name", "unit", "scope"> takes any text by hand ("us", "1/s", "°C", "mA·s").
//
// What is read from it: the unit a person reads the values in, the factor that takes a value there and the digits
// after the point that keep an integer's resolution (`display`, `scale()`, `decimals`) - and the SI base units it is
// made of with their factor (`base`, `toBase`), so that two spellings of one quantity compare ("A·s" and "mA s",
// "Hz" and "1/s"). A text this does not read passes through: shown as it came, compared only with the same text.
//
// The shown unit depends on the text alone, never on a value, so a series, its numbers and a threshold share one:
// - one unit with an engineering prefix keeps it: µs, mA, mV, kHz stay; another prefix goes to the nearest
//   engineering one towards none (c, d, da, h: none), a power-of-ten scale likewise ("(10⁻³ s)" is ms);
// - a temperature (°C, K) reads in degrees: c℃ and m℃ show as °C with 2 and 3 decimals;
// - a product, a power or a quotient shows in its units without prefixes (mA s as A·s, m℃/s as °C/s, (10⁻³ 1/℃) as
//   1/°C), as does a scale that is no power of ten ((1/48 × 10⁻⁶ s) mA as A·s);
// - min, h, %, ‰, ppm, °, dB, dBm take no prefix and show as they are; ℃ is written °C, `·` joins a product;
// - at most six digits after the point (what a table's value column holds): a product with a prefix below µ (pA s)
//   shows rounded, the stream's `value` keeps the number as sent.

#include "uc_log/detail/Lifetimebound.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::printer {

struct MetricUnit {
    std::string text;      // as sent
    std::string display;   // the unit the values are shown in; `text` when it was not read
    // a value in `text` x scaleRest x 10^scaleExp10 is the value in `display`: a power of ten stays exact
    int    scaleExp10{};
    double scaleRest{1.0};
    int
      decimals{};   // digits after the point an integer in `text` keeps in `display` (c℃ in °C: 2)
    // the SI base units it is made of ("kg·m²·s⁻³·A⁻¹" for V, "" for a plain number; °C, rad, B, dB and dBm count as
    // bases of their own), `text` when it was not read: two units of one quantity have the same
    std::string base;
    double      toBase{1.0};   // a value in `text` x toBase is the value in `base`
    bool        known{};       // false: `text` was not read, a value shows as it came

    [[nodiscard]] double scale() const {
        return scaleExp10 >= 0 ? scaleRest * std::pow(10.0, scaleExp10)
                               : scaleRest / std::pow(10.0, -scaleExp10);
    }

    /// `value` (in `text`) in `display`; a power of ten divides, so 31250 c℃ is 312.5 °C to the bit.
    [[nodiscard]] double toDisplay(double value) const {
        auto const v = value * scaleRest;
        return scaleExp10 >= 0 ? v * std::pow(10.0, scaleExp10) : v / std::pow(10.0, -scaleExp10);
    }
};

namespace Detail::units {

    // the SI base units (ampere and kelvin spelt out: an enum constant is lowerCamel), and those this counts as bases of
    // their own - a °C is no K plus a number to compare by
    enum Base : std::uint8_t {
        kg,
        m,
        s,
        amp,
        kelvin,
        mol,
        cd,
        rad,
        degC,
        byte,
        dB,
        dBm,
        baseCount
    };

    inline constexpr std::array<std::string_view, baseCount>
      baseNames{"kg", "m", "s", "A", "K", "mol", "cd", "rad", "°C", "B", "dB", "dBm"};

    using Dims = std::array<std::int8_t, baseCount>;

    constexpr Dims dims(std::initializer_list<std::pair<Base,
                                                        int>> parts) {
        Dims d{};
        for(auto const& [b, p] : parts) { d[b] = static_cast<std::int8_t>(p); }
        return d;
    }

    struct Symbol {
        std::string_view text;
        std::string_view display;
        double           toBase;   // one of it in its base units
        Dims             dims;
        bool             prefixable;
        bool             degrees;   // shown without a prefix (a temperature)
        bool             time;      // after the other factors of a product (A·s)
    };

    // every spelling mp-units' unit_symbol prints for the units these firmwares use (utf8 and portable), the ones
    // written by hand, and their neighbours; a longer one before a shorter one it starts with is not needed: a symbol
    // is matched whole, with or without one prefix
    inline constexpr std::array symbols{
      Symbol{     "s",   "s",                      1.0,                              dims({{s, 1}}),  true, false,  true},
      Symbol{   "min", "min",                     60.0,                              dims({{s, 1}}), false, false,  true},
      Symbol{     "h",   "h",                   3600.0,                              dims({{s, 1}}), false, false,  true},
      Symbol{     "m",   "m",                      1.0,                              dims({{m, 1}}),  true, false, false},
      Symbol{     "g",   "g",                     1e-3,                             dims({{kg, 1}}),  true, false, false},
      Symbol{     "A",   "A",                      1.0,                            dims({{amp, 1}}),  true, false, false},
      Symbol{     "K",   "K",                      1.0,                         dims({{kelvin, 1}}),  true,  true, false},
      Symbol{    "°C",  "°C",                      1.0,                           dims({{degC, 1}}),  true,  true, false},
      Symbol{     "℃",  "°C",                      1.0,                           dims({{degC, 1}}),  true,  true, false},
      Symbol{  "degC",  "°C",                      1.0,                           dims({{degC, 1}}),  true,  true, false},
      Symbol{    "`C",  "°C",                      1.0,                           dims({{degC, 1}}),  true,  true, false},
      Symbol{   "mol", "mol",                      1.0,                            dims({{mol, 1}}),  true, false, false},
      Symbol{    "cd",  "cd",                      1.0,                             dims({{cd, 1}}),  true, false, false},
      Symbol{    "Hz",  "Hz",                      1.0,                             dims({{s, -1}}),  true, false, false},
      Symbol{     "N",   "N",                      1.0,            dims({{kg, 1}, {m, 1}, {s, -2}}),  true, false, false},
      Symbol{    "Pa",  "Pa",                      1.0,           dims({{kg, 1}, {m, -1}, {s, -2}}),  true, false, false},
      Symbol{     "J",   "J",                      1.0,            dims({{kg, 1}, {m, 2}, {s, -2}}),  true, false, false},
      Symbol{    "Wh",  "Wh",                   3600.0,            dims({{kg, 1}, {m, 2}, {s, -2}}),  true, false, false},
      Symbol{     "W",   "W",                      1.0,            dims({{kg, 1}, {m, 2}, {s, -3}}),  true, false, false},
      Symbol{    "VA",  "VA",                      1.0,            dims({{kg, 1}, {m, 2}, {s, -3}}),  true, false, false},
      Symbol{     "C",   "C",                      1.0,                    dims({{amp, 1}, {s, 1}}),  true, false, false},
      Symbol{     "V",   "V",                      1.0, dims({{kg, 1}, {m, 2}, {s, -3}, {amp, -1}}),  true, false, false},
      Symbol{     "F",   "F",                      1.0, dims({{kg, -1}, {m, -2}, {s, 4}, {amp, 2}}),  true, false, false},
      Symbol{     "Ω",   "Ω",                      1.0, dims({{kg, 1}, {m, 2}, {s, -3}, {amp, -2}}),  true, false, false},
      Symbol{"\u2126",   "Ω",                      1.0, dims({{kg, 1}, {m, 2}, {s, -3}, {amp, -2}}),  true, false, false},
      Symbol{   "ohm",   "Ω",                      1.0, dims({{kg, 1}, {m, 2}, {s, -3}, {amp, -2}}),  true, false, false},
      Symbol{     "S",   "S",                      1.0, dims({{kg, -1}, {m, -2}, {s, 3}, {amp, 2}}),  true, false, false},
      Symbol{     "L",   "L",                     1e-3,                              dims({{m, 3}}),  true, false, false},
      Symbol{   "rad", "rad",                      1.0,                            dims({{rad, 1}}),  true, false, false},
      Symbol{     "°",   "°", std::numbers::pi / 180.0,                            dims({{rad, 1}}), false, false, false},
      Symbol{   "deg",   "°", std::numbers::pi / 180.0,                            dims({{rad, 1}}), false, false, false},
      Symbol{     "%",   "%",                     1e-2,                                      Dims{}, false, false, false},
      Symbol{     "‰",   "‰",                     1e-3,                                      Dims{}, false, false, false},
      Symbol{    "%o",   "‰",                     1e-3,                                      Dims{}, false, false, false},
      Symbol{   "ppm", "ppm",                     1e-6,                                      Dims{}, false, false, false},
      Symbol{     "B",   "B",                      1.0,                           dims({{byte, 1}}),  true, false, false},
      Symbol{   "bit", "bit",                    0.125,                           dims({{byte, 1}}),  true, false, false},
      Symbol{    "dB",  "dB",                      1.0,                             dims({{dB, 1}}), false, false, false},
      Symbol{   "dBm", "dBm",                      1.0,                            dims({{dBm, 1}}), false, false, false},
    };

    struct Prefix {
        std::string_view text;
        int              exp10;
    };

    // the SI prefixes, µ (U+00B5, what mp-units prints) shown for 10⁻⁶; μ (U+03BC) and u read as it too
    inline constexpr std::array<Prefix, 26> prefixes{
      {{"Q", 30},  {"R", 27},  {"Y", 24},  {"Z", 21},  {"E", 18}, {"P", 15},  {"T", 12},
       {"G", 9},   {"M", 6},   {"k", 3},   {"h", 2},   {"da", 1}, {"d", -1},  {"c", -2},
       {"m", -3},  {"µ", -6},  {"μ", -6},  {"u", -6},  {"n", -9}, {"p", -12}, {"f", -15},
       {"a", -18}, {"z", -21}, {"y", -24}, {"r", -27}, {"q", -30}}
    };

    inline std::string_view prefixText(int exp10) {
        for(auto const& p : prefixes) {
            if(p.exp10 == exp10) { return p.text; }
        }
        return {};
    }

    struct Atom {
        std::size_t symbol{};
        int         prefixExp10{};
    };

    /// A symbol, with or without one prefix (a prefixable one); none for anything else.
    inline std::optional<Atom> atomOf(std::string_view text) {
        for(std::size_t i = 0; i < symbols.size(); ++i) {
            if(symbols[i].text == text) { return Atom{i, 0}; }
        }
        for(auto const& p : prefixes) {
            if(text.size() <= p.text.size() || !text.starts_with(p.text)) { continue; }
            auto const rest = text.substr(p.text.size());
            for(std::size_t i = 0; i < symbols.size(); ++i) {
                if(symbols[i].prefixable && symbols[i].text == rest) { return Atom{i, p.exp10}; }
            }
        }
        return std::nullopt;
    }

    // a code point and its bytes; a broken byte is U+FFFD of one byte (it ends up in a symbol nobody knows)
    struct CodePoint {
        char32_t    value{};
        std::size_t bytes{};
    };

    inline CodePoint decode(std::string_view text,
                            std::size_t      at) {
        if(at >= text.size()) { return {}; }
        auto const  byte = [&](std::size_t i) { return static_cast<unsigned char>(text[at + i]); };
        auto const  lead = byte(0);
        std::size_t n    = lead < 0x80U            ? 1
                         : (lead & 0xE0U) == 0xC0U ? 2
                         : (lead & 0xF0U) == 0xE0U ? 3
                                                   : 4;
        if((lead & 0xC0U) == 0x80U || (lead & 0xF8U) == 0xF8U || at + n > text.size()) {
            return {0xFFFD, 1};
        }
        char32_t v = n == 1 ? lead
                   : n == 2 ? (lead & 0x1FU)
                   : n == 3 ? (lead & 0x0FU)
                            : (lead & 0x07U);
        for(std::size_t i = 1; i < n; ++i) {
            if((byte(i) & 0xC0U) != 0x80U) { return {0xFFFD, 1}; }
            v = (v << 6U) | (byte(i) & 0x3FU);
        }
        return {v, n};
    }

    // ' ', '·' (U+00B7), '⋅' (U+22C5, mp-units' half-high dot), '∙', '*', '×' (between a magnitude's parts)
    constexpr bool separator(char32_t c) {
        return c == U' ' || c == U'\t' || c == U'·' || c == U'⋅' || c == U'∙' || c == U'*'
            || c == U'×';
    }

    constexpr std::optional<int> superscriptDigit(char32_t c) {
        switch(c) {
        case U'⁰': return 0;
        case U'¹': return 1;
        case U'²': return 2;
        case U'³': return 3;
        default:   break;
        }
        if(c >= U'⁴' && c <= U'⁹') { return static_cast<int>(c - U'⁴') + 4; }
        return std::nullopt;
    }

    constexpr bool superscript(char32_t c) {
        return superscriptDigit(c).has_value() || c == U'⁻' || c == U'⁺';
    }

    // a NUL byte too: it ends up in a symbol nobody knows, not as the text's end
    constexpr bool symbolChar(char32_t c) {
        return !separator(c) && c != U'/' && c != U'(' && c != U')' && c != U'^' && !superscript(c);
    }

    // in a product the coherent unit of mass is the kilogram: a gram shows as one
    constexpr bool gram(Symbol const& sym) { return sym.text == "g"; }

    inline constexpr int maxPower    = 99;
    inline constexpr int maxExp10    = 400;
    inline constexpr int maxDepth    = 8;
    inline constexpr int maxTextLen  = 128;
    inline constexpr int maxDecimals = 6;   // what the shown form keeps: a value column is 11 cells

    struct Factor {
        std::size_t symbol{};
        int         power{};
    };

    /// rest x 10^exp10 x the factors.
    struct Term {
        std::vector<Factor> factors;   // each symbol once, in the order it came
        int                 exp10{};
        double              rest{1.0};

        /// false: a power past maxPower
        [[nodiscard]] bool add(std::size_t  symbol,
                               std::int64_t power) {
            for(auto& f : factors) {
                if(f.symbol == symbol) { return bounded(f.power, f.power + power, maxPower); }
            }
            factors.push_back({symbol, 0});
            return bounded(factors.back().power, power, maxPower);
        }

        /// this x other^power; false: a power or a scale past its bound (nested groups multiply them)
        [[nodiscard]] bool multiply(Term const& other,
                                    int         power) {
            if(!bounded(exp10, exp10 + (std::int64_t{other.exp10} * power), maxExp10)) {
                return false;
            }
            rest *= std::pow(other.rest, power);
            return std::ranges::all_of(other.factors, [&](Factor const& f) {
                return add(f.symbol, std::int64_t{f.power} * power);
            });
        }

    private:
        static bool bounded(int&         to,
                            std::int64_t value,
                            int          bound) {
            if(value > bound || value < -bound) { return false; }
            to = static_cast<int>(value);
            return true;
        }
    };

    struct Times {};   // a standalone "x": the portable times of a magnitude

    class Reader {
    public:
        explicit Reader(std::string_view text UC_LOG_LIFETIMEBOUND) : text_{text} {}

        [[nodiscard]] bool atEnd() const { return at_ >= text_.size(); }

        /// 0 at the end
        [[nodiscard]] char32_t peek() const { return decode(text_, at_).value; }

        void next() { at_ += std::max<std::size_t>(decode(text_, at_).bytes, 1); }

        void skipSeparators() {
            while(!atEnd() && separator(peek())) { next(); }
        }

        std::string_view symbol() {
            auto const from = at_;
            while(!atEnd() && symbolChar(peek())) { next(); }
            return text_.substr(from, at_ - from);
        }

        /// "^2", "^-1", "²", "⁻¹"; 1 when none follows, none when one is broken or a fraction ("^(1/2)")
        std::optional<int> power() {
            int sign = 1;
            if(peek() == U'^') {
                next();
                if(peek() == U'-' || peek() == U'−') {
                    sign = -1;
                    next();
                } else if(peek() == U'+') {
                    next();
                }
                return digits(sign, [](char32_t c) -> std::optional<int> {
                    if(c >= U'0' && c <= U'9') { return static_cast<int>(c - U'0'); }
                    return std::nullopt;
                });
            }
            if(!superscript(peek())) { return 1; }
            if(peek() == U'⁻') {
                sign = -1;
                next();
            } else if(peek() == U'⁺') {
                next();
            }
            return digits(sign, superscriptDigit);
        }

        /// an expression up to its end or, `inGroup`, its ')'
        std::optional<Term> expression(bool inGroup,
                                       int  depth) {
            if(depth > maxDepth) { return std::nullopt; }
            Term t;
            bool divide = false;
            bool times  = false;   // after an "x": another item must follow
            bool any    = false;
            while(true) {
                skipSeparators();
                if(atEnd()) {
                    if(inGroup) { return std::nullopt; }
                    break;
                }
                auto const c = peek();
                if(c == U')') {
                    if(!inGroup) { return std::nullopt; }
                    break;
                }
                if(c == U'/') {
                    if(divide || times) { return std::nullopt; }
                    divide = true;
                    next();
                    continue;
                }
                auto item = this->item(depth);
                if(!item) { return std::nullopt; }
                if(std::holds_alternative<Times>(*item)) {
                    if(divide || times || !any) { return std::nullopt; }
                    times = true;
                    continue;
                }
                if(!t.multiply(std::get<Term>(*item), divide ? -1 : 1)) { return std::nullopt; }
                divide = false;
                times  = false;
                any    = true;
            }
            if(divide || times) { return std::nullopt; }
            return t;
        }

    private:
        template<typename DigitOf>
        std::optional<int> digits(int            sign,
                                  DigitOf const& digitOf) {
            int  n   = 0;
            bool any = false;
            while(auto const d = digitOf(peek())) {
                n = (n * 10) + *d;
                if(n > maxPower) { return std::nullopt; }
                any = true;
                next();
            }
            if(!any || peek() == U'(') { return std::nullopt; }
            return sign * n;
        }

        // a number of a magnitude: "1", "10", "48", "32768", "1.5"; a 1 followed by zeros as a power of ten
        static std::optional<Term> number(std::string_view text,
                                          int              power) {
            if(!std::ranges::all_of(text,
                                    [](char ch) { return (ch >= '0' && ch <= '9') || ch == '.'; })
               || std::ranges::count(text, '.') > 1 || text.front() == '.' || text.back() == '.')
            {
                return std::nullopt;
            }
            Term t;
            if(text.front() == '1' && text.find_first_not_of('0', 1) == std::string_view::npos) {
                Term ten;
                ten.exp10 = static_cast<int>(text.size() - 1);
                if(!t.multiply(ten, power)) { return std::nullopt; }
                return t;
            }
            double     value{};
            auto const end = std::to_address(text.end());
            if(auto const [ptr, ec] = std::from_chars(std::to_address(text.begin()), end, value);
               ec != std::errc{} || ptr != end || value == 0.0)
            {
                return std::nullopt;
            }
            t.rest = std::pow(value, power);
            return t;
        }

        std::optional<std::variant<Term,
                                   Times>>
        item(int depth) {
            if(peek() == U'(') {
                next();
                auto inner = expression(true, depth + 1);
                if(!inner || peek() != U')') { return std::nullopt; }
                next();
                auto const p = power();
                if(!p) { return std::nullopt; }
                Term t;
                if(!t.multiply(*inner, *p)) { return std::nullopt; }
                return t;
            }
            auto const text = symbol();
            if(text.empty()) { return std::nullopt; }
            if(text == "x") { return Times{}; }
            auto const p = power();
            if(!p) { return std::nullopt; }
            if(auto n = number(text, *p)) { return *n; }
            auto const atom = atomOf(text);
            if(!atom) { return std::nullopt; }
            Term t;
            t.exp10 = atom->prefixExp10 * *p;
            if(!t.add(atom->symbol, *p)) { return std::nullopt; }
            return t;
        }

        std::string_view text_;
        std::size_t      at_{};
    };

    inline std::string superscriptOf(int power) {
        static constexpr std::array<std::string_view, 10>
                    digits{"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
        std::string out = power < 0 ? "⁻" : "";
        auto const  n   = std::to_string(power < 0 ? -power : power);
        for(char const d : n) { out += digits[static_cast<std::size_t>(d - '0')]; }
        return out;
    }

    inline std::string piece(std::string_view text,
                             int              power) {
        return power == 1 ? std::string{text} : std::string{text} + superscriptOf(power);
    }

    inline std::string joined(std::vector<std::string> const& parts) {
        std::string out;
        for(auto const& p : parts) {
            if(!out.empty()) { out += "·"; }
            out += p;
        }
        return out;
    }

    /// "A·s", "°C/s", "1/°C", "1/(°C·s)", "m/s²", "kg·m²" - the factors without prefixes, time last in a product
    inline std::string composed(std::vector<Factor> const& factors) {
        std::vector<std::string> numerator;
        std::vector<std::string> times;
        std::vector<std::string> denominator;
        for(auto const& f : factors) {
            auto const& sym  = symbols[f.symbol];
            auto const  text = gram(sym) ? std::string_view{"kg"} : sym.display;
            if(f.power > 0) {
                (sym.time ? times : numerator).push_back(piece(text, f.power));
            } else {
                denominator.push_back(piece(text, -f.power));
            }
        }
        numerator.insert(numerator.end(), times.begin(), times.end());
        if(denominator.empty()) { return joined(numerator); }
        auto const top = joined(numerator);
        auto const bottom
          = denominator.size() == 1 ? denominator.front() : "(" + joined(denominator) + ")";
        return (top.empty() ? std::string{"1"} : top) + "/" + bottom;
    }

    inline std::string baseText(Dims const& d) {
        std::vector<std::string> parts;
        for(std::size_t b = 0; b < baseCount; ++b) {
            if(d[b] != 0) { parts.push_back(piece(baseNames[b], d[b])); }
        }
        return joined(parts);
    }

    inline MetricUnit notRead(std::string_view text) {
        return MetricUnit{.text       = std::string{text},
                          .display    = std::string{text},
                          .scaleExp10 = 0,
                          .scaleRest  = 1.0,
                          .decimals   = 0,
                          .base       = std::string{text},
                          .toBase     = 1.0,
                          .known      = false};
    }

}   // namespace Detail::units

/// What `text` means (the rules: this file's head); a text that is not read gives `known` false and itself as
/// display and base.
inline MetricUnit readMetricUnit(std::string_view text) {
    namespace u = Detail::units;
    if(text.size() > u::maxTextLen) { return u::notRead(text); }
    u::Reader  reader{text};
    auto const parsed = reader.expression(false, 0);
    if(!parsed) { return u::notRead(text); }
    std::vector<u::Factor> factors;
    for(auto const& f : parsed->factors) {
        if(f.power != 0) { factors.push_back(f); }
    }

    u::Dims dims{};
    double  toBase = parsed->rest * std::pow(10.0, parsed->exp10);
    for(auto const& f : factors) {
        auto const& sym = u::symbols[f.symbol];
        for(std::size_t b = 0; b < u::baseCount; ++b) {
            auto const p = dims[b] + (sym.dims[b] * f.power);
            if(p > u::maxPower || p < -u::maxPower) { return u::notRead(text); }
            dims[b] = static_cast<std::int8_t>(p);
        }
        toBase *= std::pow(sym.toBase, f.power);
    }
    if(!std::isfinite(toBase) || toBase == 0.0 || !std::isfinite(parsed->rest)
       || parsed->rest == 0.0)
    {
        return u::notRead(text);
    }

    MetricUnit unit;
    unit.text          = std::string{text};
    unit.base          = u::baseText(dims);
    unit.toBase        = toBase;
    unit.known         = true;
    bool const decimal = parsed->rest == 1.0;
    if(factors.size() == 1 && factors.front().power == 1 && decimal) {
        auto const& sym = u::symbols[factors.front().symbol];
        if(sym.prefixable && !sym.degrees) {
            // the engineering prefix towards none: c, d, da, h go, a power of ten becomes one
            auto const shown = std::clamp(3 * (parsed->exp10 / 3), -30, 30);
            unit.display     = std::string{u::prefixText(shown)} + std::string{sym.display};
            unit.scaleExp10  = parsed->exp10 - shown;
        } else {
            unit.display    = std::string{sym.display};
            unit.scaleExp10 = parsed->exp10;
        }
    } else {
        unit.display    = u::composed(factors);
        unit.scaleExp10 = parsed->exp10;
        unit.scaleRest  = parsed->rest;
        for(auto const& f : factors) {
            if(u::gram(u::symbols[f.symbol])) { unit.scaleExp10 -= 3 * f.power; }
        }
    }
    if(!std::isfinite(unit.scale()) || unit.scale() == 0.0) { return u::notRead(text); }
    if(decimal) {
        unit.decimals = std::clamp(-unit.scaleExp10, 0, u::maxDecimals);
    } else {
        // a scale that is no power of ten (mA x a 48 MHz tick): enough for it, maxDecimals at most
        auto const digits = std::ceil(-std::log10(unit.scale()) - 1e-9);
        unit.decimals     = static_cast<int>(std::clamp(digits, 0.0, double{u::maxDecimals}));
    }
    return unit;
}

namespace Detail::units {
    struct TextHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view text) const {
            return std::hash<std::string_view>{}(text);
        }
    };
}   // namespace Detail::units

/// Units read once each, for whoever turns many values of a few units (a stream of samples): not shared between
/// threads, each owner keeps its own. A firmware has a few dozen units; past `maxKept` (a fuzzer's) it starts anew.
class MetricUnits {
public:
    static constexpr std::size_t maxKept = 1024;

    /// readMetricUnit(text), valid until the next call
    MetricUnit const& of(std::string_view text) UC_LOG_LIFETIMEBOUND {
        if(auto const it = units_.find(text); it != units_.end()) { return it->second; }
        if(units_.size() >= maxKept) { units_.clear(); }
        return units_.emplace(std::string{text}, readMetricUnit(text)).first->second;
    }

private:
    std::unordered_map<std::string, MetricUnit, Detail::units::TextHash, std::equal_to<>> units_;
};

/// A value that came as an integer: the unit's decimals are all it has.
inline bool integralMetricValue(double value) {
    return std::isfinite(value) && std::trunc(value) == value && std::abs(value) < 9.0e15;
}

/// Digits after the point for `shown`, a value in a unit's display: one that came as an integer (`integral`) keeps
/// the unit's `decimals`; any other up to four significant digits and no more than it needs, at least the unit's.
inline int metricDecimals(double shown,
                          int    decimals,
                          bool   integral) {
    if(integral || !std::isfinite(shown) || shown == 0.0) { return decimals; }
    auto const significant
      = std::clamp(3 - static_cast<int>(std::floor(std::log10(std::abs(shown)))), 0, 9);
    auto const most = std::max(decimals, significant);
    for(int d = decimals; d < most; ++d) {
        auto const scaled = shown * std::pow(10.0, d);
        if(std::abs(scaled - std::round(scaled)) <= 1e-9 * std::max(1.0, std::abs(scaled))) {
            return d;
        }
    }
    return most;
}

/// `value` (in the unit as sent) as a number in its display unit: "312.50" for 31250 c℃.
inline std::string formatMetricNumber(double            value,
                                      MetricUnit const& unit) {
    auto shown = unit.toDisplay(value);
    if(shown == 0.0) { shown = 0.0; }   // no "-0.00"
    return fmt::format("{:.{}f}",
                       shown,
                       metricDecimals(shown, unit.decimals, integralMetricValue(value)));
}

/// The same with its display unit: "312.50 °C", "160.000 A·s", "42" for a plain number.
inline std::string formatMetricValue(double            value,
                                     MetricUnit const& unit) {
    auto number = formatMetricNumber(value, unit);
    if(unit.display.empty()) { return number; }
    return number + (unit.display == "°" ? "" : " ") + unit.display;
}

}   // namespace uc_log::printer
