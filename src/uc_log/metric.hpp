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
    ValueType const&      value;
    static constexpr auto Name  = Name_;
    static constexpr auto Unit  = Unit_;
    static constexpr auto Scope = Scope_;
};

template<sc::StringConstant Name,
         sc::StringConstant Unit  = sc::StringConstant<>{},
         sc::StringConstant Scope = sc::StringConstant<>{}>
constexpr auto metric(auto const& value_) {
    using ValueType = std::remove_cvref_t<decltype(value_)>;
    return Metric<ValueType, Name, Unit, Scope>{.value = value_};
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

            for(char c : prefix) {
                result[pos++] = c;
            }

            for(char c : scope) {
                result[pos++] = c;
            }
            result[pos++] = ':';
            result[pos++] = ':';

            for(char c : name) {
                result[pos++] = c;
            }

            result[pos++] = '[';
            for(char c : unit) {
                result[pos++] = c;
            }
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

            std::array<char, sizeof...(chars) * 4> output{};
            std::size_t                            out_pos   = 0;
            std::size_t                            arg_index = 0;

            for(std::size_t i = 0; i < input.size(); ++i) {
                if(input[i] == '{') {
                    if(i + 1 < input.size() && input[i + 1] == '{') {
                        output[out_pos++] = '{';
                        output[out_pos++] = '{';
                        ++i;
                    } else {
                        std::size_t close_pos = i + 1;
                        while(close_pos < input.size() && input[close_pos] != '}') {
                            ++close_pos;
                        }

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
        return formatter<ValueType>{}.format(val.value, printer);
    }
};
}   // namespace remote_fmt
