// A metric's unit as the printer reads it (printer/MetricUnit.hpp): the texts mp-units' unit_symbol prints (utf8 and
// the inline strings' portable spelling) and the ones written by hand, the unit each is shown in, its factor and
// decimals, its SI base units for comparing, and the texts that pass through unread. The quantities first are those
// of an integer firmware: a temperature in 0.01 °C, a charge in mA s and in mA x a 48 MHz tick, gains in thousandths
// of 1/°C.
#include "support/CheckFmt.hpp"
#include "uc_log/printer/MetricUnit.hpp"

#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
using uc_log::printer::formatMetricNumber;
using uc_log::printer::formatMetricValue;
using uc_log::printer::MetricUnits;
using uc_log::printer::readMetricUnit;

bool near(double a,
          double b) {
    return std::abs(a - b) <= 1e-12 * std::max(std::abs(a), std::abs(b));
}

std::string shown(std::string_view unit,
                  double           value) {
    return formatMetricValue(value, readMetricUnit(unit));
}

// what an integer firmware sends, as mp-units 2.5 prints it (uc_log's metric with the utf8 character set)
void integerFirmware() {
    auto const centi = readMetricUnit("c℃");
    CHECK(centi.known && centi.display == "°C" && centi.decimals == 2 && centi.base == "°C",
          "c℃: shown in °C, two decimals");
    CHECK(centi.toDisplay(31250) == 312.5,
          "31250 c℃ is 312.5 °C to the bit (a power of ten divides)");
    CHECK_EQ(shown("c℃", 31250), std::string{"312.50 °C"}, "c℃ shown");
    CHECK_EQ(shown("c℃", -125), std::string{"-1.25 °C"}, "a negative difference");
    CHECK_EQ(shown("m℃", 58300), std::string{"58.300 °C"}, "m℃: three decimals");
    CHECK_EQ(shown("µ℃", 178000), std::string{"0.178000 °C"}, "µ℃ (a gain per count): six");
    CHECK_EQ(shown("d℃", 3125), std::string{"312.5 °C"}, "d℃: one");

    auto const charge = readMetricUnit("mA s");
    CHECK(charge.known && charge.display == "A·s" && charge.decimals == 3
            && near(charge.toBase, 1e-3),
          "mA s: a product, shown without prefixes in A·s");
    CHECK_EQ(shown("mA s", 160000), std::string{"160.000 A·s"}, "160000 mA s");
    CHECK_EQ(shown("mA", 850), std::string{"850 mA"}, "one unit keeps an engineering prefix");
    CHECK_EQ(shown("µs", 600), std::string{"600 µs"}, "600 µs stays µs");
    CHECK_EQ(shown("us", 600), std::string{"600 µs"}, "\"us\" by hand is µs");
    CHECK_EQ(shown("‰", 250), std::string{"250 ‰"}, "‰");

    // the PID's gains in thousandths: mp-units' scaled-unit notation in parentheses
    auto const kp = readMetricUnit("(10⁻³ 1/℃)");
    CHECK(kp.known && kp.display == "1/°C" && kp.decimals == 3 && near(kp.toBase, 1e-3)
            && kp.base == "°C⁻¹",
          "Kp (10⁻³ 1/℃): 1/°C, three decimals");
    CHECK_EQ(shown("(10⁻³ 1/℃)", 2500), std::string{"2.500 1/°C"}, "Kp shown");
    CHECK_EQ(readMetricUnit("(10⁻³ ℃⁻¹ s⁻¹)").display,
             std::string{"1/(°C·s)"},
             "Ki: two denominators");
    CHECK_EQ(readMetricUnit("(10⁻³ s/℃)").display, std::string{"s/°C"}, "Kd");
    CHECK_EQ(shown("(1/100)", 300),
             std::string{"3.00"},
             "hundredths of one: a number with two decimals");

    // a charge in mA x one tick of a 48 MHz clock: a scale that is no power of ten, shown in A·s
    auto const ticks = readMetricUnit("(1/48 × 10⁻⁶ s) mA");
    CHECK(ticks.known && ticks.display == "A·s" && near(ticks.toBase, 1e-3 / 48e6)
            && ticks.decimals == 6 && ticks.base == charge.base,
          "mA x a 48 MHz tick: A·s, six decimals at most, the same quantity as mA s");
    CHECK_EQ(shown("(1/48 × 10⁻⁶ s) mA", 160.0 * 48e9),
             std::string{"160.000000 A·s"},
             "160 A·s in ticks");
    auto const q15 = readMetricUnit("(1/32768 c℃)");
    CHECK(q15.known && q15.display == "°C" && near(q15.toBase, 0.01 / 32768) && q15.decimals == 6,
          "a Q15 of centidegrees: °C");

    // by hand, as a firmware's main writes them
    CHECK(readMetricUnit("1/s").display == "1/s" && readMetricUnit("°C").display == "°C"
            && readMetricUnit("mA·s").display == "A·s",
          "1/s, °C, mA·s by hand");
}

