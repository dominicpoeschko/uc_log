#pragma once

#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace uc_log {
struct metric_tag {};

template<typename ValueType,
         sc::StringConstant Name_,
         sc::StringConstant Unit_,
         sc::StringConstant Scope_>
struct Metric : metric_tag {
    ValueType const& value;

    Metric(ValueType const& value_) : value{value_} {}

    static constexpr auto Name  = Name_;
    static constexpr auto Unit  = Unit_;
    static constexpr auto Scope = Scope_;
};

namespace detail {

    template<typename T>
    inline constexpr bool isMetricQuantity = false;

#if REMOTE_FMT_USE_MP_UNITS
    template<auto R, typename Rep>
    inline constexpr bool isMetricQuantity<mp_units::quantity<R, Rep>> = true;

    template<auto R, auto PO, typename Rep>
    inline constexpr bool isMetricQuantity<mp_units::quantity_point<R, PO, Rep>> = true;

    /// A quantity_point counts from its own zero (20 degC as 20, not 293.15 K).
    template<auto R,
             typename Rep>
    constexpr auto metricQuantity(mp_units::quantity<R,
                                                     Rep> const& q) {
        return q;
    }

    template<auto R,
             auto PO,
             typename Rep>
    constexpr auto metricQuantity(mp_units::quantity_point<R,
                                                           PO,
                                                           Rep> const& qp) {
        return qp.quantity_from_zero();
    }

    template<typename T>
    inline constexpr auto metricUnitOf
      = std::remove_cvref_t<decltype(metricQuantity(std::declval<T const&>()))>::unit;

    // The UTF-8 symbol when cataloged; inline strings are ASCII only, so the portable symbol,
    // with "deg" for its backtick ("`C").
    template<auto U>
    inline constexpr auto metricUnitStorage = [] {
        static constexpr auto sym     = mp_units::unit_symbol<mp_units::unit_symbol_formatting{
          .char_set = remote_fmt::use_catalog ? mp_units::character_set::utf8
                                              : mp_units::character_set::portable}>(U);
        constexpr std::size_t degrees = [] {
            std::size_t n = 0;
            for(std::size_t k = 0; k < sym.size(); ++k) {
                if(sym[k] == '`') { ++n; }
            }
            return n;
        }();
        std::array<char, sym.size() + 2 * degrees> out{};
        std::size_t                                i = 0;
        for(std::size_t k = 0; k < sym.size(); ++k) {
            if(sym[k] == '`') {
                for(char c : std::string_view{"deg"}) { out[i++] = c; }
            } else {
                out[i++] = static_cast<char>(sym[k]);
            }
        }
        return out;
    }();

    template<auto U>
    consteval auto metricUnitConstant() {
        return sc::create([] {
            return std::string_view{metricUnitStorage<U>.data(), metricUnitStorage<U>.size()};
        });
    }
#endif
}   // namespace detail

/// A value the host also records as a time series: `metric<"name", "unit", "scope">(x)`, or
/// `metric<"name", "scope">(q)` for an mp-units quantity, which brings its own unit.
template<sc::StringConstant Name,
         sc::StringConstant UnitOrScope = sc::StringConstant<>{},
         sc::StringConstant Scope       = sc::StringConstant<>{}>
constexpr auto metric(auto const& value) {
    using ValueType = std::remove_cvref_t<decltype(value)>;
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
}

namespace detail {

    template<std::size_t ArgIndex,
             typename Arg>
    consteval auto getMetricPrefix() {
        if constexpr(std::is_base_of_v<metric_tag, std::remove_cvref_t<Arg>>) {
            using MetricType     = std::remove_cvref_t<Arg>;
            constexpr auto scope = std::string_view{MetricType::Scope.storage.data(),
                                                    MetricType::Scope.storage.size()};
            constexpr auto name
              = std::string_view{MetricType::Name.storage.data(), MetricType::Name.storage.size()};
            constexpr auto unit
              = std::string_view{MetricType::Unit.storage.data(), MetricType::Unit.storage.size()};

            constexpr auto        prefix = std::string_view{"@METRIC("};
            constexpr std::size_t total_size
              = prefix.size() + scope.size() + name.size() + unit.size() + 5;

            std::array<char, total_size> result{};
            std::size_t                  pos = 0;

            for(char c : prefix) { result[pos++] = c; }

            for(char c : scope) { result[pos++] = c; }
            result[pos++] = ':';
            result[pos++] = ':';

            for(char c : name) { result[pos++] = c; }

            result[pos++] = '[';
            for(char c : unit) { result[pos++] = c; }
            result[pos++] = ']';

            result[pos++] = '=';

            return std::make_pair(result, pos);
        } else {
            return std::make_pair(std::array<char, 1>{}, std::size_t{0});
        }
    }

