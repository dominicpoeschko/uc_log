#pragma once

// A metric series as a window shows it, for any front end: the numbers of a series (count, latest, min, mean, max,
// rate) with the unit they are shown in (MetricUnit.hpp) and a plot of it as braille cells (2 x 4 dots a cell), with
// the value and time its axes span. A range ("the last 60 s") ends at the series' newest value, never at the clock:
// what a plot shows depends on the values alone.

#include "uc_log/printer/MetricUnit.hpp"
#include "uc_log/printer/metric_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// The numbers in the unit the metric was sent in; `unit` says how they are shown (the store reads it once a series).
struct MetricSummary {
    std::size_t count{};
    double      latest{};
    double      min{};
    double      mean{};
    double      max{};
    double      rate{};   // values a second, over the series' span (host receive times)
    std::shared_ptr<MetricUnit const> unit{};   // from the store; none in a summary made by hand
};

/// The unit `sum`'s values are shown in: the store's, or - a summary made without one - read from `info`.
inline std::shared_ptr<MetricUnit const> shownUnit(MetricInfo const&    info,
                                                   MetricSummary const& sum) {
    return sum.unit ? sum.unit : std::make_shared<MetricUnit const>(readMetricUnit(info.unit));
}

/// A series' running numbers: its least, greatest and sum, taken in as each value comes, so that its summary walks
/// no value (a window lists every metric every frame). Values that go (the oldest, dropped) are counted out by
/// counting the rest anew. The sum adds in the series' order, as `summarize` does: the two agree to the bit.
struct MetricTally {
    double min{};
    double max{};
    double sum{};

    /// `e`, the series' newest; `first`: the series had none before.
    void add(MetricEntry const& e,
             bool               first) {
        if(first) { *this = {e.value, e.value, 0.0}; }
        min = std::min(min, e.value);
        max = std::max(max, e.value);
        sum += e.value;
    }

    /// Counted anew from `values`, the whole series.
    void recount(std::span<MetricEntry const> values) {
        *this = {};
        for(std::size_t i = 0; i < values.size(); ++i) { add(values[i], i == 0); }
    }

    /// The summary of `values`, the series this tally has taken in: its ends give the count, the latest and the rate.
    [[nodiscard]] MetricSummary summary(std::span<MetricEntry const> values) const {
        MetricSummary s;
        s.count = values.size();
        if(values.empty()) { return s; }
        s.latest = values.back().value;
        s.min    = min;
        s.max    = max;
        s.mean   = sum / static_cast<double>(values.size());
        auto const span
          = std::chrono::duration<double>(values.back().recvTime - values.front().recvTime).count();
        if(span > 0) { s.rate = static_cast<double>(values.size() - 1) / span; }
        return s;
    }
};

/// The summary of a whole series, walking it (MetricTally keeps one as values come).
inline MetricSummary summarize(std::span<MetricEntry const> values) {
    MetricTally t;
    t.recount(values);
    return t.summary(values);
}

/// Values a second, short: "9.95/s", "120/s", "1.51K/s", "2.30M/s".
inline std::string formatRate(double perSecond) {
    if(perSecond >= 1e6) { return fmt::format("{:.2f}M/s", perSecond / 1e6); }
    if(perSecond >= 1e3) { return fmt::format("{:.2f}K/s", perSecond / 1e3); }
    return fmt::format("{:.3g}/s", perSecond);
}

enum class MetricRange : std::uint8_t { all, last10s, last60s, last10min };

inline constexpr std::array metricRanges{MetricRange::all,
                                         MetricRange::last10s,
                                         MetricRange::last60s,
                                         MetricRange::last10min};

inline std::string_view rangeName(MetricRange r) {
    switch(r) {
    case MetricRange::all:       return "all";
    case MetricRange::last10s:   return "the last 10 s";
    case MetricRange::last60s:   return "the last 60 s";
    case MetricRange::last10min: return "the last 10 min";
    }
    return "all";
}

inline std::chrono::seconds rangeLength(MetricRange r) {
    switch(r) {
    case MetricRange::all:       return std::chrono::seconds{0};
    case MetricRange::last10s:   return std::chrono::seconds{10};
    case MetricRange::last60s:   return std::chrono::seconds{60};
    case MetricRange::last10min: return std::chrono::seconds{600};
    }
    return std::chrono::seconds{0};
}

/// A plot: `rows` of braille characters, the value at its top and bottom row, the seconds it spans (its right edge
/// is the newest value), and how many values it drew.
struct MetricChart {
    std::vector<std::string> rows;
    double                   top{};
    double                   bottom{};
    double                   seconds{};
    std::size_t              drawn{};
};

namespace Detail {
    // U+2800 + the dot bits: column 0 is bits 0, 1, 2, 6 from the top, column 1 bits 3, 4, 5, 7
    inline std::string braille(std::uint8_t bits) {
        auto const  cp = static_cast<std::uint32_t>(0x2800U + bits);
        std::string out;
        out += static_cast<char>(0xE0U | (cp >> 12U));
        out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (cp & 0x3FU));
        return out;
    }

    constexpr std::array<std::array<std::uint8_t, 4>, 2> dotBits{
      {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}}
    };
}   // namespace Detail

