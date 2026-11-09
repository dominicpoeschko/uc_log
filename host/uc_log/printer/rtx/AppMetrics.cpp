// The App: the metrics page - the marked metric's plot over a range across the page, under it a table of every metric
// grouped by scope (a scope's row folds it, a question narrows it to the metrics that hold its words:
// printer/MetricTable.hpp) whose rows scroll by themselves (the wheel, its scrollbar) and go to the mark when it
// moves (the keys); its keys and the wheel. Every value in the unit it is shown in (printer/MetricUnit.hpp: 31250 c℃
// as 312.50 °C), the plot's in its title.

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/ListCursor.hpp"
#include "uc_log/printer/MetricTable.hpp"
#include "uc_log/printer/PanelRows.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

namespace {
    std::string spanText(double seconds) {
        if(seconds < 120.0) { return fmt::format("{:.1f} s", seconds); }
        if(seconds < 7200.0) { return fmt::format("{:.1f} min", seconds / 60.0); }
        return fmt::format("{:.1f} h", seconds / 3600.0);
    }

    // where `info` is in `metrics`
    std::optional<std::size_t> indexOf(MetricSummaries const&           metrics,
                                       std::optional<MetricInfo> const& info) {
        if(!info) { return std::nullopt; }
        auto const it
          = std::ranges::find_if(metrics, [&](auto const& m) { return m.first == *info; });
        if(it == metrics.end()) { return std::nullopt; }
        return static_cast<std::size_t>(it - metrics.begin());
    }

}   // namespace

void App::moveMetric(long step) {
    // `step` metrics on in the table as it is now (a new one may have come before the marked one)
    if(metrics_ == nullptr) { return; }
    auto const metrics = metrics_->metricSummaries();
    auto const rows    = metricRows(metrics, pages_.metricFilter, pages_.foldedScopes);
    auto const marked  = indexOf(metrics, pages_.metric);
    auto const at      = marked ? rowOfMetric(rows, metrics, *marked) : std::nullopt;
    if(auto const next = metricAfter(rows, at.value_or(0), step)) {
        pages_.metric = metrics[*next].first;
    }
    pages_.metricFollow = true;
    relayout();
}

void App::scrollMetrics(long rows) {
    // the next fill holds the first row shown within the table
    auto const top      = static_cast<long>(pages_.metricTop) + rows;
    pages_.metricTop    = static_cast<std::size_t>(std::max(top, 0L));
    pages_.metricFollow = false;
    relayout();
}

bool App::metricsKey(::rtxui::Event::Keyboard const& k) {
    using Keyboard     = ::rtxui::Event::Keyboard;
    auto const special = k.special();
    if(special == Keyboard::ArrowUp || special == Keyboard::ArrowDown || special == Keyboard::PageUp
       || special == Keyboard::PageDown)
    {
        auto const rows = static_cast<long>(std::max<std::size_t>(pages_.metricRows, 1));
        moveMetric(special == Keyboard::ArrowUp     ? -1
                   : special == Keyboard::ArrowDown ? 1
                   : special == Keyboard::PageUp    ? -rows
                                                    : rows);
        return true;
    }
    if(special == Keyboard::ArrowLeft) {
        pages_.metricRange = (pages_.metricRange + metricRanges.size() - 1) % metricRanges.size();
    } else if(special == Keyboard::ArrowRight) {
        pages_.metricRange = (pages_.metricRange + 1) % metricRanges.size();
    } else {
        return false;
    }
    relayout();
    return true;
}

std::string App::metricsWhat() const {
    if(metrics_ == nullptr) { return {}; }
    auto const infos = metrics_->metricInfos();
    if(infos.empty()) { return {}; }
    std::set<std::string_view> scopes;
    for(auto const& info : infos) { scopes.insert(info.scope); }
    return fmt::format("{} in {} group{}",
                       infos.size(),
                       scopes.size(),
                       scopes.size() == 1 ? "" : "s");
}