// the SI prefixes, as unit_symbol prints them, and which one a single unit is shown with
void prefixes() {
    struct Case {
        std::string_view text;
        double           toBase;
    };

    for(auto const& [text, toBase] :
        std::vector<Case>{
          { "qs", 1e-30},
          { "rs", 1e-27},
          { "ys", 1e-24},
          { "zs", 1e-21},
          { "as", 1e-18},
          { "fs", 1e-15},
          { "ps", 1e-12},
          { "ns",  1e-9},
          { "µs",  1e-6},
          { "μs",  1e-6},
          { "us",  1e-6},
          { "ms",  1e-3},
          { "cs",  1e-2},
          { "ds",  1e-1},
          {"das",   1e1},
          { "hs",   1e2},
          { "ks",   1e3},
          { "Ms",   1e6},
          { "Gs",   1e9},
          { "Ts",  1e12},
          { "Ps",  1e15},
          { "Es",  1e18},
          { "Zs",  1e21},
          { "Ys",  1e24},
          { "Rs",  1e27},
          { "Qs",  1e30}
    })
    {
        auto const u = readMetricUnit(text);
        CHECKF(u.known && u.base == "s" && near(u.toBase, toBase),
               "%.*s",
               static_cast<int>(text.size()),
               text.data());
    }
    CHECK(readMetricUnit("ms").display == "ms" && readMetricUnit("ns").display == "ns"
            && readMetricUnit("kHz").display == "kHz" && readMetricUnit("MHz").display == "MHz"
            && readMetricUnit("kΩ").display == "kΩ" && readMetricUnit("mV").display == "mV",
          "an engineering prefix stays");
    CHECK(readMetricUnit("cs").display == "s" && readMetricUnit("cs").decimals == 2
            && readMetricUnit("hPa").display == "Pa" && readMetricUnit("hPa").decimals == 0
            && near(readMetricUnit("hPa").scale(), 100) && readMetricUnit("dam").display == "m",
          "c, d, da, h: towards none");
    CHECK(readMetricUnit("(10⁻⁵ s)").display == "ms" && readMetricUnit("(10⁻⁵ s)").decimals == 2
            && readMetricUnit("(10³ m)").display == "km"
            && readMetricUnit("(10⁻³ s)").display == "ms",
          "a power-of-ten scale: the engineering prefix towards none");
    CHECK(readMetricUnit("mK").display == "K" && readMetricUnit("mK").decimals == 3,
          "a temperature reads in degrees");
    CHECK(readMetricUnit("(10⁻⁶ s) mA").display == "A·s"
            && readMetricUnit("(10⁻⁶ s) mA").decimals == 6 && readMetricUnit("pA s").decimals == 6,
          "at most six digits after the point");
    CHECK(readMetricUnit("kg").display == "kg" && near(readMetricUnit("kg").toBase, 1.0)
            && near(readMetricUnit("g").toBase, 1e-3),
          "the gram is prefixed, the kilogram is the base");
    // a symbol is matched whole before a prefix is tried
    CHECK(readMetricUnit("min").base == "s" && near(readMetricUnit("min").toBase, 60)
            && readMetricUnit("Pa").base == "kg·m⁻¹·s⁻²" && readMetricUnit("cd").base == "cd"
            && readMetricUnit("mol").base == "mol" && readMetricUnit("h").base == "s"
            && readMetricUnit("dB").base == "dB" && readMetricUnit("dBm").base == "dBm"
            && readMetricUnit("m").base == "m" && readMetricUnit("mm").base == "m",
          "min, Pa, cd, mol, h, dB, dBm, m whole; mm is a prefix and a symbol");
    CHECK(!readMetricUnit("k%").known && !readMetricUnit("mmin").known
            && !readMetricUnit("kdB").known && !readMetricUnit("KB").known
            && !readMetricUnit("mmm").known,
          "no prefix on %, min, dB; K is kelvin, no prefix; one prefix only");
}

