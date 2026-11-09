// The front-end-neutral metric view (MetricView.hpp): a series' numbers and its running tally, the braille plot (its
// dots, a flat line, a range that ends at the newest value), the value and rate format; the store's summaries
// (EntryStore.hpp); the metrics' table (MetricTable.hpp): its scopes, folds and words, the mark's moves, the columns
// lined up, the trend. No window, no clock: the times are made up.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/MetricTable.hpp"
#include "uc_log/printer/MetricView.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace uc_log::printer;

namespace {

std::vector<MetricEntry> series(std::vector<double> const& values,
                                std::chrono::milliseconds  step = std::chrono::seconds{1}) {
    std::vector<MetricEntry>              out;
    std::chrono::system_clock::time_point at{};
    for(auto const v : values) {
        out.push_back({.recvTime = at, .level = uc_log::LogLevel::info, .ucTime = {}, .value = v});
        at += step;
    }
    return out;
}

void summaryNumbers() {
    auto const s = summarize(series({4.0, 1.0, 3.0, 2.0}));
    CHECK(s.count == 4 && s.latest == 2.0 && s.min == 1.0 && s.max == 4.0 && s.mean == 2.5,
          "count, latest, min, max, mean");
    CHECK(s.rate == 1.0, "four values over three seconds: one a second");
    auto const none = summarize({});
    CHECK(none.count == 0 && none.rate == 0.0, "an empty series: zeros");
    CHECK(summarize(series({7.0})).rate == 0.0, "a lone value has no rate");
}

// the same numbers to the bit: a tally adds in the series' order, as summarize does
bool sameSummary(MetricSummary const& a,
                 MetricSummary const& b) {
    auto const bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    return a.count == b.count && bits(a.latest) == bits(b.latest) && bits(a.min) == bits(b.min)
        && bits(a.mean) == bits(b.mean) && bits(a.max) == bits(b.max)
        && bits(a.rate) == bits(b.rate);
}

void tallyMatchesTheSeries() {
    auto        values = series({4.0, -1.0, 3.5, 2.0, 9.0, 0.25});
    MetricTally t;
    for(std::size_t i = 0; i < values.size(); ++i) { t.add(values[i], i == 0); }
    CHECK(sameSummary(t.summary(values), summarize(values)),
          "taken in one by one: the whole series' summary, to the bit");
    values.erase(values.begin(),
                 values.begin() + 2);   // the oldest dropped, as the store's bound does
    t.recount(values);
    CHECK(sameSummary(t.summary(values), summarize(values)) && t.min == 0.25 && t.max == 9.0,
          "counted anew from the rest: the dropped least is gone");
}

void rateText() {
    CHECK_EQ(formatRate(9.95), std::string{"9.95/s"}, "below a thousand: three digits");
    CHECK_EQ(formatRate(1510.0), std::string{"1.51K/s"}, "thousands: K, no exponent");
    CHECK_EQ(formatRate(2.3e6), std::string{"2.30M/s"}, "millions: M");
}

void storeSummaries() {
    EntryStore store;
    for(int i = 0; i < 5; ++i) {
        LogEntry e{0, {}};
        e.logMsg   = "v @METRIC(adc::vin[mV]=" + std::to_string(i) + ")";
        e.parsedOk = true;
        store.addEntry(std::chrono::system_clock::time_point{} + std::chrono::seconds{i}, e);
    }
    auto const               sums = store.metricSummaries();
    std::vector<MetricEntry> values;
    if(!sums.empty()) {
        store.withMetric(sums[0].first, [&](std::vector<MetricEntry> const& v) { values = v; });
    }
    CHECK(sums.size() == 1 && sameSummary(sums[0].second, summarize(values))
            && sums[0].second.count == 5 && sums[0].second.max == 4.0,
          "the store's summary: the series' own, no value walked");
    CHECK(sums.size() == 1 && sums[0].second.unit && sums[0].second.unit->display == "mV"
            && sums[0].second.unit == store.metricSummaries()[0].second.unit,
          "its unit, read once for the series");
    if(!sums.empty()) { store.clearMetric(sums[0].first); }
    auto const cleared = store.metricSummaries();
    CHECK(cleared.size() == 1 && cleared[0].second.count == 0 && cleared[0].second.unit
            && cleared[0].second.unit->display == "mV",
          "a metric cleared: known, no value, its unit kept");
}

MetricSummaries someMetrics() {
    MetricSummaries out;
    for(auto const& info :
        std::vector<MetricInfo>{
          {"adc",  "vin", "V"},
          {"adc", "vref", "V"},
          {"cpu", "load", "%"},
          {"usb",   "rx", "B"}
    })
    {
        out.emplace_back(info, MetricSummary{1, 3.3, -1.0, 1.0, 3.3, 9.95});
    }
    return out;
}

void tableGroupsAndFolds() {
    auto const m   = someMetrics();
    auto const all = metricRows(m, "", {});
    CHECK(all.size() == 7 && all[0].group && all[0].scope == "adc" && all[0].count == 2
            && !all[1].group && all[1].metric == 0 && all[2].metric == 1 && all[3].group
            && all[3].scope == "cpu",
          "each scope's row, then its metrics");
    auto const folded = metricRows(m, "", {"adc"});
    CHECK(folded.size() == 5 && folded[0].folded && folded[0].count == 2
            && folded[1].scope == "cpu",
          "a folded scope: its one row, how many it holds");
    auto const volts = metricRows(m, "[V]", {});
    CHECK(volts.size() == 3 && volts[0].count == 2,
          "the words narrow it, a scope none matches goes");
    CHECK(rowOfMetric(folded, m, 1) == std::optional<std::size_t>{0}
            && rowOfMetric(folded, m, 2) == std::optional<std::size_t>{2},
          "a folded metric shows on its scope's row");
    CHECK(!rowOfMetric(volts, m, 2), "hidden by the words: no row");
    CHECK_EQ(metricGroupText(folded[0]), std::string{"▸ adc · 2"}, "a folded scope's row");
}

void markMoves() {
    auto const m    = someMetrics();
    auto const rows = metricRows(m, "", {});   // adc, vin, vref, cpu, load, usb, rx
    CHECK(metricAfter(rows, 1, 1) == std::optional<std::size_t>{1}
            && metricAfter(rows, 2, 1) == std::optional<std::size_t>{2},
          "down: the next metric, over a scope's row");
    CHECK(metricAfter(rows, 6, 5) == std::optional<std::size_t>{3}
            && metricAfter(rows, 1, -3) == std::optional<std::size_t>{0},
          "the ends held");
    auto const folded = metricRows(m, "", {"cpu"});   // adc, vin, vref, cpu (folded), usb, rx
    CHECK(metricAfter(folded, 3, 1) == std::optional<std::size_t>{3}
            && metricAfter(folded, 3, -1) == std::optional<std::size_t>{1},
          "from a folded scope's row: the first after it, the last before it");
    CHECK(!metricAfter(metricRows(m, "zzz", {}), 0, 1), "nothing shown: no move");
}

struct CodePoints {
    std::size_t operator()(std::string_view text) const {
        std::size_t n = 0;
        for(char const c : text) {
            if((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) { ++n; }
        }
        return n;
    }
};

void columnsLineUp() {
    auto const m = someMetrics();
    auto const c = metricColumns(m, 28, CodePoints{});
    CHECK(c.name == 4, "the name column: the longest name, the heading's at least");
    auto const heading = metricHeading(c, CodePoints{});
    auto const row     = metricRowText(m[0].first, m[0].second, "⣀⣀", c, CodePoints{});
    auto const endOf   = [](std::string const& text, std::string_view part) {
        return CodePoints{}(std::string_view{text}.substr(0, text.find(part) + part.size()));
    };
    CHECK(endOf(heading, "latest") == endOf(row, "3.3")
            && endOf(heading, "rate") == endOf(row, "9.95/s") && heading.contains("unit")
            && CodePoints{}(heading.substr(0, heading.find("unit")))
                 == CodePoints{}(row.substr(0, row.find("V "))),
          "each value ends under its heading, the unit starts under its own");
    CHECK(row.starts_with("  vin ") && row.ends_with("⣀⣀"), "the name indented, the trend last");
    MetricInfo const long_{"adc", "a_very_long_name", "V"};
    CHECK(
      metricRowText(long_, m[0].second, "", MetricColumns{6}, CodePoints{}).starts_with("  …_name"),
      "a name past its column: its end kept");
}

// an integer firmware's quantities: the numbers in the unit they are shown in, that unit in its column, a long one cut
void columnsShowUnits() {
    MetricSummaries m;
    m.emplace_back(MetricInfo{"heater", "tip", "c℃"},
                   MetricSummary{1, 31250, 2500, 31000.4, 32000, 10});
    m.emplace_back(MetricInfo{"heater", "owed", "mA s"}, MetricSummary{1, 160000, 0, 1, 160000, 1});
    m.emplace_back(MetricInfo{"heater", "kp", "(10⁻³ 1/℃)"},
                   MetricSummary{1, 2500, 2500, 2500, 2500, 1});
    m.emplace_back(MetricInfo{"x", "y", "kg m²/(s³ A)"}, MetricSummary{1, 1, 1, 1, 1, 1});
    auto const c = metricColumns(m, 28, CodePoints{});
    CHECK(c.unit == MetricColumns::maxUnit, "the unit column: the longest, at most maxUnit");
    auto const tip = metricRowText(m[0].first, m[0].second, "", c, CodePoints{});
    CHECK(tip.contains("312.50") && tip.contains("25.00") && tip.contains("320.00")
            && tip.contains("°C "),
          "c℃ as °C, two decimals");
    auto const owed = metricRowText(m[1].first, m[1].second, "", c, CodePoints{});
    CHECK(owed.contains("160.000") && owed.contains("A·s "), "mA s as A·s");
    auto const kp = metricRowText(m[2].first, m[2].second, "", c, CodePoints{});
    CHECK(kp.contains("2.500") && kp.contains("1/°C "), "a scaled unit");
    auto const cut = metricRowText(m[3].first, m[3].second, "", c, CodePoints{});
    CHECK(cut.contains("kg·m²/(s³…") && CodePoints{}(cut) == CodePoints{}(tip),
          "a unit past its column: cut, the row as wide as the others");
    CHECK(metricWords(m[0].first, m[0].second) == "heater::tip [°C c℃]"
            && !metricRows(m, "c℃", {}).empty() && !metricRows(m, "°C", {}).empty(),
          "the words hold the unit shown and the one sent");
}

void trendCostsTheSame() {
    std::vector<double> rising;
    rising.reserve(16);
    for(int i = 0; i < 16; ++i) { rising.push_back(static_cast<double>(i)); }
    auto const t = metricTrend(series(rising), 8);
    CHECK(CodePoints{}(t) == 8 && t.starts_with("⣀") && t.ends_with("⠉"),
          "eight cells: low at the oldest, high at the newest");
    auto const many = series(std::vector<double>(100'000, 1.0), std::chrono::milliseconds{1});
    CHECK(CodePoints{}(metricTrend(many, 8)) == 8,
          "a hundred thousand values: the same eight cells");
    CHECK(metricTrend({}, 8) == std::string(8, ' '), "no value: blank");
}

void chartDots() {
    // 2 x 1 cells = 4 x 4 dots; 0 at the bottom left, 1 at the top right, the diagonal between
    auto const c = metricChart(series({0.0, 1.0}), MetricRange::all, 2, 1);
    CHECK(c.rows.size() == 1 && c.top == 1.0 && c.bottom == 0.0 && c.drawn == 2 && c.seconds == 1.0,
          "one row, the axes' values and time");
    // cell 0: dots (0,3) and (1,2) = 0x40 | 0x20 -> U+2860; cell 1: (2,1) and (3,0) = 0x02 | 0x08 -> U+280A
    CHECK_EQ(c.rows.front(),
             std::string{"\xE2\xA1\xA0\xE2\xA0\x8A"},
             "the diagonal as braille dots");
    auto const empty = metricChart({}, MetricRange::all, 3, 2);
    CHECK(empty.rows.size() == 2 && empty.rows[0] == "\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80"
            && empty.drawn == 0,
          "no values: blank braille cells");
}

void flatLine() {
    auto const c = metricChart(series({5.0, 5.0, 5.0, 5.0}), MetricRange::all, 4, 1);
    CHECK(c.top == 5.5 && c.bottom == 4.5, "all the same: the axis 10 % around the value");
    // the third dot row of every cell, both columns: 0x04 | 0x20 -> U+2824
    std::string row;
    for(int i = 0; i < 4; ++i) { row += "\xE2\xA0\xA4"; }
    CHECK_EQ(c.rows.front(), row, "a line through the middle, joined");
    CHECK(metricChart(series({0.0, 0.0}), MetricRange::all, 1, 1).top == 1.0,
          "zero: one either side");
}

void rangeEndsAtTheNewest() {
    std::vector<double> values;
    for(int i = 0; i <= 100; ++i) { values.push_back(static_cast<double>(i)); }
    auto const all   = series(values);   // 0 .. 100 s
    auto const whole = metricChart(all, MetricRange::all, 20, 4);
    CHECK(whole.drawn == 101 && whole.seconds == 100.0 && whole.top == 100.0 && whole.bottom == 0.0,
          "all: every value, oldest to newest");
    auto const last = metricChart(all, MetricRange::last10s, 20, 4);
    CHECK(last.drawn == 11 && last.seconds == 10.0 && last.top == 100.0 && last.bottom == 90.0,
          "the last 10 s: back from the newest value, its own axis");
    auto const wide = metricChart(all, MetricRange::last10min, 20, 4);
    CHECK(wide.drawn == 101 && wide.seconds == 600.0,
          "a range longer than the series: all of it, the range's time");
    CHECK(rangeName(MetricRange::last60s) == "the last 60 s"
            && rangeLength(MetricRange::last10min).count() == 600 && metricRanges.size() == 4,
          "four ranges, named");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {       "summaryNumbers",        summaryNumbers},
                               {"tallyMatchesTheSeries", tallyMatchesTheSeries},
                               {             "rateText",              rateText},
                               {       "storeSummaries",        storeSummaries},
                               {  "tableGroupsAndFolds",   tableGroupsAndFolds},
                               {            "markMoves",             markMoves},
                               {        "columnsLineUp",         columnsLineUp},
                               {     "columnsShowUnits",      columnsShowUnits},
                               {    "trendCostsTheSame",     trendCostsTheSame},
                               {            "chartDots",             chartDots},
                               {             "flatLine",              flatLine},
                               { "rangeEndsAtTheNewest",  rangeEndsAtTheNewest},
    });
}
