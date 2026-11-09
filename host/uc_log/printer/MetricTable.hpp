#pragma once

// The metrics as a table, for any front end: grouped by scope (a scope's row, then its metrics unless it is folded),
// only those a query matches, each row's columns lined up - the latest value, the least, the greatest in the unit
// they are shown in (MetricUnit.hpp, one column of its own), the rate and a trend of the last minute - the moves of
// the mark through the rows shown, and the choices of the question that narrows the table. Many metrics (a hundred)
// stay readable: a scope folds to one row, a word narrows them.

#include "uc_log/printer/MetricView.hpp"
#include "uc_log/printer/TextMatch.hpp"
#include "uc_log/printer/metric_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// Every metric with its summary, ordered by scope, name and unit (EntryStore::metricSummaries).
using MetricSummaries = std::vector<std::pair<MetricInfo, MetricSummary>>;

/// A row of the table: a scope's - how many of its metrics the query matches, folded or not - or a metric's.
struct MetricRow {
    std::string scope;
    std::size_t metric{};   // a metric's row: into the summaries
    std::size_t count{};    // a scope's row: its metrics the query matches
    bool        group{};    // a scope's row
    bool        folded{};   // a scope's row: its metrics are not shown

    bool operator==(MetricRow const&) const = default;
};

/// What a query matches a metric against: "scope::name [unit]", the unit shown and, when it was sent in another, that
/// one too ("heater::tip [°C c℃]").
inline std::string metricWords(MetricInfo const&    info,
                               MetricSummary const& sum) {
    auto const  unit  = shownUnit(info, sum);
    std::string units = unit->display;
    if(unit->text != unit->display) { units += (units.empty() ? "" : " ") + unit->text; }
    return units.empty() ? fmt::format("{}::{}", info.scope, info.name)
                         : fmt::format("{}::{} [{}]", info.scope, info.name, units);
}

/// The table's rows: per scope its row and, unless `folded` holds the scope, its metrics `query` matches (every
/// word, any case); a scope none of whose metrics matches is left out.
inline std::vector<MetricRow> metricRows(MetricSummaries const&       metrics,
                                         std::string_view             query,
                                         std::set<std::string> const& folded) {
    std::vector<MetricRow>     rows;
    std::optional<std::size_t> group;   // the row of the scope the last metric had
    for(std::size_t i = 0; i < metrics.size(); ++i) {
        auto const& [info, sum] = metrics[i];
        if(!matchesWords(metricWords(info, sum), query)) { continue; }
        // the summaries come by scope: a new scope starts where the scope differs
        if(!group || rows[*group].scope != info.scope) {
            group = rows.size();
            rows.push_back({info.scope, 0, 0, true, folded.contains(info.scope)});
        }
        ++rows[*group].count;
        if(!rows[*group].folded) { rows.push_back({info.scope, i, 0, false, false}); }
    }
    return rows;
}

/// The row that shows metric `metric`: its own, or - folded - its scope's; none when the query hides it.
inline std::optional<std::size_t> rowOfMetric(std::vector<MetricRow> const& rows,
                                              MetricSummaries const&        metrics,
                                              std::size_t                   metric) {
    if(metric >= metrics.size()) { return std::nullopt; }
    auto const& scope = metrics[metric].first.scope;
    for(std::size_t r = 0; r < rows.size(); ++r) {
        auto const& row = rows[r];
        if(!row.group && row.metric == metric) { return r; }
        if(row.group && row.folded && row.scope == scope) { return r; }
    }
    return std::nullopt;
}

/// The metric `step` metric rows on from row `at` (back when negative; a scope's row counts as between its
/// neighbours), the ends held; none when no metric row is shown.
inline std::optional<std::size_t> metricAfter(std::vector<MetricRow> const& rows,
                                              std::size_t                   at,
                                              long                          step) {
    std::vector<std::size_t> shown;   // the metric rows' places
    for(std::size_t r = 0; r < rows.size(); ++r) {
        if(!rows[r].group) { shown.push_back(r); }
    }
    if(shown.empty()) { return std::nullopt; }
    auto const n = static_cast<long>(shown.size());
    // from a metric's row: its place; from a scope's: as if just before the next metric row (or after the last one)
    auto const next = std::ranges::lower_bound(shown, at);
    long       from = static_cast<long>(next - shown.begin());
    bool const on   = next != shown.end() && *next == at;
    if(!on) {
        if(step > 0) {
            --step;   // the first metric row after it is the first step
        } else if(step < 0) {
            --from;   // the last one before it is the first step back
            ++step;
        }
    }
    auto const to = std::clamp(from + step, 0L, n - 1);
    return rows[shown[static_cast<std::size_t>(to)]].metric;
}

/// The cells of the table's columns: the name's and the unit's (as wide as the longest, up to `maxName` and
/// `maxUnit`), then the fixed ones.
struct MetricColumns {
    static constexpr std::size_t value   = 11;   // "-123456.789", "160.000000"
    static constexpr std::size_t rate    = 9;    // "999.99K/s"
    static constexpr std::size_t trend   = 8;    // braille cells: a minute
    static constexpr std::size_t gap     = 2;
    static constexpr std::size_t maxUnit = 10;

    std::size_t name{4};
    std::size_t unit{4};

