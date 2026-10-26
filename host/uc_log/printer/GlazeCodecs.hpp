#pragma once

// The glaze codecs of the TUI's saved filter (FilterFile.hpp): every enum by its enchantum names, and
// std::set<std::pair<K, V>> as an array of pairs. Only this header defines them; ControlProtocol.hpp's explicit
// specialisations of its own enums take precedence over the catch-all.

#include <cstddef>
#include <enchantum/enchantum.hpp>
#include <glaze/glaze.hpp>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace glz {
/// Registers every non-`std::byte` enum type with glaze using
/// `enchantum`-derived names, satisfying `glaze_enum_t<T>`.
template<typename T>
    requires(std::is_enum_v<T> && !std::is_same_v<T, std::byte>)
struct meta<T> {
    static constexpr auto value = []<std::size_t... Is>(std::index_sequence<Is...>) {
        constexpr auto names  = enchantum::names<T>;
        constexpr auto values = enchantum::values<T>;
        return std::apply(
          [](auto&&... args) { return glz::enumerate(std::forward<decltype(args)>(args)...); },
          std::tuple_cat(std::make_tuple(names[Is], values[Is])...));
    }(std::make_index_sequence<enchantum::count<T>>{});
};

/// Serialises std::set<std::pair<K,V>> as a JSON array-of-arrays [[k,v],...].
/// Glaze's default treats any container of pair<string,T> as a sorted map → {}
/// which then fails to round-trip.  Direct to/from specialisations bypass that.
template<typename K, typename V>
struct to<JSON, std::set<std::pair<K, V>>> {
    template<auto Opts,
             class B>
    static void op(std::set<std::pair<K,
                                      V>> const& value,
                   is_context auto&&             ctx,
                   B&&                           b,
                   auto&                         ix) {
        dump('[', b, ix);
        bool firstElem = true;
        for(auto const& [k, v] : value) {
            if(!firstElem) { dump(',', b, ix); }
            firstElem = false;
            dump('[', b, ix);
            serialize<JSON>::op<Opts>(k, ctx, b, ix);
            dump(',', b, ix);
            serialize<JSON>::op<Opts>(v, ctx, b, ix);
            dump(']', b, ix);
        }
        dump(']', b, ix);
    }
};

/// Reads a JSON array-of-arrays [[k,v],...] back into std::set<std::pair<K,V>>.
/// Delegates to glaze's built-in vector<tuple> reader (tuples are always arrays).
template<typename K, typename V>
struct from<JSON, std::set<std::pair<K, V>>> {
    template<auto Opts>
    static void op(std::set<std::pair<K,
                                      V>>& value,
                   is_context auto&&       ctx,
                   auto&&                  it,
                   auto&&                  end) {
        std::vector<std::tuple<K, V>> tmp;
        from<JSON, std::vector<std::tuple<K, V>>>::template op<Opts>(tmp, ctx, it, end);
        for(auto& [k, v] : tmp) { value.emplace(std::move(k), std::move(v)); }
    }
};
}   // namespace glz