// products, quotients, powers: unit_symbol's forms (space, ⋅, solidus, superscripts) and the ones by hand
void compounds() {
    for(auto const text : {"mA s", "mA·s", "mA⋅s", "mA*s", "mA  s", "s mA", "(10⁻³ A) s"}) {
        auto const u = readMetricUnit(text);
        CHECKF(u.known && u.display == "A·s" && near(u.toBase, 1e-3), "%s is mA·s", text);
    }
    CHECK(readMetricUnit("Hz").base == readMetricUnit("1/s").base
            && readMetricUnit("1/s").base == readMetricUnit("s⁻¹").base
            && readMetricUnit("s^-1").display == "1/s",
          "Hz, 1/s, s⁻¹, s^-1: one quantity");
    CHECK(readMetricUnit("C").base == readMetricUnit("A s").base
            && readMetricUnit("V").base == readMetricUnit("W/A").base
            && readMetricUnit("J").base == readMetricUnit("W s").base
            && readMetricUnit("Ω").base == readMetricUnit("V/A").base
            && readMetricUnit("ohm").base == readMetricUnit("Ω").base
            && near(readMetricUnit("kWh").toBase, 3.6e6),
          "derived units compare by their base units");
    CHECK(readMetricUnit("°C").base != readMetricUnit("K").base,
          "°C and K do not compare (a point on each scale is another number)");
    CHECK(readMetricUnit("m/s²").display == "m/s²" && readMetricUnit("m/s^2").display == "m/s²"
            && readMetricUnit("m²").display == "m²" && readMetricUnit("A^2").display == "A²"
            && readMetricUnit("m m").display == "m²"
            && readMetricUnit("kg m/s²").display == "kg·m/s²",
          "powers as superscripts");
    CHECK(readMetricUnit("℃/s").display == "°C/s" && readMetricUnit("m℃/s").display == "°C/s"
            && readMetricUnit("m℃/s").decimals == 3 && readMetricUnit("mK/s").display == "K/s",
          "a rate of a temperature: no prefix");
    CHECK(readMetricUnit("J/(kg K)").display == "J/(kg·K)"
            && readMetricUnit("J/(kg K)").scaleExp10 == 0,
          "in a product the kilogram stays the kilogram");
    CHECK(readMetricUnit("s/℃").display == "s/°C" && readMetricUnit("1/℃").display == "1/°C"
            && readMetricUnit("℃⁻¹ s⁻¹").display == "1/(°C·s)"
            && readMetricUnit("1/(℃ s)").display == "1/(°C·s)",
          "one denominator, two, the solidus always");
    CHECK(readMetricUnit("1/min").display == "1/min"
            && near(readMetricUnit("1/min").toBase, 1.0 / 60),
          "a named multiple stays itself in a quotient");
}

