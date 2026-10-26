#pragma once

// The formatting half of uc_log::metric (metric.hpp includes it with USE_UC_LOG): the mp-units quantity traits and
// unit symbols, the "@METRIC(...)" markers put into a line's format string, and the Metric formatter. Needs remote_fmt,
// so only a log variant parses it.

#include "MetricValue.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uc_log {
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
          .char_set = remote_fmt::useCatalog ? mp_units::character_set::utf8
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

namespace detail {

    // "@METRIC(" scope "::" name "[" unit "]=": one array per Metric type, built once per type.
    template<typename M>
    inline constexpr auto metricPrefixStorage = [] {
        constexpr std::array<std::string_view, 7> parts{"@METRIC(",
                                                        M::scope.stringView,
                                                        "::",
                                                        M::name.stringView,
                                                        "[",
                                                        M::unit.stringView,
                                                        "]="};
        constexpr std::size_t                     size = [&] {
            std::size_t n = 0;
            for(auto p : parts) { n += p.size(); }
            return n;
        }();
        std::array<char, size> out{};
        std::size_t            i = 0;
        for(auto p : parts) {
            for(char c : p) { out[i++] = c; }
        }
        return out;
    }();

    /// The prefix a field of an argument of type Arg gets: empty unless Arg is a Metric (whose
    /// prefix is never empty, at least "@METRIC(::[]=").
    template<typename Arg>
    consteval std::string_view metricPrefixOf() {
        using A = std::remove_cvref_t<Arg>;
        if constexpr(std::is_base_of_v<metric_tag, A>) {
            return {metricPrefixStorage<A>.data(), metricPrefixStorage<A>.size()};
        } else {
            return {};
        }
    }

    template<char... Chars,
             typename... Args>
    consteval auto injectMetricFmtString(sc::StringConstant<Chars...> fmt,
                                         Args&&...) {
        if constexpr(!(std::is_base_of_v<metric_tag, std::remove_cvref_t<Args>> || ...)) {
            // No metric argument: the walk below would copy every character unchanged (`{{`,
            // `}}` and every field included), so the same StringConstant comes back without it.
            // Most lines take this branch, and every log line of a firmware pays for the walk.
            return fmt;
        } else {
            constexpr auto input  = fmt.stringView;
            constexpr auto result = [input] {
                constexpr std::array<std::string_view, sizeof...(Args)> prefixes{
                  metricPrefixOf<Args>()...};
                // + 1 per argument for the ')' (a non-metric adds room it never uses; the result
                // is cut at outPos)
                constexpr std::size_t outputSize = [&] {
                    std::size_t n = input.size();
                    for(auto p : prefixes) { n += p.size() + 1; }
                    return n;
                }();
                std::array<char, outputSize> output{};
                std::size_t                  outPos   = 0;
                std::size_t                  argIndex = 0;

                for(std::size_t i = 0; i < input.size(); ++i) {
                    if(input[i] == '{') {
                        if(i + 1 < input.size() && input[i + 1] == '{') {
                            output[outPos++] = '{';
                            output[outPos++] = '{';
                            ++i;
                        } else {
                            std::size_t closePos = i + 1;
                            while(closePos < input.size() && input[closePos] != '}') { ++closePos; }

                            if(closePos < input.size()) {
                                if(argIndex < prefixes.size() && !prefixes[argIndex].empty()) {
                                    for(char c : prefixes[argIndex]) { output[outPos++] = c; }
                                    for(std::size_t j = i; j <= closePos; ++j) {
                                        output[outPos++] = input[j];
                                    }
                                    output[outPos++] = ')';
                                } else {
                                    for(std::size_t j = i; j <= closePos; ++j) {
                                        output[outPos++] = input[j];
                                    }
                                }
                                i = closePos;
                                ++argIndex;
                            } else {
                                output[outPos++] = input[i];
                            }
                        }
                    } else if(input[i] == '}' && i + 1 < input.size() && input[i + 1] == '}') {
                        output[outPos++] = '}';
                        output[outPos++] = '}';
                        ++i;
                    } else {
                        output[outPos++] = input[i];
                    }
                }

                return std::make_pair(output, outPos);
            }();

            return [&result]<std::size_t... Is>(std::index_sequence<Is...>) {
                return sc::StringConstant<result.first[Is]...>{};
            }(std::make_index_sequence<result.second>{});
        }
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
    struct HostType<uc_log::Metric<ValueType, Name, Unit, Scope>> {
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
    // transparent: the value's own descriptor (a quantity's number - the unit is in the marker already)
    static consteval auto descriptor() {
        if constexpr(uc_log::detail::isMetricQuantity<ValueType>) {
#if REMOTE_FMT_USE_MP_UNITS
            using Rep = typename std::remove_cvref_t<decltype(uc_log::detail::metricQuantity(
              std::declval<ValueType const&>()))>::rep;
            return descriptorOf<Rep>();
#endif
        } else {
            return descriptorOf<ValueType>();
        }
    }

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
