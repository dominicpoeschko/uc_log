#pragma once

// A target time as a person types it ("1.5 ms", "20", "3 min") and the first log line at or after one: what a jump
// to a time and the time window's ends take.

#include "uc_log/printer/EntryStore.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace uc_log::printer {

/// A target time as a person types it: a number with an optional unit (ns, us, ms, s, min; seconds without one).
inline std::optional<std::chrono::nanoseconds> parseTargetTime(std::string_view text) {
    while(!text.empty() && text.front() == ' ') { text.remove_prefix(1); }
    while(!text.empty() && text.back() == ' ') { text.remove_suffix(1); }
    double     value{};
    auto const begin      = std::to_address(text.begin());
    auto const end        = std::to_address(text.end());
    auto const [rest, ec] = std::from_chars(begin, end, value);
    if(ec != std::errc{} || rest == begin || !std::isfinite(value) || value < 0) {
        return std::nullopt;
    }
    std::string_view unit{rest, static_cast<std::size_t>(end - rest)};
    while(!unit.empty() && unit.front() == ' ') { unit.remove_prefix(1); }
    double scale = 1e9;
    if(unit == "ns") {
        scale = 1;
    } else if(unit == "us" || unit == "µs") {
        scale = 1e3;
    } else if(unit == "ms") {
        scale = 1e6;
    } else if(unit == "min") {
        scale = 60e9;
    } else if(!unit.empty() && unit != "s") {
        return std::nullopt;
    }
    // past what the nanosecond clock holds (9.22e18 ns): no line there, and the cast would be UB
    if(!(value * scale < 9e18)) { return std::nullopt; }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(value * scale)};
}

/// The first line (from the oldest on) whose target time is at or after `time`.
inline std::optional<std::size_t> firstAtOrAfter(EntryChunkList::Snapshot const& lines,
                                                 std::chrono::nanoseconds        time) {
    for(std::size_t i = 0; i < lines.size(); ++i) {
        if(lines[i].common->ucTime.time >= time) { return i; }
    }
    return std::nullopt;
}

}   // namespace uc_log::printer
