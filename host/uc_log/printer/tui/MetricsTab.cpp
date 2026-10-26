// the Metrics tab.
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getMetricOverviewComponent() -> ftxui::Component {
        auto clearButton
          = printer::tui::button("🗑️ Clear Metrics", printer::tui::Style::destructive, [this]() {
                store.clearMetrics();
                metricPlotWidget.setSelectedMetric(std::nullopt);
            });

        std::vector<ftxui::Component> components;
        components.push_back(clearButton);
        components.push_back(printer::tui::divider());

        components.push_back(ftxui::Renderer([this]() {
            std::size_t const count = store.metricCount();
            return ftxui::text(fmt::format("📈 Metrics ({} entries)", count)) | ftxui::bold
                 | ftxui::color(Theme::Header::primary()) | ftxui::center;
        }));
        components.push_back(printer::tui::divider());

        auto metricsContainer = ftxui::Container::Vertical({});

        auto dynamicMetricsList
          = metricsContainer
          | ftxui::Renderer([this, metricsContainer](ftxui::Element const&) mutable {
                auto currentSelected = metricPlotWidget.getSelectedMetric();

                bool const needsRebuild = (store.metricCount() != lastMetricCount)
                                       || (!hasLastSelectedInfo && currentSelected.has_value())
                                       || (hasLastSelectedInfo && !currentSelected.has_value())
                                       || (hasLastSelectedInfo && currentSelected.has_value()
                                           && *currentSelected != lastSelectedInfo);
                std::vector<MetricInfo> metricInfos;
                if(needsRebuild) {
                    metricInfos     = store.metricInfos();
                    lastMetricCount = metricInfos.size();
                }

                if(needsRebuild) {
                    metricsContainer->DetachAllChildren();

                    if(metricInfos.empty()) {
                        metricsContainer->Add(ftxui::Renderer([]() {
                            return ftxui::text("No metrics available")
                                 | ftxui::color(Theme::Status::inactive()) | ftxui::center;
                        }));
                    } else {
                        for(auto const& metricInfo : metricInfos) {
                            bool const isSelected
                              = metricPlotWidget.getSelectedMetric()
                             && metricPlotWidget.getSelectedMetric() == metricInfo;

                            auto selectButton = printer::tui::button(
                              isSelected ? "📈 Selected" : "📊 Select",
                              isSelected ? printer::tui::Style::positive
                                         : printer::tui::Style::build,
                              [this, metricInfo]() {
                                  metricPlotWidget.setSelectedMetric(metricInfo);
                              });

                            auto metricRow = ftxui::Container::Horizontal(
                              {ftxui::Renderer([this, metricInfo]() {
                                   double      latestValue{};
                                   std::size_t valueCount{};
                                   bool        found{};
                                   // kvasir_bench.py metrics' summary
                                   double minValue{};
                                   double maxValue{};
                                   double sum{};
                                   double rate{};
                                   found = store.withMetric(
                                     metricInfo,
                                     [&](std::vector<MetricEntry> const& v) {
                                         valueCount  = v.size();
                                         latestValue = v.empty() ? 0.0 : v.back().value;
                                         if(!v.empty()) {
                                             minValue = maxValue = v.front().value;
                                             for(auto const& e : v) {
                                                 minValue = std::min(minValue, e.value);
                                                 maxValue = std::max(maxValue, e.value);
                                                 sum += e.value;
                                             }
                                             auto const span
                                               = std::chrono::duration<double>(v.back().recvTime
                                                                               - v.front().recvTime)
                                                   .count();
                                             if(span > 0) {
                                                 rate = static_cast<double>(v.size() - 1) / span;
                                             }
                                         }
                                     });
                                   if(!found) {
                                       return ftxui::text("Metric not found")
                                            | ftxui::color(Theme::Status::error());
                                   }

                                   return ftxui::hbox(
                                            {ftxui::text("📊 ") | ftxui::color(Theme::Data::icon()),
                                             ftxui::text(metricInfo.scope)
                                               | ftxui::color(Theme::Data::scope()),
                                             ftxui::text("::")
                                               | ftxui::color(Theme::UI::separator()),
                                             ftxui::text(metricInfo.name)
                                               | ftxui::color(Theme::Data::name()) | ftxui::bold,
                                             ftxui::text(metricInfo.unit.empty()
                                                           ? ""
                                                           : fmt::format(" [{}]", metricInfo.unit))
                                               | ftxui::color(Theme::Data::unit()),
                                             ftxui::text(fmt::format(" = {:.3f}", latestValue))
                                               | ftxui::color(Theme::Status::info()) | ftxui::bold,
                                             ftxui::text(fmt::format(" ({} values)", valueCount))
                                               | ftxui::color(Theme::Data::count()),
                                             ftxui::text(
                                               valueCount == 0
                                                 ? std::string{}
                                                 : fmt::format("  min {:.4g} mean {:.4g} max {:.4g}"
                                                               "  {:.2g}/s",
                                                               minValue,
                                                               sum
                                                                 / static_cast<double>(valueCount),
                                                               maxValue,
                                                               rate))
                                               | ftxui::color(Theme::Text::metadata())})
                                        | ftxui::flex;
                               }) | ftxui::flex,
                               selectButton});

                            metricsContainer->Add(metricRow);
                        }
                    }

                    if(currentSelected.has_value()) {
                        hasLastSelectedInfo = true;
                        lastSelectedInfo    = *currentSelected;
                    } else {
                        hasLastSelectedInfo = false;
                    }
                }

                // a list of buttons scrolls by its focus: the wheel moves the container's
                // selection and the frame follows the selected row
                return metricsContainer->Render() | ftxui::vscroll_indicator | ftxui::yframe;
            });

        components.push_back(dynamicMetricsList | ftxui::flex);

        return ftxui::Container::Vertical(components);
    }

}}   // namespace uc_log::printer::tui
