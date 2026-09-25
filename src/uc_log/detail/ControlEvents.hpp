#pragma once

#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/ControlProtocol.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/LogFormat.hpp"
#include "uc_log/metric_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

/// Printer data to control-socket events, shared with the golden tests.
namespace uc_log::detail {

static_assert(static_cast<int>(control::Level::crit) == static_cast<int>(uc_log::LogLevel::crit)
                && static_cast<int>(control::Level::trace)
                     == static_cast<int>(uc_log::LogLevel::trace),
              "control::Level mirrors uc_log::LogLevel");

/// Out-of-range levels as crit: every level on the wire must be a name.
inline control::Level toControlLevel(uc_log::LogLevel level) {
    return level > uc_log::LogLevel::crit ? control::Level::crit
                                          : static_cast<control::Level>(level);
}

inline control::LogLine toLogLine(std::chrono::system_clock::time_point recv_time,
                                  LogEntry const&                       entry) {
    return control::LogLine{.recv_time  = logformat::toIso8601Utc(recv_time),
                            .channel    = static_cast<std::uint32_t>(entry.channel.channel),
                            .file       = entry.fileName,
                            .line       = static_cast<std::uint32_t>(entry.line),
                            .function   = entry.functionName,
                            .level      = toControlLevel(entry.logLevel),
                            .uc_time_ns = static_cast<std::int64_t>(entry.ucTime.time.count()),
                            .message    = entry.logMsg,
                            .module     = entry.module};
}

/// Non-finite values are skipped: JSON has no such number (glaze would write null).
inline std::vector<control::MetricSample>
toMetricSamples(std::chrono::system_clock::time_point recv_time,
                LogEntry const&                       entry) {
    std::vector<control::MetricSample> out;
    for(auto const& [info, value] : uc_log::extractMetrics(recv_time, entry)) {
        if(!std::isfinite(value.value)) { continue; }
        out.push_back(
          control::MetricSample{.name  = info.name,
                                .scope = info.scope,
                                .unit  = info.unit,
                                .time  = std::chrono::duration<double>(value.uc_time.time).count(),
                                .value = value.value});
    }
    return out;
}

inline control::StatusMessage toStatusMessage(std::chrono::system_clock::time_point time,
                                              std::string_view                      level,
                                              std::string_view                      text) {
    control::StatusMessage message{.time  = logformat::toIso8601Utc(time),
                                   .level = std::string{level},
                                   .text  = std::string{text}};
    std::ranges::replace(message.text, '\n', ' ');
    return message;
}

inline std::string statusFileLine(control::StatusMessage const& message) {
    return message.time + " [" + message.level + "] " + message.text;
}

}   // namespace uc_log::detail