    template<char... chars,
             typename... Args>
    consteval auto injectMetricFmtString(sc::StringConstant<chars...> fmt,
                                         Args&&...) {
        constexpr auto input  = std::string_view{fmt.storage.data(), sizeof...(chars)};
        constexpr auto result = [input]<std::size_t... Is>(std::index_sequence<Is...>) {
            constexpr auto metrics_mask
              = std::array{std::is_base_of_v<metric_tag, std::remove_cvref_t<Args>>...};
            constexpr auto metric_prefixes = std::make_tuple(getMetricPrefix<Is, Args>()...);

            constexpr std::size_t outputSize
              = sizeof...(chars)
              + (std::size_t{0} + ... + (getMetricPrefix<Is, Args>().second + 1));
            std::array<char, outputSize> output{};
            std::size_t                  out_pos   = 0;
            std::size_t                  arg_index = 0;

            for(std::size_t i = 0; i < input.size(); ++i) {
                if(input[i] == '{') {
                    if(i + 1 < input.size() && input[i + 1] == '{') {
                        output[out_pos++] = '{';
                        output[out_pos++] = '{';
                        ++i;
                    } else {
                        std::size_t close_pos = i + 1;
                        while(close_pos < input.size() && input[close_pos] != '}') { ++close_pos; }

                        if(close_pos < input.size()) {
                            if(arg_index < metrics_mask.size() && metrics_mask[arg_index]) {
                                [&]<std::size_t... Js>(std::index_sequence<Js...>) {
                                    ((arg_index == Js ? [&]() {
                                        auto& metric_prefix = std::get<Js>(metric_prefixes);
                                        for(std::size_t j = 0; j < metric_prefix.second; ++j) {
                                            output[out_pos++] = metric_prefix.first[j];
                                        }
                                        for(std::size_t j = i; j <= close_pos; ++j) {
                                            output[out_pos++] = input[j];
                                        }
                                        output[out_pos++] = ')';
                                    }() : void()), ...);
                                }(std::index_sequence_for<Args...>{});
                            } else {
                                for(std::size_t j = i; j <= close_pos; ++j) {
                                    output[out_pos++] = input[j];
                                }
                            }
                            i = close_pos;
                            ++arg_index;
                        } else {
                            output[out_pos++] = input[i];
                        }
                    }
                } else if(input[i] == '}' && i + 1 < input.size() && input[i + 1] == '}') {
                    output[out_pos++] = '}';
                    output[out_pos++] = '}';
                    ++i;
                } else {
                    output[out_pos++] = input[i];
                }
            }

            return std::make_pair(output, out_pos);
        }(std::index_sequence_for<Args...>{});

        return [&result]<std::size_t... Is>(std::index_sequence<Is...>) {
            return sc::StringConstant<result.first[Is]...>{};
        }(std::make_index_sequence<result.second>{});
    }
}   // namespace detail
}   // namespace uc_log

namespace remote_fmt { namespace detail {

// A Metric is transparent on the wire, so its spec is checked against ValueType. Without this fmt
// finds no formatter for Metric and the argument is silently left unchecked.
#if REMOTE_FMT_USE_FMT_CHECK
    template<typename ValueType,
             sc::StringConstant Name,
             sc::StringConstant Unit,
             sc::StringConstant Scope>
    struct host_type<uc_log::Metric<ValueType, Name, Unit, Scope>> {
        using type = host_type_t<ValueType>;
    };
#endif

}}   // namespace remote_fmt::detail

namespace remote_fmt {

template<sc::StringConstant Name,
         sc::StringConstant Unit,
         sc::StringConstant Scope,
         typename ValueType>
struct formatter<uc_log::Metric<ValueType, Name, Unit, Scope>> {
    template<typename Printer>
    constexpr auto format(uc_log::Metric<ValueType,
                                         Name,
                                         Unit,
                                         Scope> const& val,
                          Printer&                     printer) const {
        if constexpr(uc_log::detail::isMetricQuantity<ValueType>) {
#if REMOTE_FMT_USE_MP_UNITS
            // the unit is in the marker already
            auto const q = uc_log::detail::metricQuantity(val.value);
            using Rep    = typename std::remove_cvref_t<decltype(q)>::rep;
            return formatter<Rep>{}.format(q.numerical_value_in(q.unit), printer);
#endif
        } else {
            return formatter<ValueType>{}.format(val.value, printer);
        }
    }
};
}   // namespace remote_fmt
