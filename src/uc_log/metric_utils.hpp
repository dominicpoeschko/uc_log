#pragma once

#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log {
struct MetricInfo {
    std::string scope;
    std::string name;
    std::string unit;

    auto operator<=>(MetricInfo const&) const = default;
};

struct MetricEntry {
    std::chrono::system_clock::time_point recv_time;
    uc_log::LogLevel                      level;
    uc_log::detail::LogEntry::UcTime      uc_time;
    double                                value;
};

inline std::vector<std::pair<MetricInfo,
                             MetricEntry>>
extractMetrics(std::chrono::system_clock::time_point recv_time,
               uc_log::detail::LogEntry const&       logEntry) {
    std::vector<std::pair<MetricInfo, MetricEntry>> metrics;

    std::string_view const msg{logEntry.logMsg};
    std::size_t            pos = 0;

    while((pos = msg.find("@METRIC(", pos)) != std::string_view::npos) {
        pos += 8;

        std::size_t const end_pos = msg.find(')', pos);
        if(end_pos == std::string_view::npos) {
            break;
        }

        std::string_view const metric_content = msg.substr(pos, end_pos - pos);

        std::size_t const scope_end = metric_content.find("::");
        if(scope_end == std::string_view::npos) {
            pos = end_pos + 1;
            continue;
        }

        std::string scope{metric_content.substr(0, scope_end)};
        if(scope.empty()) {
            scope = logEntry.fileName + ":" + std::to_string(logEntry.line);
        }

        std::string_view const remainder = metric_content.substr(scope_end + 2);

        std::size_t const equals_pos = remainder.find('=');
        if(equals_pos == std::string_view::npos) {
            pos = end_pos + 1;
            continue;
        }

        std::string_view const name_and_unit = remainder.substr(0, equals_pos);
        std::string_view const value_str     = remainder.substr(equals_pos + 1);

        std::string name;
        std::string unit;

        std::size_t const bracket_start = name_and_unit.find('[');
        if(bracket_start != std::string_view::npos) {
            std::size_t const bracket_end = name_and_unit.find(']', bracket_start);
            if(bracket_end != std::string_view::npos) {
                name = std::string{name_and_unit.substr(0, bracket_start)};
                unit = std::string{
                  name_and_unit.substr(bracket_start + 1, bracket_end - bracket_start - 1)};
            } else {
                name = std::string{name_and_unit};
            }
        } else {
            name = std::string{name_and_unit};
        }

        try {
            double const value = std::stod(std::string{value_str});

            metrics.emplace_back(MetricInfo{.scope = scope, .name = name, .unit = unit},
                                 MetricEntry{.recv_time = recv_time,
                                             .level     = logEntry.logLevel,
                                             .uc_time   = logEntry.ucTime,
                                             .value     = value});
        } catch(std::invalid_argument const&) {
        }

        pos = end_pos + 1;
    }

    return metrics;
}
}   // namespace uc_log