PageForm<std::function<void()>> App::metricsForm() {
    PageForm<std::function<void()>> p;
    if(metrics_ == nullptr) {
        p.box("📈 Metrics", true).push_back({label("this window has no metric source")});
        return p;
    }
    auto const metrics = metrics_->metricSummaries();   // one lock, no value walked
    if(metrics.empty()) {
        p.box("📈 Metrics", true)
          .push_back({label(
            R"(no metric yet: uc_log::metric<"name"_sc, "unit"_sc, "scope"_sc>(value) in the firmware)")});
        return p;
    }
    auto const& filter = pages_.metricFilter;
    p.tool(filter.empty() ? std::string{"🔎 Narrow …"} : fmt::format("🔎 {} …", filter),
           [this] { ask(Field::metricFilter); });
    if(!filter.empty()) {
        p.tool(
          "✕ all",
          [this] {
              pages_.metricFilter.clear();
              pages_.metricTop    = 0;
              pages_.metricFollow = true;
          },
          ButtonLook::chip);
    }
    p.tool("🗑 Clear", [this] { run(Command::clearMetrics); }, ButtonLook::danger);

    auto const rows = metricRows(metrics, filter, pages_.foldedScopes);
    // the marked metric by what it is (a new one sorts in before it); gone or hidden by the words: the first shown
    auto marked = indexOf(metrics, pages_.metric);
    if(!marked || !rowOfMetric(rows, metrics, *marked)) {
        marked = metricAfter(rows, 0, 1).value_or(marked.value_or(0));
    }
    pages_.metric = metrics[*marked].first;

    // the plot on top across the page, the table under it, its rows scrolling by themselves. The rows: the page's
    // (its head off) less the plot's frame, ranges, time axis and numbers and the table's frame and heading; the
    // plot takes what the table leaves, the table at most half of it. The page's last column is its scrollbar
    constexpr std::size_t labelCells = 10;   // the plot's value column
    auto const            pageCells  = static_cast<std::size_t>(std::max(width_ - 1, 20));
    // the box's frame and padding, the value column and its axis, a gap
    auto const plotCells = pageCells > labelCells + 16 ? pageCells - labelCells - 8 : 8;
    auto const pageRows  = panelRows() > 1 ? panelRows() - 1 : 1;
    auto const room      = pageRows > 8 ? pageRows - 8 : 0;
    auto const plotRows
      = std::clamp<std::size_t>(room - std::min(std::max<std::size_t>(rows.size(), 1), room / 2),
                                4,
                                30);
    auto const tableRows = room > plotRows ? room - plotRows : 1;
    auto const shown     = std::min(rows.size(), tableRows);
    // the shown rows go to the mark when it moved, else they stay where the wheel or the scrollbar put them
    auto const markedRow = rowOfMetric(rows, metrics, *marked);
    ListCursor cursor{pages_.metricFollow ? markedRow : std::nullopt, pages_.metricTop};
    cursor.fit(rows.size(), std::max<std::size_t>(shown, 1));
    pages_.metricTop    = cursor.top;
    pages_.metricRows   = shown;
    pages_.metricFollow = false;

    // the marked metric's plot: the ranges, the value column, the axis, the braille cells; the time axis and its
    // numbers under them
    auto const& [info, sum] = metrics[*marked];
    auto const unit         = shownUnit(info, sum);
    auto const range        = metricRanges[pages_.metricRange % metricRanges.size()];
    auto&      plot = p.box(fmt::format("📉 {}::{}{}",
                                        info.scope,
                                        info.name,
                                        unit->display.empty() ? "" : " [" + unit->display + "]"),
                            true);
    PanelRow   ranges;
    for(std::size_t i = 0; i < metricRanges.size(); ++i) {
        ranges.push_back(p.option(rangeName(metricRanges[i]),
                                  i == pages_.metricRange % metricRanges.size(),
                                  [this, i] { pages_.metricRange = i; }));
    }
    plot.push_back(std::move(ranges));
    MetricChart chart;
    metrics_->withMetric(info, [&](std::vector<MetricEntry> const& v) {
        chart = metricChart(v, range, plotCells, plotRows);
    });
    for(std::size_t r = 0; r < chart.rows.size(); ++r) {
        bool const  first = r == 0;
        bool const  last  = r + 1 == chart.rows.size();
        std::string value;
        if(first) { value = formatMetricNumber(chart.top, *unit); }
        if(last) { value = formatMetricNumber(chart.bottom, *unit); }
        auto const cells = RtxuiWidth{}(value);
        value.insert(0, labelCells > cells ? labelCells - cells : 0, ' ');
        plot.push_back({label(std::move(value)),
                        label(first || last ? " ┤" : " │"),
                        toned(Tone::info, chart.rows[r])});
    }
    constexpr std::string_view newest = "newest";
    std::string axis = std::string(labelCells, ' ') + " └ -" + spanText(chart.seconds);
    auto const  wide = RtxuiWidth{}(axis);
    auto const  end  = labelCells + 2 + plotCells;
    if(end > wide + newest.size()) { axis += std::string(end - wide - newest.size(), ' '); }
    axis += newest;
    plot.push_back({label(std::move(axis))});
    // the unit it came in when that is another: what a firmware's source names
    plot.push_back({label(fmt::format(
      "{} values · min {} · mean {} · max {} · {}{}",
      sum.count,
      formatMetricValue(sum.min, *unit),
      formatMetricValue(sum.mean, *unit),
      formatMetricValue(sum.max, *unit),
      formatRate(sum.rate),
      unit->text == unit->display ? std::string{} : fmt::format(" · sent in {}", unit->text)))});

    // the table: its heading, then the rows shown - a scrollbar beside them when they are a part of all
    std::size_t matched = 0;
    for(auto const& r : rows) { matched += r.group ? r.count : 0; }
    auto legend = matched == metrics.size()
                  ? fmt::format("📈 {} metrics", metrics.size())
                  : fmt::format("📈 {} of {} metrics", matched, metrics.size());
    if(shown < rows.size()) {
        legend
          += fmt::format(" · rows {}-{} of {}", cursor.top + 1, cursor.top + shown, rows.size());
    }
    auto&      table   = p.box(std::move(legend), true);
    auto const columns = metricColumns(metrics, 28, RtxuiWidth{});
    if(rows.empty()) {
        table.push_back({label(fmt::format("no metric holds {:?}", filter))});
    } else {
        table.push_back({label(metricHeading(columns, RtxuiWidth{}))});
    }
    for(std::size_t r = cursor.top; r < rows.size() && r < cursor.top + shown; ++r) {
        auto const& row = rows[r];
        bool const  on  = markedRow == r;
        if(row.group) {
            // a scope's row: a click folds it to this row, or opens it again
            table.push_back({p.button(
              metricGroupText(row),
              [this, scope = row.scope] {
                  if(pages_.foldedScopes.erase(scope) == 0) { pages_.foldedScopes.insert(scope); }
              },
              ButtonLook::row,
              on,
              Tone::info)});
            continue;
        }
        auto const& [rowInfo, rowSum] = metrics[row.metric];
        std::string trend;
        metrics_->withMetric(rowInfo, [&](std::vector<MetricEntry> const& v) {
            trend = metricTrend(v, MetricColumns::trend);
        });
        table.push_back({p.button(
          metricRowText(rowInfo, rowSum, trend, columns, RtxuiWidth{}),
          [this, rowInfo] { pages_.metric = rowInfo; },
          ButtonLook::row,
          on)});
    }
    if(shown < rows.size()) { p.scrolling({1, cursor.top, rows.size()}); }   // under the heading
    return p;
}

}   // namespace uc_log::printer::rtx
