// Which RTT ring a log write goes to, for every ring layout (RttComBackend.hpp) and policy (IsrPolicy.hpp): thread or
// exception, served or unserved priority level, NMI/HardFault, core 0 or 1, the level's logical channel. Only the ring
// choice is called (no write: a backend's control block is target storage), with a fake Contexts that also says the
// core - the one type a layout asks, as SingleCore / TwoCores<CoreId> do on the target.
#include "support/Check.hpp"
#include "uc_log/AlwaysAttached.hpp"
#include "uc_log/ChannelRouter.hpp"
#include "uc_log/RttLog.hpp"
#include "uc_log/Sink.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using uc_log::LogLevel;
using uc_log::detail::noLogRing;

struct Where {
    static inline bool thread{true};
    static inline int  level{0};   // the running exception's priority level; < 0 NMI/HardFault
    static inline std::size_t core{0};
};

template<std::size_t N>
struct Fake {
    static constexpr std::size_t cores = N;

    static bool inThread() { return Where::thread; }

    static std::size_t core() { return Where::core; }
};

struct FakePriority {
    int operator()() const { return Where::level; }
};

void at(bool        thread,
        int         level,
        std::size_t core) {
    Where::thread = thread;
    Where::level  = level;
    Where::core   = core;
}

using PerLevel = uc_log::IsrPolicy::PerLevel<FakePriority, uc_log::Levels<1, 3>>;
using Split    = uc_log::LevelSplitRouter<LogLevel::warn, LogLevel::error, LogLevel::crit>;

template<std::size_t Cores, typename Router, typename Policy, std::size_t Main = 16>
using Backend = uc_log::RttLog<uc_log::AlwaysAttached,
                               uc_log::RingLayout<Fake<Cores>, Router>,
                               uc_log::Rings<Main, 8>,
                               Policy>;

using One         = Backend<1, uc_log::SingleChannelRouter, uc_log::IsrPolicy::SingleLevel<>>;
using OneLvl      = Backend<1, uc_log::SingleChannelRouter, PerLevel>;
using Masked      = Backend<1, uc_log::SingleChannelRouter, uc_log::IsrPolicy::MaskedRecord>;
using Two         = Backend<2, uc_log::SingleChannelRouter, uc_log::IsrPolicy::SingleLevel<>>;
using TwoLvl      = Backend<2, uc_log::SingleChannelRouter, PerLevel>;
using OneSplit    = Backend<1, Split, uc_log::IsrPolicy::SingleLevel<>>;
using TwoSplit    = Backend<2, Split, uc_log::IsrPolicy::SingleLevel<>>;
using TwoSplitLvl = Backend<2, Split, PerLevel>;

template<typename B,
         LogLevel L>
std::size_t splitRing() {
    return B::template RingFor<L>::ring();
}

void oneGroup() {
    at(true, 0, 0);
    CHECK(One::ring() == 0, "one group: thread ring 0");
    at(false, 5, 0);
    CHECK(One::ring() == 1, "one group: any ISR ring 1");
    at(false, -1, 0);
    CHECK(One::ring() == 1, "one group: NMI on the ISR ring");
    at(false, 2, 0);
    CHECK(Masked::ring() == 1, "MaskedRecord: ISR ring 1");
}

void perLevel() {
    at(true, 3, 0);
    CHECK(OneLvl::ring() == 0, "PerLevel: thread ring 0");
    at(false, 1, 0);
    CHECK(OneLvl::ring() == 1, "PerLevel: level 1 -> ring 1");
    at(false, 3, 0);
    CHECK(OneLvl::ring() == 2, "PerLevel: level 3 -> ring 2");
    at(false, 2, 0);
    CHECK(OneLvl::ring() == noLogRing, "PerLevel: an unserved level has no ring");
    at(false, -1, 0);
    CHECK(OneLvl::ring() == 1, "PerLevel: NMI/HardFault on the first ISR ring");
}

void twoCores() {
    at(true, 0, 0);
    CHECK(Two::ring() == 0, "two cores: core 0 thread 0");
    at(false, 4, 0);
    CHECK(Two::ring() == 1, "two cores: core 0 ISR 1");
    at(true, 0, 1);
    CHECK(Two::ring() == 2, "two cores: core 1 thread 2");
    at(false, 4, 1);
    CHECK(Two::ring() == 3, "two cores: core 1 ISR 3");
    at(false, 3, 1);
    CHECK(TwoLvl::ring() == 5, "two cores PerLevel: core 1 level 3 -> 3 + 2");
    at(false, 2, 1);
    CHECK(TwoLvl::ring() == noLogRing, "two cores PerLevel: unserved");
}

void levelSplit() {
    at(true, 0, 0);
    CHECK((splitRing<OneSplit, LogLevel::info>() == 0), "split: info thread 0");
    CHECK((splitRing<OneSplit, LogLevel::warn>() == 2), "split: warn thread 2");
    at(false, 1, 0);
    CHECK((splitRing<OneSplit, LogLevel::debug>() == 1), "split: debug ISR 1");
    CHECK((splitRing<OneSplit, LogLevel::crit>() == 3), "split: crit ISR 3");
}

void twoCoresSplit() {
    at(true, 0, 0);
    CHECK((splitRing<TwoSplit, LogLevel::info>() == 0), "cores x split: core 0 info thread 0");
    CHECK((splitRing<TwoSplit, LogLevel::error>() == 2), "cores x split: core 0 error thread 2");
    at(true, 0, 1);
    CHECK((splitRing<TwoSplit, LogLevel::info>() == 4), "cores x split: core 1 info thread 4");
    CHECK((splitRing<TwoSplit, LogLevel::warn>() == 6), "cores x split: core 1 warn thread 6");
    at(false, 1, 1);
    CHECK((splitRing<TwoSplit, LogLevel::info>() == 5), "cores x split: core 1 info ISR 5");
    CHECK((splitRing<TwoSplit, LogLevel::warn>() == 7), "cores x split: core 1 warn ISR 7");
    at(false, 3, 1);
    CHECK((splitRing<TwoSplitLvl, LogLevel::warn>() == 3 * 3 + 2),
          "cores x split PerLevel: core 1 warn level 3");
}

template<typename B>
concept PerCore = requires { B::perCoreIsrContexts; };
static_assert(!PerCore<One> && Two::perCoreIsrContexts && TwoSplit::perCoreIsrContexts);
static_assert(uc_log::detail::LevelAware<OneSplit> && uc_log::detail::LevelAware<TwoSplit>
              && !uc_log::detail::LevelAware<Two>);

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {     "oneGroup",      oneGroup},
                               {     "perLevel",      perLevel},
                               {     "twoCores",      twoCores},
                               {   "levelSplit",    levelSplit},
                               {"twoCoresSplit", twoCoresSplit}
    });
}