// the inline (ASCII) strings: mp-units' portable symbols, uc_log's "deg" for the backtick
void portableSpelling() {
    CHECK(readMetricUnit("mdegC").display == "°C" && readMetricUnit("mdegC").decimals == 3,
          "mdegC");
    CHECK(readMetricUnit("c`C").display == "°C" && readMetricUnit("degC").display == "°C",
          "`C, degC");
    CHECK(readMetricUnit("(10^-3 1/degC)").display == "1/°C", "(10^-3 1/degC)");
    auto const ticks = readMetricUnit("(1/48 x 10^-6 s) mA");
    CHECK(ticks.known && near(ticks.toBase, 1e-3 / 48e6), "(1/48 x 10^-6 s) mA");
    CHECK(readMetricUnit("%o").display == "‰" && readMetricUnit("ohm").display == "Ω"
            && readMetricUnit("us").display == "µs" && readMetricUnit("deg").display == "°",
          "%o, ohm, us, deg");
}

// non-SI units these firmwares log: no prefix, shown as they are, compared through their factor
void namedUnits() {
    CHECK(near(readMetricUnit("%").toBase, 1e-2) && near(readMetricUnit("‰").toBase, 1e-3)
            && near(readMetricUnit("ppm").toBase, 1e-6) && readMetricUnit("%").base.empty()
            && readMetricUnit("").base.empty(),
          "%, ‰, ppm: plain numbers with a factor");
    CHECK(readMetricUnit("h").display == "h" && near(readMetricUnit("h").toBase, 3600), "h");
    CHECK_EQ(shown("%", 50), std::string{"50 %"}, "a space before %");
    CHECK_EQ(shown("°", 45), std::string{"45°"}, "none before °");
    CHECK_EQ(shown("", 42), std::string{"42"}, "no unit: the number alone");
    auto const none = readMetricUnit("");
    CHECK(none.known && none.display.empty() && none.decimals == 0,
          "an empty unit is a plain number");
}

// what is not read passes through: shown as it came, its own base
void notRead() {
    for(auto const text : {"half-cycles",
                           "counts",
                           "KB",
                           "T",
                           "1//s",
                           "s)",
                           "(s",
                           "^2",
                           "s^(1/2)",
                           "x",
                           "s x",
                           "x s",
                           "/",
                           "s/",
                           "π rad",
                           "(π/180 rad)",
                           "s^999",
                           "mA s/",
                           "(((((((((s)))))))))",
                           "((((s^99)^99)^99)^99)"})
    {
        auto const u = readMetricUnit(text);
        CHECKF(!u.known && u.display == text && u.base == text && u.scale() == 1.0
                 && u.decimals == 0,
               "%s passes through",
               text);
        CHECKF(formatMetricValue(7, u) == std::string{"7 "} + text, "%s: shown as it came", text);
    }
    CHECK(!readMetricUnit(std::string(200, 's')).known, "an over-long text");
    CHECK(!readMetricUnit("m\xFF").known && !readMetricUnit("\xE2\x84").known, "broken UTF-8");
    CHECK(!readMetricUnit(std::string_view{"s\0x", 3}).known, "a NUL inside is no end");
}

// how many digits after the point: an integer as sent keeps the unit's, anything else up to four significant
void numbers() {
    auto const centi = readMetricUnit("c℃");
    CHECK_EQ(formatMetricNumber(31250, centi),
             std::string{"312.50"},
             "an integer: the unit's decimals");
    CHECK_EQ(formatMetricNumber(31250.4, centi),
             std::string{"312.50"},
             "a mean: at least the unit's");
    CHECK_EQ(formatMetricNumber(0, centi), std::string{"0.00"}, "zero");
    CHECK_EQ(formatMetricNumber(-0.0, centi), std::string{"0.00"}, "no minus zero");
    auto const volts = readMetricUnit("V");
    CHECK_EQ(formatMetricNumber(1.5, volts), std::string{"1.5"}, "a float: the digits it has");
    CHECK_EQ(formatMetricNumber(0.8414709848, volts), std::string{"0.8415"}, "four significant");
    CHECK_EQ(formatMetricNumber(1234.5678, volts),
             std::string{"1235"},
             "four significant, none after the point");
    CHECK_EQ(formatMetricNumber(2.5e-7, volts),
             std::string{"0.00000025"},
             "small: the digits it needs");
    CHECK_EQ(formatMetricNumber(600.4, readMetricUnit("µs")),
             std::string{"600.4"},
             "a mean of integers");
    CHECK(formatMetricNumber(std::nan(""), volts) == "nan", "not a number");
}

