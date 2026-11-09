#pragma once

// Times as the windows print them.

#include <chrono>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <string>

namespace uc_log::printer {

/// "HH:MM:SS.mmm" in local time. localtime_r instead of std::chrono::zoned_time: libc++ has no tz database.
inline std::string localTimeOfDay(std::chrono::system_clock::time_point value) {
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
      value - std::chrono::time_point_cast<std::chrono::minutes>(value));
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      value - std::chrono::time_point_cast<std::chrono::seconds>(value));
    auto const time = std::chrono::system_clock::to_time_t(value);
    std::tm    local{};
    localtime_r(&time, &local);
    return fmt::format("{:%H:%M}:{:02}.{:03}", local, seconds.count(), milliseconds.count());
}

}   // namespace uc_log::printer
