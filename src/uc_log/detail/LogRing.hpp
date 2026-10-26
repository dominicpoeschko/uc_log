#pragma once

// Ring selection and dispatch of the RTT backend (RttComBackend.hpp's layouts). A layout puts its log rings in
// groups of 1 + Policy::numIsrRings (thread first, then one ring per served level), one group
// per logical channel or core; the backend computes the group's first index, this picks the
// ring inside it.

#include "../CortexM.hpp"
#include "../IsrPolicy.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace uc_log::detail {

// The ring for this write: the group's thread ring, or 1 + the policy's ISR ring;
// noLogRing when the policy has no ring for the running exception's level. Contexts says whether the write runs in
// Thread mode (CortexM.hpp; a fake in the host tests).
template<typename Policy,
         typename Contexts>
[[gnu::always_inline]] inline std::size_t logRing(std::size_t base) {
    if(Contexts::inThread()) { return base; }
    int level = 0;
    if constexpr(requires { Policy::activeLevel(); }) { level = Policy::activeLevel(); }
    std::size_t const ring = Policy::isrRing(level);
    if(ring == noLogRing) { return noLogRing; }
    return base + 1 + ring;
}

// f(std::integral_constant<std::size_t, ring>) for a ring index known only at run time (rtt's accessors take the ring as
// a template argument); false for an index past the rings. The one run-time-to-compile-time ring fold: the plain write
// below and LossPolicy::Counted's path (CountedRings.hpp).
template<std::size_t NumRings,
         typename F>
[[gnu::always_inline]] inline bool onRing(std::size_t ring,
                                          F&&         f) {
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        bool result = false;
        static_cast<void>(
          ((ring == Is ? (result = f(std::integral_constant<std::size_t, Is>{}), true) : false)
           || ...));
        return result;
    }(std::make_index_sequence<NumRings>{});
}

template<std::size_t NumRings,
         typename Rtt>
inline void writeLogRing(Rtt&                       rtt,
                         std::size_t                ring,
                         std::span<std::byte const> span) {
    static_cast<void>(onRing<NumRings>(ring, [&](auto i) {
        static_cast<void>(rtt.template write<decltype(i)::value>(span));
        return true;
    }));
}

}   // namespace uc_log::detail
