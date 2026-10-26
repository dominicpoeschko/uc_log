#pragma once

// The Filter tab's "noisy locations": which source locations log so much more than the rest that they hide it. Pure:
// a location list with counts in, a cutoff and the locations above it out.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

namespace uc_log::printer {

enum class OutlierMethod : std::uint8_t {
    iqrTukey,           // cutoff = Q3 + k × (Q3 - Q1)
    topNPercent,        // exclude the top N% most frequent locations
    absoluteThreshold   // exclude locations with count > N
};

template<typename Location>
struct OutlierResult {
    std::size_t           cutoff{0};
    std::size_t           q1{0};
    std::size_t           median{0};
    std::size_t           q3{0};
    std::vector<Location> wouldExclude;
    bool                  valid{false};
};

template<typename Location>
[[nodiscard]] OutlierResult<Location>
computeOutliers(std::vector<std::pair<Location,
                                      std::size_t>> const& locations,
                OutlierMethod                              method,
                double                                     iqrK,
                double                                     topNPct,
                std::size_t                                absThreshold) {
    if(locations.size() < 3) { return {}; }

    auto counts = locations | std::views::values | std::ranges::to<std::vector>();
    std::ranges::sort(counts);
    auto const n = counts.size();

    auto medianOf = [&counts](std::size_t lo, std::size_t hi) -> std::size_t {
        auto const len = hi - lo;
        if(len % 2 == 1) { return counts[lo + len / 2]; }
        return (counts[lo + len / 2 - 1] + counts[lo + len / 2]) / 2;
    };
    std::size_t const medianVal = medianOf(0, n);
    std::size_t const q1        = medianOf(0, n / 2);
    std::size_t const q3        = medianOf((n % 2 == 1) ? n / 2 + 1 : n / 2, n);

    std::size_t cutoff{};
    switch(method) {
    case OutlierMethod::iqrTukey:
        {
            double const iqr = (q3 >= q1) ? static_cast<double>(q3 - q1) : 0.0;
            cutoff
              = static_cast<std::size_t>(static_cast<double>(q3) + iqrK * (iqr > 0.0 ? iqr : 1.0));
            cutoff = std::max(cutoff, std::size_t{1});
            break;
        }
    case OutlierMethod::topNPercent:
        {
            if(topNPct <= 0.0 || topNPct >= 100.0) {
                cutoff = counts.back();
                break;
            }
            auto const keep = static_cast<std::size_t>(
              std::floor((1.0 - topNPct / 100.0) * static_cast<double>(n)));
            cutoff = (keep > 0 && keep < n) ? counts[keep] : counts.back();
            break;
        }
    case OutlierMethod::absoluteThreshold: cutoff = absThreshold; break;
    }

    return OutlierResult<Location>{
      .cutoff       = cutoff,
      .q1           = q1,
      .median       = medianVal,
      .q3           = q3,
      .wouldExclude = locations
                    | std::views::filter([cutoff](auto const& kv) { return kv.second > cutoff; })
                    | std::views::keys | std::ranges::to<std::vector>(),
      .valid        = true};
}

}   // namespace uc_log::printer
