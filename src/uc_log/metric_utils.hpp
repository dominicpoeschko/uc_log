#pragma once

#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
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

// @METRIC(scope::name[unit]=value), as views into the message.
struct MetricMarker {
    std::size_t      begin{};
    std::size_t      end{};
    std::string_view scope;
    std::string_view name;
    std::string_view unit;
    std::string_view value;
};

/// The closing parenthesis is looked for behind the '=': a unit may hold one ("J/(kg K)").
inline std::optional<MetricMarker> nextMetricMarker(std::string_view msg,
                                                    std::size_t      pos) {
    static constexpr std::string_view Start{"@METRIC("};
    while((pos = msg.find(Start, pos)) != std::string_view::npos) {
        std::size_t const content  = pos + Start.size();
        std::size_t const scopeEnd = msg.find("::", content);
        if(scopeEnd == std::string_view::npos) { return std::nullopt; }

        std::size_t const nameStart = scopeEnd + 2;
        std::size_t const nameEnd   = msg.find_first_of("[=)", nameStart);
        std::size_t       equals    = nameEnd;
        std::string_view  unit;
        if(nameEnd != std::string_view::npos && msg[nameEnd] == '[') {
            std::size_t const unitEnd = msg.find("]=", nameEnd);
            if(unitEnd == std::string_view::npos || unitEnd > msg.find(Start, content)) {
                pos = content;   // this marker's unit is unclosed; later ones may still be fine
                continue;
            }
            unit   = msg.substr(nameEnd + 1, unitEnd - nameEnd - 1);
            equals = unitEnd + 1;
        }
        if(equals == std::string_view::npos || msg[equals] != '='
           || msg.substr(content, scopeEnd - content).find(')') != std::string_view::npos)
        {
            pos = content;
            continue;
        }
        std::size_t const close = msg.find(')', equals);
        if(close == std::string_view::npos) { return std::nullopt; }

        return MetricMarker{.begin = pos,
                            .end   = close + 1,
                            .scope = msg.substr(content, scopeEnd - content),
                            .name  = msg.substr(nameStart, nameEnd - nameStart),
                            .unit  = unit,
                            .value = msg.substr(equals + 1, close - equals - 1)};
    }
    return std::nullopt;
}

inline std::vector<std::pair<MetricInfo,
                             MetricEntry>>
extractMetrics(std::chrono::system_clock::time_point recv_time,
               uc_log::detail::LogEntry const&       logEntry) {
    std::vector<std::pair<MetricInfo, MetricEntry>> metrics;

    std::string_view const msg{logEntry.logMsg};
    std::size_t            pos = 0;

    while(auto const marker = nextMetricMarker(msg, pos)) {
        pos = marker->end;

        std::string scope{marker->scope};
        if(scope.empty()) { scope = logEntry.fileName + ":" + std::to_string(logEntry.line); }

        // from_chars: no exceptions on this path (it runs inside the locked gui/tcp add),
        // no locale dependence, and out-of-range values are skipped instead of throwing
        double value{};
        auto const [ptr, ec]
          = std::from_chars(marker->value.data(), std::to_address(marker->value.end()), value);
        if(ec == std::errc{} && ptr != marker->value.data()) {
            metrics.emplace_back(MetricInfo{.scope = std::move(scope),
                                            .name  = std::string{marker->name},
                                            .unit  = std::string{marker->unit}},
                                 MetricEntry{.recv_time = recv_time,
                                             .level     = logEntry.logLevel,
                                             .uc_time   = logEntry.ucTime,
                                             .value     = value});
        }
    }

    return metrics;
}
}   // namespace uc_log
