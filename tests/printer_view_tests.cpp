// What the window shows, computed without it: the noisy-location cutoffs (printer/Outliers.hpp), the plain text of an
// exported or yanked line (printer/RowText.hpp), a saved filter read back (printer/FilterFile.hpp), a filter's toggles
// (printer/ToggleSet.hpp), a metric's value in its unit (printer/MetricUnit.hpp).
#include "support/Check.hpp"
#include "uc_log/printer/FilterFile.hpp"
#include "uc_log/printer/MetricUnit.hpp"
#include "uc_log/printer/Outliers.hpp"
#include "uc_log/printer/RowText.hpp"
#include "uc_log/printer/ToggleSet.hpp"

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

// the plot's axis and stat labels: in the unit the metric is shown in, one unit for every value of a series (its
// rules: MetricUnit.hpp and metric_unit_tests)
void metricValues() {
    using uc_log::printer::formatMetricNumber;
    using uc_log::printer::formatMetricValue;
    using uc_log::printer::readMetricUnit;
    auto const centi = readMetricUnit("c℃");
    CHECK(formatMetricValue(31250.0, centi) == "312.50 °C"
            && formatMetricNumber(31250.0, centi) == "312.50",
          "a label with its unit, an axis number without");
    CHECK(formatMetricValue(2960.0, readMetricUnit("mV")) == "2960 mV"
            && formatMetricValue(0.5, readMetricUnit("mV")) == "0.5 mV",
          "no value moves the unit: every label of a series in one");
    CHECK(formatMetricValue(0.0, readMetricUnit("")) == "0"
            && formatMetricValue(7.0, readMetricUnit("min")) == "7 min",
          "no unit, a unit that only starts like a prefix");
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
                               {   "toggleSet",    toggleSet},
                               {"metricValues", metricValues},
    });
}