// a unit's own display reads back as itself, factor 1
void displayReadsBack() {
    for(auto const text : {"c℃",
                           "mA s",
                           "µs",
                           "(10⁻³ 1/℃)",
                           "(10⁻³ ℃⁻¹ s⁻¹)",
                           "(10⁻³ s/℃)",
                           "(1/48 × 10⁻⁶ s) mA",
                           "m/s²",
                           "kg m/s²",
                           "1/s",
                           "Hz",
                           "kΩ",
                           "%",
                           "‰",
                           "ppm",
                           "min",
                           "°",
                           "m℃/s",
                           "J/(kg K)",
                           "A^2",
                           "1/min",
                           ""})
    {
        auto const u    = readMetricUnit(text);
        auto const back = readMetricUnit(u.display);
        CHECKF(back.known && back.display == u.display && back.scale() == 1.0 && back.base == u.base
                 && near(back.toBase * u.scale(), u.toBase),
               "%s -> %s reads back",
               text,
               u.display.c_str());
    }
}

// a stream's units, read once each
void keptUnits() {
    MetricUnits units;
    auto const* first = &units.of("c℃");
    CHECK(first == &units.of("c℃") && first->display == "°C",
          "the same text: the same unit, read once");
    for(std::size_t i = 0; i < MetricUnits::maxKept + 3; ++i) {
        (void)units.of(std::to_string(i) + "x");
    }
    CHECK(units.of("mA s").display == "A·s", "past maxKept it starts anew and still reads");
}

// any bytes: read or passed through, never a crash, a known one finite (deterministic: a fixed seed)
void anyText() {
    std::mt19937                        gen{0x5eedU};
    std::uniform_int_distribution<int>  length{0, 24};
    std::vector<std::string_view> const parts{
      "m", "s", "A",  "℃", "°C",   "/",    "(",   ")", " ",  "·",  "⋅",
      "^", "-", "2",  "⁻", "¹",    "²",    "10",  "1", "×",  "x",  "µ",
      "k", "h", "da", "%", "\xFF", "\xE2", "min", "g", "Pa", "dB", "."};
    std::uniform_int_distribution<std::size_t> pick{0, parts.size() - 1};
    std::size_t                                known = 0;
    for(int round = 0; round < 20000; ++round) {
        std::string text;
        for(int n = length(gen); n > 0; --n) { text += parts[pick(gen)]; }
        auto const u = readMetricUnit(text);
        if(u.known) {
            ++known;
            CHECKF(std::isfinite(u.toBase) && u.toBase != 0.0 && std::isfinite(u.scale())
                     && u.decimals >= 0 && u.decimals <= 6,
                   "%s: a finite factor",
                   text.c_str());
        } else {
            CHECKF(u.display == text && u.base == text, "%s: passed through", text.c_str());
        }
    }
    CHECK(known > 100, "some of them read");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               { "integerFirmware",  integerFirmware},
                               {        "prefixes",         prefixes},
                               {       "compounds",        compounds},
                               {"portableSpelling", portableSpelling},
                               {      "namedUnits",       namedUnits},
                               {         "notRead",          notRead},
                               {         "numbers",          numbers},
                               {"displayReadsBack", displayReadsBack},
                               {       "keptUnits",        keptUnits},
                               {         "anyText",          anyText},
    });
}
