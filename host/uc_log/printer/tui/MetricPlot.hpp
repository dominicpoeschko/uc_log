#pragma once

// The Metrics tab's plot of one metric series: the plot, its axes and the controls (zoom, time range).

#include "uc_log/printer/metric_utils.hpp"
#include "uc_log/printer/tui/FTXUI_Utils.hpp"
#include "uc_log/printer/tui/Widgets.hpp"
#include "uc_log/printer/tui/theme.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::tui {

/// A metric value with an SI prefix and two decimals ("1.50K", "-2.00mV"), 0 as "0", below 1e-6 in exponent form.
inline std::string formatMetricValue(double           value,
                                     std::string_view unit) {
    // from the largest: the first whose bound the magnitude reaches; divide and multiply as the plot always did
    // (value / 1e9 and value * 1e-9 can differ in the last bit)
    struct Prefix {
        double      atLeast;
        double      divideBy;
        double      multiplyBy;
        char const* symbol;
    };

    static constexpr std::array<Prefix, 6> prefixes{
      {{.atLeast = 1e9, .divideBy = 1e9, .multiplyBy = 1.0, .symbol = "G"},
       {.atLeast = 1e6, .divideBy = 1e6, .multiplyBy = 1.0, .symbol = "M"},
       {.atLeast = 1e3, .divideBy = 1e3, .multiplyBy = 1.0, .symbol = "K"},
       {.atLeast = 1.0, .divideBy = 1.0, .multiplyBy = 1.0, .symbol = ""},
       {.atLeast = 1e-3, .divideBy = 1.0, .multiplyBy = 1e3, .symbol = "m"},
       {.atLeast = 1e-6, .divideBy = 1.0, .multiplyBy = 1e6, .symbol = "µ"}}
    };
    auto const magnitude = std::abs(value);
    if(magnitude == 0.0) { return fmt::format("0{}", unit); }
    for(auto const& p : prefixes) {
        if(magnitude >= p.atLeast) {
            return fmt::format("{:.2f}{}{}", value / p.divideBy * p.multiplyBy, p.symbol, unit);
        }
    }
    return fmt::format("{:.2e}{}", value, unit);
}

enum class TimeUnit : std::uint8_t { seconds, minutes, hours };

enum class TimeRangeMode : std::uint8_t { showAll, lastPeriod };

class MetricPlotWidget {
public:
    struct Config {
        bool   autoFitY     = true;
        double yZoomLevel   = 1.0;
        double yCenterValue = 0.0;

        TimeRangeMode timeRangeMode   = TimeRangeMode::showAll;
        int           timePeriodValue = 10;
        TimeUnit      timePeriodUnit  = TimeUnit::seconds;
        bool          autoScroll      = true;

        int minHeight = 6;
    };

private:
    Config                    config_;
    std::optional<MetricInfo> selectedMetric_;

    int         timeModeIndex_      = 0;
    int         timeUnitIndex_      = 0;
    std::string timePeriodValueStr_ = "1";

    static std::chrono::seconds timeUnitToSeconds(int      value,
                                                  TimeUnit unit) {
        switch(unit) {
        case TimeUnit::seconds: return std::chrono::seconds(value);
        case TimeUnit::minutes: return std::chrono::seconds(value * 60);
        case TimeUnit::hours:   return std::chrono::seconds(value * 3600);
        }
        return std::chrono::seconds(value);
    }

    static std::string timeUnitToString(TimeUnit unit,
                                        bool     plural = true) {
        switch(unit) {
        case TimeUnit::seconds: return plural ? "seconds" : "second";
        case TimeUnit::minutes: return plural ? "minutes" : "minute";
        case TimeUnit::hours:   return plural ? "hours" : "hour";
        }
        return "seconds";
    }

    static TimeUnit suggestTimeUnit(std::chrono::seconds totalSpan) {
        auto secs = totalSpan.count();
        if(secs <= 300) { return TimeUnit::seconds; }
        if(secs <= 7200) { return TimeUnit::minutes; }
        return TimeUnit::hours;
    }

