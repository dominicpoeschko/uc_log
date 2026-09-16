// The IsrPolicy shapes (uc_log/IsrPolicy.hpp) on the host: ring selection per level, the
// contract members Kvasir's Startup reads, and which policy carries a RecordGuard. Compile time
// only; main() exists so ctest has something to run. The rejections are in isr_policy_fail.cpp.
#include "uc_log/IsrPolicy.hpp"
#include "uc_log/detail/LevelBoundBackend.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <type_traits>

namespace {

namespace IP = uc_log::IsrPolicy;
using uc_log::Levels;
using uc_log::SilentLevels;
using uc_log::detail::noLogRing;

struct FakeActive {
    [[maybe_unused]] static int operator()() { return 3; }
};

// requires-expressions on a concrete type are hard errors, not false: go through a template.
template<typename T>
constexpr bool hasSingle = requires { T::singleIsrPriorityLevel; };
template<typename T>
constexpr bool hasLevels = requires { T::isrPriorityLevels; };
template<typename T>
constexpr bool hasActive = requires { T::activeLevel(); };
template<typename T>
constexpr bool hasGuard = requires { typename T::RecordGuard; };

// ---- PerLevel: one ring per served level, in the order listed ----------------------------
using Three = IP::PerLevel<FakeActive, Levels<0, 3, 7>, SilentLevels<1, 2>>;
static_assert(Three::NumIsrRings == 3);
static_assert(Three::isrRing(0) == 0);
static_assert(Three::isrRing(3) == 1);
static_assert(Three::isrRing(7) == 2);
static_assert(Three::isrRing(5) == noLogRing,
              "an unserved level writes nothing");
static_assert(Three::isrRing(1) == noLogRing,
              "a silent level that logs anyway writes nothing");
static_assert(Three::isrRing(15) == noLogRing);
static_assert(Three::isrRing(16) == noLogRing,
              "beyond the last level");
static_assert(Three::isrRing(-1) == 0,
              "HardFault: the first ISR ring");
static_assert(Three::isrRing(-2) == 0,
              "NMI: the first ISR ring");
static_assert(Three::isrPriorityLevels
              == std::array<int,
                            3>{0,
                               3,
                               7});
static_assert(hasLevels<Three> && hasActive<Three>);
static_assert(Three::silentIsrPriorityLevels
              == std::array<int,
                            2>{1,
                               2});
static_assert(!hasSingle<Three>);
static_assert(!hasGuard<Three>);

// the order of the listing is the ring order, not the numeric order
using Reversed = IP::PerLevel<FakeActive, Levels<7, 3>>;
static_assert(Reversed::isrRing(7) == 0 && Reversed::isrRing(3) == 1);
static_assert(Reversed::silentIsrPriorityLevels.size() == 0);

// ---- SingleLevel: one ring, whatever the level -------------------------------------------
using Single = IP::SingleLevel<>;
static_assert(Single::NumIsrRings == 1);
static_assert(Single::isrRing(0) == 0 && Single::isrRing(9) == 0 && Single::isrRing(-1) == 0);
static_assert(Single::singleIsrPriorityLevel);
static_assert(Single::silentIsrPriorityLevels.size() == 0);
static_assert(!hasLevels<Single>);
static_assert(!hasActive<Single>,
              "no priority read on the single-level path");
static_assert(!hasGuard<Single>);

using SingleSilent = IP::SingleLevel<SilentLevels<0>>;
static_assert(SingleSilent::silentIsrPriorityLevels
              == std::array<int,
                            1>{0});

// ---- MaskedRecord: one ring, the guard, nothing for Startup -------------------------------
using Masked = IP::MaskedRecord;
static_assert(Masked::NumIsrRings == 1);
static_assert(Masked::isrRing(4) == 0);
static_assert(hasGuard<Masked>);
static_assert(std::is_same_v<Masked::RecordGuard,
                             uc_log::detail::IsrRecordGuard>);
static_assert(!hasSingle<Masked>);
static_assert(!hasLevels<Masked>);
static_assert(!hasActive<Masked>);

// ---- LevelBoundBackend forwards a guard, or supplies an empty one -----------------------
struct WithGuard : Masked {
    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

struct Without {
    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

using BoundWith    = uc_log::detail::LevelBoundBackend<WithGuard, uc_log::LogLevel::info>;
using BoundWithout = uc_log::detail::LevelBoundBackend<Without, uc_log::LogLevel::info>;
static_assert(std::is_same_v<BoundWith::RecordGuard,
                             uc_log::detail::IsrRecordGuard>);
static_assert(std::is_empty_v<BoundWithout::RecordGuard>);

}   // namespace

int main() {
    std::printf("all checks passed\n");
    return 0;
}
