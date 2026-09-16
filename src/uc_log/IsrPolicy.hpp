#pragma once

// How an RTT backend keeps one producer per ISR ring.
//
// A log record is many write() calls, and an RTT ring is only safe with one producer. A
// core's thread is one producer; its exceptions are not: with NVIC priorities in use, an ISR
// at a higher level preempts a lower one mid-record, and if both log on one ring the second
// record lands inside the first. Exceptions at the *same* level never preempt each other, so
// what matters is which levels log, and the policy says so:
//
//   SingleLevel<SilentLevels<...>>                     one ISR ring; every exception that
//                                                      may log is at one level (the default)
//   PerLevel<Active, Levels<...>, SilentLevels<...>>   one ISR ring per listed level; the
//                                                      preferred choice when levels mix
//   MaskedRecord                                       one ISR ring; an ISR's record is
//                                                      written with PRIMASK set (latency)
//
// SilentLevels names the levels whose ISRs never log (SysTick's level 0, typically): they
// get no ring and do not count. That is a promise the compiler cannot check: under PerLevel
// a silent ISR that logs anyway drops its records, under SingleLevel it shares the ring.
//
// The policy's constexpr members are a contract Kvasir's Startup checks against the enabled
// interrupts of every core (kvasir/StartUp/ListRules.hpp): a level that is enabled but
// neither served nor silent fails the build, and so does a priority the check cannot read.
// Startup checks the user backend whether or not its list names it. What it reads are the
// priorities the init steps write: one changed at run time is not seen, and a backend used
// without Startup is unchecked.
//
// NMI and HardFault are outside the NVIC levels, cannot be masked and preempt anything: their
// records go on the first ISR ring under every policy, after whatever they cut off.

#include "detail/IsrRecordGuard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uc_log {

template<int... Ls>
struct Levels {};

template<int... Ls>
struct SilentLevels {};

namespace detail {
    // Four priority bits is the most a Cortex-M implements.
    inline constexpr int MaxIsrLevel = 15;

    inline constexpr std::size_t noLogRing = ~std::size_t{0};

    template<typename T>
    inline constexpr bool isLevels = false;
    template<int... Ls>
    inline constexpr bool isLevels<Levels<Ls...>> = true;

    template<typename T>
    inline constexpr bool isSilentLevels = false;
    template<int... Ls>
    inline constexpr bool isSilentLevels<SilentLevels<Ls...>> = true;

    template<typename T>
    struct LevelArray;

    template<int... Ls>
    struct LevelArray<Levels<Ls...>> {
        static constexpr std::array<int, sizeof...(Ls)> value{Ls...};
    };

    template<int... Ls>
    struct LevelArray<SilentLevels<Ls...>> {
        static constexpr std::array<int, sizeof...(Ls)> value{Ls...};
    };

    template<std::size_t N>
    constexpr bool levelsValid(std::array<int,
                                          N> const& ls) {
        for(std::size_t i = 0; i < N; ++i) {
            if(ls[i] < 0 || ls[i] > MaxIsrLevel) { return false; }
            for(std::size_t j = i + 1; j < N; ++j) {
                if(ls[i] == ls[j]) { return false; }
            }
        }
        return true;
    }

    template<std::size_t N,
             std::size_t M>
    constexpr bool levelsDisjoint(std::array<int,
                                             N> const& a,
                                  std::array<int,
                                             M> const& b) {
        for(auto const x : a) {
            for(auto const y : b) {
                if(x == y) { return false; }
            }
        }
        return true;
    }

    // level -> position in the served list, 0xFF when not served
    template<std::size_t N>
    constexpr std::array<std::uint8_t,
                         MaxIsrLevel + 1>
    ringTable(std::array<int,
                         N> const& served) {
        std::array<std::uint8_t, MaxIsrLevel + 1> table{};
        for(auto& t : table) { t = 0xFF; }
        for(std::size_t i = 0; i < N; ++i) {
            table[static_cast<std::size_t>(served[i])] = static_cast<std::uint8_t>(i);
        }
        return table;
    }
}   // namespace detail

namespace IsrPolicy {

    template<typename Silent = SilentLevels<>>
    struct SingleLevel {
        static_assert(detail::isSilentLevels<Silent>,
                      "SingleLevel takes a SilentLevels<...>");
        static_assert(detail::levelsValid(detail::LevelArray<Silent>::value),
                      "SilentLevels: each level once, in 0..15");

        static constexpr std::size_t NumIsrRings = 1;

        // The Startup contract: every enabled interrupt outside the silent levels is at one level.
        static constexpr bool singleIsrPriorityLevel  = true;
        static constexpr auto silentIsrPriorityLevels = detail::LevelArray<Silent>::value;

        static constexpr std::size_t isrRing(int /*level*/) { return 0; }
    };

    // ActivePriorityFunction{}() returns the running exception's level (Kvasir::Nvic::
    // ActiveIsrPriority on a Cortex-M), negative for NMI and HardFault.
    template<typename ActivePriorityFunction, typename Served, typename Silent = SilentLevels<>>
    struct PerLevel {
        static_assert(detail::isLevels<Served>,
                      "PerLevel's second argument is a Levels<...>");
        static_assert(detail::isSilentLevels<Silent>,
                      "PerLevel's third argument is a SilentLevels<...>");
        static_assert(detail::LevelArray<Served>::value.size() > 0,
                      "PerLevel needs at least one served level");
        static_assert(detail::levelsValid(detail::LevelArray<Served>::value),
                      "Levels: each level once, in 0..15");
        static_assert(detail::levelsValid(detail::LevelArray<Silent>::value),
                      "SilentLevels: each level once, in 0..15");
        static_assert(
          detail::levelsDisjoint(detail::LevelArray<Served>::value,
                                 detail::LevelArray<Silent>::value),
          "a level cannot both have a ring (Levels) and be declared silent (SilentLevels)");

        static constexpr std::size_t NumIsrRings = detail::LevelArray<Served>::value.size();

        // The Startup contract: every enabled interrupt is at a served or a silent level.
        static constexpr auto isrPriorityLevels       = detail::LevelArray<Served>::value;
        static constexpr auto silentIsrPriorityLevels = detail::LevelArray<Silent>::value;

        static constexpr std::size_t isrRing(int level) {
            if(level < 0) { return 0; }
            if(level > detail::MaxIsrLevel) { return detail::noLogRing; }
            auto const ring = table[static_cast<std::size_t>(level)];
            return ring == 0xFF ? detail::noLogRing : ring;
        }

        static int activeLevel() { return ActivePriorityFunction{}(); }

    private:
        static constexpr auto table = detail::ringTable(isrPriorityLevels);
    };

    struct MaskedRecord {
        static constexpr std::size_t NumIsrRings = 1;

        using RecordGuard = detail::IsrRecordGuard;

        static constexpr std::size_t isrRing(int /*level*/) { return 0; }
    };

}   // namespace IsrPolicy
}   // namespace uc_log
