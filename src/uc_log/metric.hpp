#pragma once

// uc_log::metric and uc_log::Metric: what a call site names. Includable without remote_fmt, so a TU of a variant without
// USE_UC_LOG parses no formatter: the formatting half (quantity traits, the format-string markers, the Metric formatter)
// is detail/MetricFormat.hpp, included here only with USE_UC_LOG.

#include "detail/Lifetimebound.hpp"
#include "detail/MetricValue.hpp"
#include "string_constant/string_constant.hpp"

#include <type_traits>

#ifdef USE_UC_LOG
    #include "detail/MetricFormat.hpp"
#endif

namespace uc_log {
/// A value the host also records as a time series: `metric<"name", "unit", "scope">(x)`, or
/// `metric<"name", "scope">(q)` for an mp-units quantity, which brings its own unit.
template<sc::StringConstant Name,
         sc::StringConstant UnitOrScope = sc::StringConstant<>{},
         sc::StringConstant Scope       = sc::StringConstant<>{}>
constexpr auto metric(auto const& value UC_LOG_LIFETIMEBOUND) {
    using ValueType = std::remove_cvref_t<decltype(value)>;
#ifdef USE_UC_LOG
    if constexpr(detail::isMetricQuantity<ValueType>) {
    #if REMOTE_FMT_USE_MP_UNITS
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(Scope)>, sc::StringConstant<>>,
                      "a quantity brings its unit: metric<\"name\", \"scope\">(quantity)");
        return Metric<ValueType,
                      Name,
                      detail::metricUnitConstant<detail::metricUnitOf<ValueType>>(),
                      UnitOrScope>{value};
    #endif
    } else {
        return Metric<ValueType, Name, UnitOrScope, Scope>{value};
    }
#else
    // Only named in the no-log macro's dead arm, never evaluated: the arguments just have to name something. The
    // quantity rule ("a quantity brings its unit") is checked by the log variants, which every firmware builds.
    return Metric<ValueType, Name, UnitOrScope, Scope>{value};
#endif
}

}   // namespace uc_log