/// `values` (in time order) of `range` as a line plot `width` x `height` cells, consecutive values joined.
inline MetricChart metricChart(std::span<MetricEntry const> values,
                               MetricRange                  range,
                               std::size_t                  width,
                               std::size_t                  height) {
    MetricChart chart;
    width  = std::max<std::size_t>(width, 1);
    height = std::max<std::size_t>(height, 1);
    std::vector<std::uint8_t> dots(width * height, 0);
    if(!values.empty()) {
        auto const newest = values.back().recvTime;
        auto       begin  = values.begin();
        if(range != MetricRange::all) {
            auto const from = newest - rangeLength(range);
            begin           = std::ranges::lower_bound(values, from, {}, &MetricEntry::recvTime);
        }
        auto const shown
          = values.subspan(static_cast<std::size_t>(std::ranges::distance(values.begin(), begin)));
        auto const oldest
          = range == MetricRange::all ? values.front().recvTime : newest - rangeLength(range);
        chart.seconds = std::chrono::duration<double>(newest - oldest).count();
        chart.drawn   = shown.size();
        if(!shown.empty()) {
            auto lo = shown.front().value;
            auto hi = lo;
            for(auto const& e : shown) {
                lo = std::min(lo, e.value);
                hi = std::max(hi, e.value);
            }
            if(hi <= lo) {   // all the same: a flat line in the middle
                auto const pad = std::abs(lo) > 0 ? std::abs(lo) * 0.1 : 1.0;
                lo -= pad;
                hi += pad;
            }
            chart.top           = hi;
            chart.bottom        = lo;
            auto const dotsWide = static_cast<double>(width * 2 - 1);
            auto const dotsHigh = static_cast<double>(height * 4 - 1);
            // dots per clock tick and per value, rounded by floor(+ 0.5) on what is never negative: lround and a
            // duration<double> per value were most of a plot's cost
            auto const ticks  = static_cast<double>((newest - oldest).count());
            auto const xScale = ticks > 0 ? dotsWide / ticks : 0.0;
            auto const yScale = dotsHigh / (hi - lo);
            auto const xOf    = [&](MetricEntry const& e) {
                auto const t = static_cast<double>((e.recvTime - oldest).count()) * xScale;
                return static_cast<long>(std::floor(std::clamp(t, 0.0, dotsWide) + 0.5));
            };
            auto const yOf = [&](MetricEntry const& e) {
                return static_cast<long>(
                  std::floor(std::clamp((hi - e.value) * yScale, 0.0, dotsHigh) + 0.5));
            };
            auto const set = [&](long x, long y) {
                if(x < 0 || y < 0 || x > static_cast<long>(dotsWide)
                   || y > static_cast<long>(dotsHigh))
                {
                    return;
                }
                auto const cx = static_cast<std::size_t>(x) / 2;
                auto const cy = static_cast<std::size_t>(y) / 4;
                dots[cy * width + cx] |= Detail::dotBits[static_cast<std::size_t>(x) % 2]
                                                        [static_cast<std::size_t>(y) % 4];
            };
            auto const line = [&](long x0, long y0, long x, long y) {   // Bresenham
                long const dx  = std::abs(x - x0);
                long const dy  = -std::abs(y - y0);
                long const sx  = x0 < x ? 1 : -1;
                long const sy  = y0 < y ? 1 : -1;
                long       err = dx + dy;
                while(true) {
                    set(x0, y0);
                    if(x0 == x && y0 == y) { break; }
                    long const e2 = 2 * err;
                    if(e2 >= dy) {
                        err += dy;
                        x0 += sx;
                    }
                    if(e2 <= dx) {
                        err += dx;
                        y0 += sy;
                    }
                }
            };

            // each value joined to the next: per dot column the first, last, lowest and highest value's dot (the
            // values within a column join as one vertical stroke), then each column's last to the next one's first -
            // the same dots as a line per value, at a cost of one pass however many values share a column
            struct Column {
                long first{}, last{}, low{}, high{};
                bool used{};
            };

            std::vector<Column> columns(static_cast<std::size_t>(dotsWide) + 1);
            for(auto const& e : shown) {
                auto const x
                  = static_cast<std::size_t>(std::clamp(xOf(e), 0L, static_cast<long>(dotsWide)));
                auto const y = yOf(e);
                auto&      c = columns[x];
                if(!c.used) {
                    c = {.first = y, .last = y, .low = y, .high = y, .used = true};
                } else {
                    c.last = y;
                    c.low  = std::min(c.low, y);
                    c.high = std::max(c.high, y);
                }
            }
            std::optional<std::pair<long, long>> previous;
            for(std::size_t x = 0; x < columns.size(); ++x) {
                auto const& c = columns[x];
                if(!c.used) { continue; }
                auto const at = static_cast<long>(x);
                if(previous) { line(previous->first, previous->second, at, c.first); }
                for(long y = c.low; y <= c.high; ++y) { set(at, y); }
                previous = std::pair{at, c.last};
            }
        }
    }
    for(std::size_t r = 0; r < height; ++r) {
        std::string row;
        for(std::size_t c = 0; c < width; ++c) { row += Detail::braille(dots[r * width + c]); }
        chart.rows.push_back(std::move(row));
    }
    return chart;
}

}   // namespace uc_log::printer