    /// A table row's cells: the indent, the name, three values, the unit, the rate, the trend, their gaps.
    [[nodiscard]] std::size_t cells() const {
        return 2 + name + (3 * (gap + value)) + gap + unit + gap + rate + gap + trend;
    }
};

namespace Detail {
    template<typename Width>
    std::string padTo(std::string  text,
                      std::size_t  cells,
                      bool         right,
                      Width const& width) {
        auto const has = width(text);
        if(has >= cells) { return text; }
        auto const pad = std::string(cells - has, ' ');
        return right ? pad + text : text + pad;
    }

    /// `text` in at most `cells` (its end cut to "…"), then padded to them
    template<typename Width>
    std::string fitTo(std::string  text,
                      std::size_t  cells,
                      Width const& width) {
        if(width(text) > cells) {
            while(!text.empty() && width(text) + 1 > cells) {
                // the last code point goes: its continuation bytes, then its lead
                auto at = text.size() - 1;
                while(at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0U) == 0x80U) { --at; }
                text.erase(at);
            }
            text += "…";
        }
        return padTo(std::move(text), cells, false, width);
    }
}   // namespace Detail

/// The columns for `metrics`: the name and the unit as wide as their longest (`width` measures cells), at least
/// the heading's.
template<typename Width>
MetricColumns metricColumns(MetricSummaries const& metrics,
                            std::size_t            maxName,
                            Width const&           width) {
    MetricColumns c;
    for(auto const& [info, sum] : metrics) {
        c.name = std::max(c.name, width(info.name));
        c.unit = std::max(c.unit, width(shownUnit(info, sum)->display));
    }
    c.name = std::min(c.name, std::max<std::size_t>(maxName, 4));
    c.unit = std::min(c.unit, MetricColumns::maxUnit);
    return c;
}

/// The table's heading, lined up with its rows.
template<typename Width>
std::string metricHeading(MetricColumns const& c,
                          Width const&         width) {
    auto const gap = std::string(MetricColumns::gap, ' ');
    return "  " + Detail::padTo("name", c.name, false, width) + gap
         + Detail::padTo("latest", MetricColumns::value, true, width) + gap
         + Detail::padTo("min", MetricColumns::value, true, width) + gap
         + Detail::padTo("max", MetricColumns::value, true, width) + gap
         + Detail::padTo("unit", c.unit, false, width) + gap
         + Detail::padTo("rate", MetricColumns::rate, true, width) + gap + "trend";
}

/// A metric's row: its name (shortened from the front past the column), the latest value, the least, the greatest
/// (numbers in the unit shown), that unit, the rate and `trend` (metricTrend), lined up.
template<typename Width>
std::string metricRowText(MetricInfo const&    info,
                          MetricSummary const& sum,
                          std::string const&   trend,
                          MetricColumns const& c,
                          Width const&         width) {
    auto const gap  = std::string(MetricColumns::gap, ' ');
    auto       name = info.name;
    if(width(name) > c.name) {
        // the end stays, where names differ: "…_rx_bytes"
        while(!name.empty() && width(name) + 1 > c.name) {
            name.erase(0, 1);
            while(!name.empty() && (static_cast<unsigned char>(name.front()) & 0xC0U) == 0x80U) {
                name.erase(0, 1);
            }
        }
        name.insert(0, "…");
    }
    auto const unit   = shownUnit(info, sum);
    auto const number = [&](double v) {
        return Detail::padTo(formatMetricNumber(v, *unit), MetricColumns::value, true, width);
    };
    return "  " + Detail::padTo(std::move(name), c.name, false, width) + gap + number(sum.latest)
         + gap + number(sum.min) + gap + number(sum.max) + gap
         + Detail::fitTo(unit->display, c.unit, width) + gap
         + Detail::padTo(formatRate(sum.rate), MetricColumns::rate, true, width) + gap + trend;
}

/// A scope's row: ▾ open, ▸ folded, its metrics shown.
inline std::string metricGroupText(MetricRow const& row) {
    return fmt::format("{} {} · {}",
                       row.folded ? "▸" : "▾",
                       row.scope.empty() ? std::string_view{"(no scope)"}
                                         : std::string_view{row.scope},
                       row.count);
}

/// The last minute of `values` (less, if it has less) as `cells` braille cells on one row: a few values picked evenly
/// from it, so that a trend costs the same however many values a minute has.
inline std::string metricTrend(std::span<MetricEntry const> values,
                               std::size_t                  cells) {
    if(values.empty()) {
        std::string blank(cells, ' ');   // not {cells, ' '}: that is two characters
        return blank;
    }
    auto const from  = values.back().recvTime - rangeLength(MetricRange::last60s);
    auto const begin = static_cast<std::size_t>(
      std::ranges::lower_bound(values, from, {}, &MetricEntry::recvTime) - values.begin());
    auto const               minute = values.subspan(begin);
    auto const               picks  = std::min(minute.size(), cells * 4);
    std::vector<MetricEntry> some;
    some.reserve(picks);
    for(std::size_t i = 0; i < picks; ++i) {
        // evenly from the first to the newest
        auto const at = picks == 1 ? minute.size() - 1 : i * (minute.size() - 1) / (picks - 1);
        some.push_back(minute[at]);
    }
    return metricChart(some, MetricRange::all, cells, 1).rows.front();   // over the time it has
}

}   // namespace uc_log::printer