    [[nodiscard]] std::chrono::seconds
    analyzeDataTimeSpan(std::vector<MetricEntry> const& values) const {
        if(values.empty()) { return std::chrono::seconds(0); }
        auto oldest = values.front().recvTime;
        auto newest = values.back().recvTime;
        return std::chrono::duration_cast<std::chrono::seconds>(newest - oldest);
    }

    [[nodiscard]] std::string
    formatTimeLabel(std::chrono::system_clock::time_point currentTime,
                    std::chrono::system_clock::time_point referenceTime) const {
        auto duration
          = std::chrono::duration_cast<std::chrono::milliseconds>(referenceTime - currentTime);

        if(duration.count() >= 0) { return "now"; }

        auto absDuration
          = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - referenceTime);
        auto absSeconds = std::chrono::duration_cast<std::chrono::seconds>(absDuration).count();

        if(absSeconds < 60) { return fmt::format("-{}s", absSeconds); }
        if(absSeconds < 3600) {
            auto minutes = absSeconds / 60;
            return fmt::format("-{}m", minutes);
        }
        if(absSeconds < 86400) {
            auto hours   = absSeconds / 3600;
            auto minutes = (absSeconds % 3600) / 60;
            return fmt::format("-{}h{}m", hours, minutes);
        }
        auto days  = absSeconds / 86400;
        auto hours = (absSeconds % 86400) / 3600;
        return fmt::format("-{}d{}h", days, hours);
    }

    [[nodiscard]] std::vector<std::string> generateYAxisLabels(double             minVal,
                                                               double             maxVal,
                                                               int                height,
                                                               std::string const& unit) const {
        std::vector<std::string> labels;

        if(height < 3) { return labels; }

        double const range = maxVal - minVal;
        if(range == 0.0) {
            labels.push_back(formatMetricValue(minVal, unit));
            return labels;
        }

        int numTicks = std::min(5, height / 2);
        numTicks     = std::max(numTicks, 2);

        for(int i = 0; i < numTicks; ++i) {
            double const value = maxVal - (static_cast<double>(i) * range / (numTicks - 1));
            labels.push_back(formatMetricValue(value, unit));
        }

        return labels;
    }

    [[nodiscard]] std::vector<std::string>
    generateXAxisLabels(std::vector<MetricEntry> const& values,
                        std::size_t                     startIdx,
                        std::size_t                     visibleDataSize) const {
        std::vector<std::string> labels;

        if(values.empty() || visibleDataSize < 2 || startIdx >= values.size()) { return labels; }

        auto currentTime = std::chrono::system_clock::now();

        int numTicks = std::min(4, static_cast<int>(visibleDataSize) / 8);
        numTicks     = std::max(numTicks, 2);

        for(int i = 0; i < numTicks; ++i) {
            std::size_t dataIdx = 0;
            if(i == numTicks - 1) {
                dataIdx = startIdx + visibleDataSize - 1;
            } else {
                dataIdx = startIdx
                        + (static_cast<std::size_t>(i) * (visibleDataSize - 1))
                            / static_cast<std::size_t>(numTicks - 1);
            }

            if(dataIdx < values.size()) {
                auto timeLabel = formatTimeLabel(currentTime, values[dataIdx].recvTime);
                labels.push_back(timeLabel);
            }
        }

        return labels;
    }

    [[nodiscard]] std::pair<double,
                            double>
    calculateYAxisRange(double dataMin,
                        double dataMax) const {
        if(config_.autoFitY) {
            if(std::abs(dataMin - dataMax) < std::numeric_limits<double>::epsilon()
                                               * std::max(std::abs(dataMin), std::abs(dataMax)))
            {
                double const center = dataMin;
                double const span   = std::max(1.0, std::abs(center) * 0.1);
                return {center - span, center + span};
            }

            double const dataRange = dataMax - dataMin;
            double const padding   = dataRange * 0.05;
            return {dataMin - padding, dataMax + padding};
        }
        double baseRange{};
        double center{};

        if(std::abs(dataMin - dataMax) < std::numeric_limits<double>::epsilon()
                                           * std::max(std::abs(dataMin), std::abs(dataMax)))
        {
            baseRange = std::max(1.0, std::abs(dataMin) * 0.2);
            center    = config_.yCenterValue != 0.0 ? config_.yCenterValue : dataMin;
        } else {
            baseRange = (dataMax - dataMin) / config_.yZoomLevel;
            center = config_.yCenterValue != 0.0 ? config_.yCenterValue : (dataMin + dataMax) / 2.0;
        }

        double const halfRange = baseRange / 2.0;
        return {center - halfRange, center + halfRange};
    }

    [[nodiscard]] std::pair<std::size_t,
                            std::size_t>
    calculateXAxisDataRange(std::vector<MetricEntry> const& values) const {
        if(values.empty()) { return {0, 0}; }

        std::size_t const dataSize = values.size();
        std::size_t       startIdx = 0;
        std::size_t       endIdx{};

        switch(config_.timeRangeMode) {
        case TimeRangeMode::showAll:
            startIdx = 0;
            endIdx   = dataSize;
            break;

        case TimeRangeMode::lastPeriod:
            {
                auto timeWindow
                  = timeUnitToSeconds(config_.timePeriodValue, config_.timePeriodUnit);
                auto cutoffTime = std::chrono::system_clock::now() - timeWindow;

                for(std::size_t i = 0; i < dataSize; ++i) {
                    if(values[i].recvTime >= cutoffTime) {
                        startIdx = i;
                        break;
                    }
                }
                endIdx = dataSize;
                break;
            }
        }

        return {startIdx, endIdx};
    }

