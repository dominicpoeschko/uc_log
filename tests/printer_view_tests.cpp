// What the TUI shows, computed without it: the noisy-location cutoffs (printer/Outliers.hpp), the plain text of an
// exported or yanked line (printer/RowText.hpp), a saved filter read back (printer/FilterFile.hpp).
#include "support/Check.hpp"
#include "uc_log/printer/FilterFile.hpp"
#include "uc_log/printer/Outliers.hpp"
#include "uc_log/printer/RowText.hpp"
#include "uc_log/printer/ToggleSet.hpp"
#include "uc_log/printer/tui/FTXUI_Utils.hpp"
#include "uc_log/printer/tui/MetricPlot.hpp"
#include "uc_log/printer/tui/Widgets.hpp"

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using uc_log::printer::OutlierMethod;

std::vector<std::pair<int,
                      std::size_t>>
counts(std::vector<std::size_t> const& n) {
    std::vector<std::pair<int, std::size_t>> out;
    for(std::size_t i = 0; i != n.size(); ++i) { out.emplace_back(static_cast<int>(i), n[i]); }
    return out;
}

void outliers() {
    using uc_log::printer::computeOutliers;
    CHECK(!computeOutliers(counts({1, 2}), OutlierMethod::iqrTukey, 1.5, 10, 100).valid,
          "fewer than 3: none");
    auto const iqr
      = computeOutliers(counts({1, 2, 3, 4, 1000}), OutlierMethod::iqrTukey, 1.5, 10, 100);
    // sorted 1 2 3 4 1000: median 3, q1 = median(1, 2) = 1, q3 = median(4, 1000) = 502, cutoff 502 + 1.5 * 501
    CHECK(iqr.valid && iqr.median == 3 && iqr.q1 == 1 && iqr.q3 == 502 && iqr.cutoff == 1253
            && iqr.wouldExclude.empty(),
          "IQR: quartiles and the cutoff");
    auto const top
      = computeOutliers(counts({1, 2, 3, 4, 1000}), OutlierMethod::topNPercent, 1.5, 20, 100);
    CHECK(top.cutoff == 1000 && top.wouldExclude.empty(), "top 20 %: keep 4, cutoff the 5th count");
    auto const abs
      = computeOutliers(counts({1, 2, 3, 4, 1000}), OutlierMethod::absoluteThreshold, 1.5, 20, 3);
    CHECK(abs.cutoff == 3 && (abs.wouldExclude == std::vector<int>{3, 4}),
          "above an absolute threshold");
}

void rowText() {
    uc_log::printer::RowFields const f{.recvTime     = "10:00:00.123",
                                       .channel      = 1,
                                       .ucTime       = "0.500000",
                                       .level        = "warn",
                                       .text         = "hello",
                                       .modulePrefix = "[i2c] ",
                                       .file         = "main.cpp",
                                       .line         = 42,
                                       .function     = "main"};
    CHECK(uc_log::printer::plainRow(f, true)
            == "10:00:00.123 1 0.500000 warn | hello ([i2c] main.cpp:42 main)",
          "the export: the level padded to 5");
    CHECK(uc_log::printer::plainRow(f, false)
            == "10:00:00.123 1 0.500000 warn| hello ([i2c] main.cpp:42 main)",
          "the yank: not padded");
}

void filterFile() {
    auto const                   path = std::filesystem::temp_directory_path()
                                      / ("uc_log_filter_" + std::to_string(::getpid()) + ".json");
    uc_log::printer::FilterState f;
    f.enabledLogLevels = {uc_log::LogLevel::warn, uc_log::LogLevel::error};
    f.enabledChannels  = {0, 2};
    f.excludedModules  = {"i2c.bus"};
    CHECK(uc_log::printer::saveFilter(path.string(), f).has_value(), "saved");
    auto const back = uc_log::printer::loadFilter(path.string());
    CHECK(back && *back == f, "read back the same");
    std::filesystem::remove(path);
    auto const gone = uc_log::printer::loadFilter(path.string());
    CHECK(!gone && gone.error().starts_with("Error: cannot open"), "a missing file: said");
    auto const nowhere = uc_log::printer::saveFilter((path / "no_dir" / "f.json").string(), f);
    CHECK(!nowhere && nowhere.error().starts_with("Error: cannot open"),
          "an unwritable path: said");
}

