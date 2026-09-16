#pragma once

// Ring selection and dispatch shared by the RTT backends. A backend lays its log rings out in
// groups of 1 + Policy::NumIsrRings (thread first, then one ring per served level), one group
// per logical channel or core; the backend computes the group's first index, this picks the
// ring inside it.

#include "../IsrPolicy.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace uc_log::detail {

template<std::size_t Value, std::size_t>
inline constexpr std::size_t repeatSize = Value;

// The ring for this write: the group's thread ring, or 1 + the policy's ISR ring;
// noLogRing when the policy has no ring for the running exception's level.
template<typename Policy>
[[gnu::always_inline]] inline std::size_t logRing(std::size_t base) {
    std::uint32_t ipsr{};
    asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    if(ipsr == 0) { return base; }
    int level = 0;
    if constexpr(requires { Policy::activeLevel(); }) { level = Policy::activeLevel(); }
    std::size_t const ring = Policy::isrRing(level);
    if(ring == noLogRing) { return noLogRing; }
    return base + 1 + ring;
}

template<std::size_t NumRings,
         typename Rtt>
inline void writeLogRing(Rtt&                       rtt,
                         std::size_t                ring,
                         std::span<std::byte const> span) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        static_cast<void>(
          ((ring == Is ? (static_cast<void>(rtt.template write<Is>(span)), true) : false) || ...));
    }(std::make_index_sequence<NumRings>{});
}

}   // namespace uc_log::detail