public:
    MetricPlotWidget()
      : timeModeIndex_(static_cast<int>(config_.timeRangeMode))
      , timeUnitIndex_(static_cast<int>(config_.timePeriodUnit))
      , timePeriodValueStr_(std::to_string(config_.timePeriodValue)) {}

    void setSelectedMetric(std::optional<MetricInfo> metric) {
        selectedMetric_ = std::move(metric);
    }

    void syncUIState() {
        timeModeIndex_      = static_cast<int>(config_.timeRangeMode);
        timeUnitIndex_      = static_cast<int>(config_.timePeriodUnit);
        timePeriodValueStr_ = std::to_string(config_.timePeriodValue);
    }

    [[nodiscard]] std::optional<MetricInfo> const& getSelectedMetric() const UC_LOG_LIFETIMEBOUND {
        return selectedMetric_;
    }

    [[nodiscard]] ftxui::Component createControlsComponent() {
        syncUIState();
        auto yAutoFitCheckbox = ftxui::Checkbox("🔧 Auto-fit", &config_.autoFitY);

        auto yZoomOutBtn = printer::tui::button("🔍➖", printer::tui::Style::settings, [this]() {
            config_.yZoomLevel = std::max(0.1, config_.yZoomLevel / 1.5);
        });

        auto yZoomInBtn = printer::tui::button("🔍➕", printer::tui::Style::settings, [this]() {
            config_.yZoomLevel = std::min(10.0, config_.yZoomLevel * 1.5);
        });

        std::vector<std::string> const timeRangeModeOptions = {"📈 Show All", "⏰ Last Period"};
        auto timeModeToggle = ftxui::Toggle(timeRangeModeOptions, &timeModeIndex_);
        timeModeToggle      = ftxui::CatchEvent(timeModeToggle, [this](ftxui::Event const&) {
            config_.timeRangeMode = static_cast<TimeRangeMode>(timeModeIndex_);
            return false;
        });

        // digits only; a number outside 1..999 keeps the period it had
        auto timePeriodInput = printer::tui::numberInput<int>(timePeriodValueStr_,
                                                              "Enter number...",
                                                              [this](int value) {
                                                                  if(value > 0 && value <= 999) {
                                                                      config_.timePeriodValue
                                                                        = value;
                                                                  }
                                                              });

        std::vector<std::string> const timeUnitOptions = {"sec", "min", "hr"};
        auto timeUnitToggle = ftxui::Toggle(timeUnitOptions, &timeUnitIndex_);
        timeUnitToggle      = ftxui::CatchEvent(timeUnitToggle, [this](ftxui::Event const&) {
            config_.timePeriodUnit = static_cast<TimeUnit>(timeUnitIndex_);
            return false;
        });

        auto yAxisRow = ftxui::Container::Horizontal({
          ftxui::Renderer(
            []() { return ftxui::text("Y-Axis:") | ftxui::color(Theme::Header::primary()); }),
          yAutoFitCheckbox,
          ftxui::Maybe(yZoomOutBtn, [this]() { return !config_.autoFitY; }),
          ftxui::Maybe(yZoomInBtn, [this]() { return !config_.autoFitY; }),
        });

        auto xAxisRow = ftxui::Container::Horizontal({
          ftxui::Renderer(
            []() { return ftxui::text("X-Axis:") | ftxui::color(Theme::Header::primary()); }),
          timeModeToggle,
          ftxui::Maybe(ftxui::Container::Horizontal({
                         printer::tui::divider(),
                         ftxui::Renderer([]() {
                             return ftxui::text("Show last:") | ftxui::color(Theme::Text::normal());
                         }),
                         timePeriodInput | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6),
                         timeUnitToggle,
                       }),
                       [this]() { return config_.timeRangeMode == TimeRangeMode::lastPeriod; }),
        });

        ftxui::Components const allControls = {
          yAxisRow,
          xAxisRow,
        };

        return ftxui::Container::Vertical(allControls);
    }

    template<typename MetricDataProvider>
    ftxui::Component createGraphComponent(MetricDataProvider&& dataProvider) {
        return ftxui::Renderer([this,
                                capturedDataProvider = std::forward<MetricDataProvider>(
                                  dataProvider)]() mutable -> ftxui::Element {
            if(!selectedMetric_) {
                ftxui::Elements const noSelectionElements
                  = {ftxui::text("No metric selected") | ftxui::color(Theme::Status::warning())
                       | ftxui::center,
                     ftxui::text("Select a metric from the overview tab")
                       | ftxui::color(Theme::Text::metadata()) | ftxui::center};
                return ftxui::vbox(noSelectionElements);
            }

            auto metricData = capturedDataProvider(*selectedMetric_);
            if(!metricData || (*metricData)->empty()) {
                ftxui::Elements const noDataElements
                  = {ftxui::text("No data available") | ftxui::color(Theme::Status::error())
                       | ftxui::center,
                     ftxui::text("Waiting for metric data...")
                       | ftxui::color(Theme::Text::metadata()) | ftxui::center};
                return ftxui::vbox(noDataElements);
            }

            auto const& values = **metricData;
            auto const& info   = *selectedMetric_;

            auto [xStartIdx, xEndIdx]         = calculateXAxisDataRange(values);
            std::size_t const visibleDataSize = xEndIdx - xStartIdx;

            if(visibleDataSize == 0) {
                ftxui::Elements const noRangeElements
                  = {ftxui::text("No data in time range") | ftxui::color(Theme::Status::warning())
                       | ftxui::center,
                     ftxui::text("Adjust X-axis controls") | ftxui::color(Theme::Text::metadata())
                       | ftxui::center};
                return ftxui::vbox(noRangeElements);
            }

            double dataMinVal = values[xStartIdx].value;
            double dataMaxVal = values[xStartIdx].value;
            for(std::size_t i = xStartIdx; i < xEndIdx; ++i) {
                dataMinVal = std::min(dataMinVal, values[i].value);
                dataMaxVal = std::max(dataMaxVal, values[i].value);
            }

            auto [yMin, yMax] = calculateYAxisRange(dataMinVal, dataMaxVal);

            auto graphFunc = [values, xStartIdx, xEndIdx, visibleDataSize, yMin, yMax](
                               int width,
                               int height) -> std::vector<int> {
                std::vector<int> output(static_cast<std::size_t>(width), height / 2);

                if(visibleDataSize == 0 || width <= 0) { return output; }

                double yRange = yMax - yMin;
                if(yRange == 0.0) { yRange = 1.0; }

                for(int xPos = 0; xPos < width; ++xPos) {
                    std::size_t dataIdx{};
                    if(visibleDataSize == 1) {
                        dataIdx = xStartIdx;
                    } else {
                        dataIdx = xStartIdx
                                + (static_cast<std::size_t>(xPos) * (visibleDataSize - 1))
                                    / static_cast<std::size_t>(width - 1);
                    }

                    if(dataIdx < xEndIdx) {
                        double normalizedValue = (values[dataIdx].value - yMin) / yRange;
                        normalizedValue        = std::clamp(normalizedValue, 0.0, 1.0);
                        int const yPos         = static_cast<int>(normalizedValue * (height - 1));
                        output[static_cast<std::size_t>(xPos)] = std::clamp(yPos, 0, height - 1);
                    }
                }

                return output;
            };

            auto graph = ftxui::graph(graphFunc) | ftxui::color(Theme::Header::accent());

            if(values.size() < 2) { return graph; }

            auto yLabels = generateYAxisLabels(yMin, yMax, config_.minHeight, info.unit);
            auto xLabels = generateXAxisLabels(values, xStartIdx, visibleDataSize);

            ftxui::Elements yLabelElements;
            for(std::size_t i = 0; i < yLabels.size(); ++i) {
                yLabelElements.push_back(ftxui::text(yLabels[i])
                                         | ftxui::color(Theme::Text::metadata()));
                if(i < yLabels.size() - 1) { yLabelElements.push_back(ftxui::filler()); }
            }

            ftxui::Elements xLabelElements;
            for(auto const& label : xLabels) {
                xLabelElements.push_back(ftxui::text(label)
                                         | ftxui::color(Theme::Text::metadata()));
            }

            auto yAxisColumn = ftxui::vbox(yLabelElements)
                             | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8) | ftxui::align_right;

            ftxui::Elements spacedXLabels;
            spacedXLabels.push_back(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8));
            for(std::size_t i = 0; i < xLabelElements.size(); ++i) {
                spacedXLabels.push_back(xLabelElements[i]);
                if(i < xLabelElements.size() - 1) { spacedXLabels.push_back(ftxui::filler()); }
            }
            auto xAxisRow = ftxui::hbox(spacedXLabels);

            ftxui::Elements const graphRowElements = {yAxisColumn, graph | ftxui::flex};
            ftxui::Elements const graphElements
              = {ftxui::hbox(graphRowElements) | ftxui::flex,
                 xAxisRow | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1)};
            return ftxui::vbox(graphElements);
        });
    }

    template<typename MetricDataProvider>
    ftxui::Component createStatsComponent(MetricDataProvider&& dataProvider) {
        return ftxui::Renderer([this,
                                capturedDataProvider = std::forward<MetricDataProvider>(
                                  dataProvider)]() mutable -> ftxui::Element {
            if(!selectedMetric_) { return ftxui::text(""); }

            auto metricData = capturedDataProvider(*selectedMetric_);
            if(!metricData || (*metricData)->empty()) { return ftxui::text(""); }

            auto const& values = **metricData;
            auto const& info   = *selectedMetric_;

            auto [statsStartIdx, statsEndIdx] = calculateXAxisDataRange(values);
            std::size_t visibleStatsSize      = statsEndIdx - statsStartIdx;

            if(visibleStatsSize == 0) { return ftxui::text(""); }

            double       dataMinVal = values[statsStartIdx].value;
            double       dataMaxVal = values[statsStartIdx].value;
            double const latestVal  = values[statsEndIdx - 1].value;

            for(std::size_t i = statsStartIdx; i < statsEndIdx; ++i) {
                dataMinVal = std::min(dataMinVal, values[i].value);
                dataMaxVal = std::max(dataMaxVal, values[i].value);
            }

            auto [yMin, yMax] = calculateYAxisRange(dataMinVal, dataMaxVal);

            ftxui::Elements const titleElements = {
              ftxui::text("📊 ") | ftxui::color(Theme::Data::icon()),
              ftxui::text(info.scope) | ftxui::color(Theme::Data::scope()),
              ftxui::text("::") | ftxui::color(Theme::Text::separator()),
              ftxui::text(info.name) | ftxui::color(Theme::Data::name()) | ftxui::bold,
              ftxui::text(info.unit.empty() ? "" : fmt::format(" [{}]", info.unit))
                | ftxui::color(Theme::Data::unit()),
            };

            ftxui::Elements const statsElements = {
              ftxui::text("Current: ") | ftxui::bold,
              ftxui::text(formatMetricValue(latestVal, "")) | ftxui::color(Theme::Data::value()),
              ftxui::text(" | Range: ") | ftxui::bold,
              ftxui::text(formatMetricValue(dataMinVal, ""))
                | ftxui::color(Theme::Status::success()),
              ftxui::text(" to ") | ftxui::color(Theme::Status::success()),
              ftxui::text(formatMetricValue(dataMaxVal, ""))
                | ftxui::color(Theme::Status::success()),
              ftxui::text(" | Y-Axis: ") | ftxui::bold,
              ftxui::text(formatMetricValue(yMin, "")) | ftxui::color(Theme::Data::value()),
              ftxui::text(" to ") | ftxui::color(Theme::Data::value()),
              ftxui::text(formatMetricValue(yMax, "")) | ftxui::color(Theme::Data::value()),
              ftxui::text(" | Visible: ") | ftxui::bold,
              ftxui::text(fmt::format("{}/{}", visibleStatsSize, values.size()))
                | ftxui::color(Theme::Data::count()),
            };

            ftxui::Elements const allStatsElements
              = {ftxui::separator(), ftxui::hbox(titleElements), ftxui::hbox(statsElements)};
            return ftxui::vbox(allStatsElements);
        });
    }

    template<typename MetricDataProvider,
             typename ClearCallback>
    ftxui::Component createComponent(MetricDataProvider& dataProvider,
                                     ClearCallback&&     clearCallback) {
        auto clearButton = printer::tui::button("🗑️ Clear Data",
                                                printer::tui::Style::danger,
                                                std::forward<ClearCallback>(clearCallback));

        auto headerRenderer = ftxui::Renderer([]() {
            ftxui::Elements const headerElements
              = {ftxui::text("📈 Live Metric Plot") | ftxui::bold
                   | ftxui::color(Theme::Header::primary()) | ftxui::center,
                 ftxui::separator()};
            return ftxui::vbox(headerElements);
        });

        auto controls = createControlsComponent();
        auto graph    = createGraphComponent(dataProvider);
        auto stats    = createStatsComponent(dataProvider);

        ftxui::Components const mainContentComponents
          = {headerRenderer, graph | ftxui::flex, stats};
        auto mainContent = ftxui::Container::Vertical(mainContentComponents);

        ftxui::Components const allComponents = {clearButton,
                                                 printer::tui::divider(),
                                                 controls,
                                                 printer::tui::divider(),
                                                 mainContent | ftxui::flex};

        return ftxui::Container::Vertical(allComponents);
    }
};

}   // namespace uc_log::printer::tui