void counts64() {
    using uc_log::printer::tui::formatCount;
    CHECK(formatCount(999) == "999" && formatCount(1000) == "1K" && formatCount(999'999) == "999K"
            && formatCount(1'000'000) == "1M",
          "the unit steps");
    CHECK(formatCount(4'294'967'296ULL) == "4294M" && formatCount(10'000'000'000ULL) == "10000M",
          "past 4 G: no wrap (a uint32 showed 0 and 1410M)");
}

void toggleSet() {
    using uc_log::printer::shownIn;
    using uc_log::printer::toggleIn;
    std::array const all{1, 2, 3};
    std::set<int>    set;
    CHECK(shownIn(set, 1) && shownIn(set, 3), "empty: all shown");
    toggleIn(set, all, 2);
    CHECK((set == std::set<int>{1, 3}) && !shownIn(set, 2), "one off from all: the others stay");
    toggleIn(set, all, 1);
    CHECK((set == std::set<int>{3}), "another off");
    toggleIn(set, all, 1);
    toggleIn(set, all, 2);
    CHECK(set.empty(), "all on again: collapses to empty");
}

void numbers() {
    using uc_log::printer::tui::leadingNumber;
    CHECK(leadingNumber<double>("1.5") == 1.5 && leadingNumber<double>("2.") == 2.0, "decimals");
    CHECK(leadingNumber<double>("1.2.3") == 1.2 && leadingNumber<int>("15x") == 15,
          "a leading number, as stod / stoi");
    CHECK(!leadingNumber<int>("") && !leadingNumber<double>(".")
            && !leadingNumber<std::size_t>("x1"),
          "none");
    CHECK(
      uc_log::printer::tui::locationText(std::pair<std::string, std::size_t>{"main.cpp", 42})
          == "main.cpp:42"
        && uc_log::printer::tui::locationText(std::pair<std::string, std::size_t>{"main.cpp", 0})
             == "main.cpp:*",
      "a location, a whole file as :*");
}

// the plot's axis and stat labels: an SI prefix and two decimals, from the largest prefix the value reaches
void metricValues() {
    using uc_log::printer::tui::formatMetricValue;
    CHECK(formatMetricValue(0.0, "") == "0" && formatMetricValue(0.0, "V") == "0V", "zero");
    CHECK(formatMetricValue(1.5e9, "") == "1.50G" && formatMetricValue(-2.5e6, "B") == "-2.50MB"
            && formatMetricValue(1234.0, "") == "1.23K",
          "G, M, K");
    CHECK(formatMetricValue(1.0, "s") == "1.00s" && formatMetricValue(999.999, "") == "1000.00",
          "no prefix from 1 up (rounded, not moved to K)");
    CHECK(formatMetricValue(0.5, "") == "500.00m" && formatMetricValue(5e-4, "V") == "500.00µV",
          "m, µ");
    CHECK(formatMetricValue(2e-7, "") == "2.00e-07"
            && formatMetricValue(-3e-9, "A") == "-3.00e-09A",
          "below 1e-6: exponent");
}

// a number field (the metric plot's period, Logs' jump-to, Trace's rows) takes digits only: a letter is refused and the
// value stays what it was
void numberField() {
    std::string text{"1"};
    int         got{0};
    int         calls{0};
    auto        field = uc_log::printer::tui::numberInput<int>(text, "", [&](int v) {
        got = v;
        ++calls;
    });
    (void)field->OnEvent(ftxui::Event::Character('x'));
    CHECK(text == "1" && calls == 0, "a letter does not reach the field");
    (void)field->OnEvent(ftxui::Event::Character('5'));
    CHECK(text.size() == 2 && text.contains('5') && calls == 1
            && got == uc_log::printer::tui::leadingNumber<int>(text),
          "a digit does, and the value follows the text");
}

// one key map for the log, scrollView and pickList
void scrollKeys() {
    using uc_log::printer::tui::ScrollKey;
    using uc_log::printer::tui::scrollKeyOf;
    CHECK(scrollKeyOf(ftxui::Event::ArrowUp) == ScrollKey::up
            && scrollKeyOf(ftxui::Event::Character('k')) == ScrollKey::up
            && scrollKeyOf(ftxui::Event::ArrowDown) == ScrollKey::down
            && scrollKeyOf(ftxui::Event::Character('j')) == ScrollKey::down,
          "a row: arrows and k/j");
    CHECK(scrollKeyOf(ftxui::Event::PageUp) == ScrollKey::pageUp
            && scrollKeyOf(ftxui::Event::PageDown) == ScrollKey::pageDown
            && scrollKeyOf(ftxui::Event::Home) == ScrollKey::home
            && scrollKeyOf(ftxui::Event::End) == ScrollKey::end,
          "a page, the ends");
    CHECK(scrollKeyOf(ftxui::Event::Character('y')) == ScrollKey::none
            && scrollKeyOf(ftxui::Event::ArrowLeft) == ScrollKey::none
            && scrollKeyOf(ftxui::Event::Return) == ScrollKey::none,
          "other keys are none of them");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {    "outliers",     outliers},
                               {     "rowText",      rowText},
                               {  "filterFile",   filterFile},
                               {    "counts64",     counts64},
                               {   "toggleSet",    toggleSet},
                               {     "numbers",      numbers},
                               {"metricValues", metricValues},
                               { "numberField",  numberField},
                               {  "scrollKeys",   scrollKeys},
    });
}
