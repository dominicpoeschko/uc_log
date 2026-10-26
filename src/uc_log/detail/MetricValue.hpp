#pragma once

// uc_log::Metric, the value a metric() call site hands to a log line, and metric_tag, its base: what metric.hpp and the
// formatting half (detail/MetricFormat.hpp) both name, so each compiles alone.

#include "Lifetimebound.hpp"
#include "string_constant/string_constant.hpp"

namespace uc_log {
struct metric_tag {};

template<typename ValueType,
         sc::StringConstant Name,
         sc::StringConstant Unit,
         sc::StringConstant Scope>
struct Metric : metric_tag {
    ValueType const& value;

    Metric(ValueType const& value_ UC_LOG_LIFETIMEBOUND) : value{value_} {}

    static constexpr auto name  = Name;
    static constexpr auto unit  = Unit;
    static constexpr auto scope = Scope;
};

}   // namespace uc_log
