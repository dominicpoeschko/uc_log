#pragma once

// LossPolicy::Counted's write path of the RTT backend (LossPolicy.hpp, LossLedger.hpp).
// A backend with LossPolicy::Silent never instantiates any of this.
//
// In skip mode every record is staged and published at its end (all of it or none: no holes);
// in block or trim mode the ring is written as without the policy and only the records
// DebuggerPresentFunction refused are counted.

#include "../LossPolicy.hpp"
#include "LogRing.hpp"
#include "LossLedger.hpp"
#include "rtt/rtt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace uc_log::detail {

// The static state of a counting backend; the backend is the first parameter, so two backends
// never share a ledger. `lossLedger` is what the printer finds by name in the map.
template<typename Backend, typename Loss, std::size_t NumRings>
struct LossMembers {};

template<typename Backend, typename Loss, std::size_t NumRings>
    requires LossPolicy::isCounted<Loss>
struct LossMembers<Backend, Loss, NumRings> {
    static constexpr bool waits = Loss::panicStallPolls != 0;

    // gnu::used: the printer reads it whether or not the image does; gcc LTO keeps the name
    [[gnu::used]] static inline constinit LossLedger<NumRings> lossLedger{};
    static inline constinit std::conditional_t<waits, PanicWait<NumRings>, NoLossState> panicWait{};
};

template<typename Loss, rtt::BufferMode Mode, std::size_t NumRings>
struct Counting {
    static constexpr bool staged = Mode == rtt::BufferMode::skip;
    static constexpr bool waits  = staged && Loss::panicStallPolls != 0;

    template<typename Rtt>
    [[gnu::always_inline]] static void begin(Rtt&                  rtt,
                                             LossLedger<NumRings>& ledger,
                                             std::size_t           ring) {
        if constexpr(staged) {
            static_cast<void>(onRing<NumRings>(ring, [&](auto i) {
                ledger.recordBegin(ring, rtt.template writeOffset<decltype(i)::value>());
                return true;
            }));
        }
    }

    template<typename Rtt,
             typename Wait>
    [[gnu::always_inline]] static void write(Rtt&                       rtt,
                                             LossLedger<NumRings>&      ledger,
                                             Wait&                      wait,
                                             std::size_t                ring,
                                             bool                       present,
                                             std::span<std::byte const> span) {
        if(!ledger.admits(ring)) { return; }   // the rest of a dropped record
        if(present
           && onRing<NumRings>(
             ring,
             [&](auto i) {
                 constexpr std::size_t index = decltype(i)::value;
                 if constexpr(waits) {
                     // asked only when the piece does not fit; says no at once outside panic mode
                     StallWaiter waiter{wait.on ? Loss::panicStallPolls : 0U,
                                        wait.goneAtPlus1[index],
                                        Loss::panicStallUs,
                                        &kvasir_announced_reset_time_us};
                     return rtt.template stage<index>(span, ledger.staged(ring), waiter);
                 } else if constexpr(staged) {
                     return rtt.template stage<index>(span, ledger.staged(ring));
                 } else {
                     static_cast<void>(rtt.template write<index>(span));
                     return true;
                 }
             }))
        {
            return;
        }
        ledger.dropped(ring);
    }

    template<typename Rtt>
    [[gnu::always_inline]] static void end(Rtt&                  rtt,
                                           LossLedger<NumRings>& ledger,
                                           std::size_t           ring) {
        if(ledger.recordEnd(ring)) {
            if constexpr(staged) {
                static_cast<void>(onRing<NumRings>(ring, [&](auto i) {
                    rtt.template publish<decltype(i)::value>(ledger.staged(ring));
                    return true;
                }));
            }
        }
    }

    // Until the host has read every log ring, or has stopped reading one.
    template<typename Rtt>
    static void drain(Rtt&                 rtt,
                      PanicWait<NumRings>& wait) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
              [&] {
                  StallWaiter waiter{Loss::panicStallPolls,
                                     wait.goneAtPlus1[Is],
                                     Loss::panicStallUs,
                                     &kvasir_announced_reset_time_us};
                  while(rtt.template unread<Is>() != 0 && waiter(rtt.template hostOffset<Is>())) {}
              }(),
              ...);
        }(std::make_index_sequence<NumRings>{});
    }
};

}   // namespace uc_log::detail
