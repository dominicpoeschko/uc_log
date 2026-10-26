// uc_log::FanOutBackend: every record whole in every sink, the primary's lost-record line in the primary only, for
// every context (thread / interrupt / NMI-HardFault x two cores), also when a loss is pending in one context while
// another context's record preempts it. The UC_LOG macros in catalog mode, as on a target; the records are decoded
// with this binary's own catalog (the post-build step writes it).
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "support/Check.hpp"
#include "uc_log/AlwaysAttached.hpp"
#include "uc_log/ChannelRouter.hpp"
#include "uc_log/FanOutBackend.hpp"
#include "uc_log/RttLog.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using uc_log::Context;

struct FakeContexts {
    static constexpr std::size_t cores = 2;
    static inline Context        ctx{Context::thread};
    static inline std::size_t    coreId{0};

    static Context context() { return ctx; }

    static std::size_t core() { return coreId; }
};

constexpr std::size_t slots = FakeContexts::cores * uc_log::contextsPerCore;

std::size_t slotNow() {
    return FakeContexts::coreId * uc_log::contextsPerCore
         + static_cast<std::size_t>(FakeContexts::ctx);
}

// A sink that gathers each context's record on its own (as a staging backend does) and keeps the finished ones.
// Leaked on purpose (here and below): a namespace-scope object with a destructor is an exit-time destructor.
template<int Id>
struct Capture {
    struct State {
        std::array<std::vector<std::byte>, slots> open{};
        std::vector<std::vector<std::byte>>       records{};
        // called after every write(): lets a test preempt the record at that point
        std::function<void()> afterWrite{};
    };

    static State& state() {
        static auto& s = *new State{};
        return s;
    }

    static std::vector<std::vector<std::byte>> const& records() { return state().records; }

    static void initTransfer() { state().open[slotNow()].clear(); }

    static void write(std::span<std::byte const> data) {
        auto& o = state().open[slotNow()];
        o.insert(o.end(), data.begin(), data.end());
        if(state().afterWrite) { state().afterWrite(); }
    }

    static void finalizeTransfer() { state().records.push_back(state().open[slotNow()]); }

    static void reset() { state() = State{}; }
};

// The primary: a counting sink, as LossPolicy::Counted is - per context the records it lost and has not announced.
struct Primary : Capture<0> {
    struct Mark {
        std::uint32_t pending;
        std::size_t   slot;
    };

    static inline std::array<std::uint32_t, slots> pending{};
    static inline std::uint32_t                    settled{};

    static Mark lossMark() { return {pending[slotNow()], slotNow()}; }

    static void lossSettle(Mark const m) {
        pending[m.slot] -= m.pending;
        settled += m.pending;
    }
};

using Other = Capture<1>;

// A sink with a pre-memory hook, as the RTT backends have one (RttEpoch.hpp).
struct Epoch {
    static inline int bumps{0};

    [[maybe_unused]] static void preMemoryInit() { ++bumps; }

    [[maybe_unused]] static void postMemoryInit() { ++bumps; }

    // and a count of boots that logged
    [[maybe_unused]] static std::uint16_t bootsThatLogged() { return 42; }

    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

}   // namespace

namespace uc_log {
template<>
struct ComBackend<Tag::User> : FanOutBackendWith<FakeContexts, Primary, Other> {};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{7}; }
};
}   // namespace uc_log

namespace {

using Fan = uc_log::ComBackend<uc_log::Tag::User>;

std::unordered_map<std::uint16_t,
                   std::string> const&
catalog() {
    static auto const& strings = *new std::unordered_map<std::uint16_t, std::string>{[] {
        auto result = remote_fmt::readCatalogFile(UC_LOG_TEST_CATALOG_JSON);
        if(!result) {
            std::printf("FAIL: catalog: %s\n", result.error().c_str());
            ++uc_log::test::failures;
            return std::unordered_map<std::uint16_t, std::string>{};
        }
        return std::move(result->strings);
    }()};
    return strings;
}

// The message of a record, or "?" when it does not decode whole.
std::string text(std::vector<std::byte> const& record) {
    std::string error;
    auto const  parsed
      = remote_fmt::parseMessage(std::span<std::byte const>{record},
                                 catalog(),
                                 [&](std::string_view e) { error = std::string{e}; });
    if(!parsed.message || parsed.discarded != 0 || !parsed.remaining.empty()) {
        return "?" + error;
    }
    return uc_log::printer::LogEntry{0, *parsed.message}.logMsg;
}

std::vector<std::string> texts(std::vector<std::vector<std::byte>> const& records) {
    std::vector<std::string> out;
    out.reserve(records.size());
    for(auto const& r : records) { out.push_back(text(r)); }
    return out;
}

bool isLostLine(std::string const& t) { return t.contains("log records lost on this channel"); }

void resetAll() {
    Primary::reset();
    Other::reset();
    Primary::pending     = {};
    Primary::settled     = 0;
    Fan::primaryOnly     = {};
    FakeContexts::ctx    = Context::thread;
    FakeContexts::coreId = 0;
}

void everyRecordInEverySink() {
    resetAll();
    UC_LOG_I("one {}", 1);
    UC_LOG_W("two {} {}", 2, "x");
    CHECK(Primary::records() == Other::records(), "the same bytes in both sinks");
    auto const t = texts(Other::records());
    CHECK(t.size() == 2 && t[0] == "one 1" && t[1] == "two 2 x", "decoded with the one catalog");
}

// For each core and context: a pending loss is announced in the primary only, and the record after it reaches both.
void lostLineInThePrimaryOnly() {
    for(std::size_t core = 0; core != FakeContexts::cores; ++core) {
        for(auto const ctx : {Context::thread, Context::isr, Context::fault}) {
            resetAll();
            FakeContexts::coreId        = core;
            FakeContexts::ctx           = ctx;
            Primary::pending[slotNow()] = 3;
            UC_LOG_I("after the loss");
            auto const p = texts(Primary::records());
            auto const o = texts(Other::records());
            CHECK(p.size() == 2 && isLostLine(p[0]) && p[1] == "after the loss",
                  "primary: the lost line, the record");
            CHECK(o.size() == 1 && o[0] == "after the loss", "other: the record only");
            CHECK(Primary::pending[slotNow()] == 0 && Primary::settled == 3, "settled");
            CHECK(!Fan::primaryOnly[slotNow()], "the flag is cleared");
            UC_LOG_I("next");
            CHECK(Other::records().size() == 2, "the next record reaches the other sink again");
        }
    }
}

// A thread's lost line preempted after every write by an interrupt record (which has a loss of its own pending
// in half the cases): the interrupt's record reaches both sinks, each lost line only the primary.
void lossPendingAcrossAContextSwitch() {
    for(int at = 1; at <= 8; ++at) {
        for(int isrLoss = 0; isrLoss != 2; ++isrLoss) {
            for(std::size_t core = 0; core != FakeContexts::cores; ++core) {
                resetAll();
                FakeContexts::coreId        = core;
                Primary::pending[slotNow()] = 5;   // the thread's
                int  countdown              = at;
                bool fired                  = false;
                Primary::state().afterWrite = [&] {
                    // only while the thread writes its lost line
                    if(fired || FakeContexts::ctx != Context::thread
                       || !Fan::primaryOnly[slotNow()])
                    {
                        return;
                    }
                    if(--countdown != 0) { return; }
                    fired             = true;
                    FakeContexts::ctx = Context::isr;
                    if(isrLoss != 0) { Primary::pending[slotNow()] = 2; }
                    UC_LOG_I("isr record");
                    FakeContexts::ctx = Context::thread;
                };
                UC_LOG_I("thread record");
                auto const        p = texts(Primary::records());
                auto const        o = texts(Other::records());
                std::size_t const lostLines
                  = static_cast<std::size_t>(std::count_if(p.begin(), p.end(), isLostLine));
                CHECK(!fired || lostLines == 1U + static_cast<std::size_t>(isrLoss),
                      "each pending loss: one line");
                CHECK(fired || lostLines == 1, "not preempted: one line");
                CHECK(std::count_if(o.begin(), o.end(), isLostLine) == 0,
                      "no lost line in the other sink");
                CHECK(std::count(o.begin(), o.end(), std::string{"thread record"}) == 1,
                      "thread record in other");
                CHECK(!fired || std::count(o.begin(), o.end(), std::string{"isr record"}) == 1,
                      "the interrupt's record reaches the other sink");
                for(auto const& t : p) { CHECK(t[0] != '?', "every primary record decodes whole"); }
                for(auto const& t : o) { CHECK(t[0] != '?', "every other record decodes whole"); }
                for(std::size_t s = 0; s != slots; ++s) {
                    CHECK(!Fan::primaryOnly[s], "no flag left set");
                }
            }
        }
    }
}

// A sink without loss counting as primary: no lossMark, nothing routed specially (compile-time check).
struct NoCount {
    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

template<typename B>
concept HasLossMark = requires { B::lossMark(); };
template<typename B>
concept HasPreMemoryInit = requires { B::preMemoryInit(); };
static_assert(!HasPreMemoryInit<Fan>,
              "no sink has the hook: the fan-out has none either");
static_assert(HasPreMemoryInit<uc_log::FanOutBackendWith<FakeContexts,
                                                         Epoch,
                                                         Other>>,
              "primary's hook forwarded");
static_assert(HasPreMemoryInit<uc_log::FanOutBackendWith<FakeContexts,
                                                         Other,
                                                         Epoch>>,
              "another sink's hook forwarded");
// the real thing: a fan-out over an RTT backend bumps its ring epoch at boot
static_assert(HasPreMemoryInit<uc_log::FanOutBackendWith<FakeContexts,
                                                         uc_log::RttLog<uc_log::AlwaysAttached,
                                                                        uc_log::Rings<64>>,
                                                         Other>>,
              "a fan-out over an RTT backend has its pre-memory hook");

// uc_log::bootsThatLogged(): the primary's count only - a second counting sink's would be another number
template<typename B>
concept HasBootCount = requires { B::bootsThatLogged(); };
static_assert(!HasBootCount<Fan>,
              "no sink counts boots: the fan-out has no count");
static_assert(HasBootCount<uc_log::FanOutBackendWith<FakeContexts,
                                                     Epoch,
                                                     Other>>,
              "the primary's count");
static_assert(!HasBootCount<uc_log::FanOutBackendWith<FakeContexts,
                                                      Other,
                                                      Epoch>>,
              "another sink's count is not the fan-out's");
static_assert(HasBootCount<uc_log::FanOutBackendWith<FakeContexts,
                                                     uc_log::RttLog<uc_log::AlwaysAttached,
                                                                    uc_log::Rings<64>>,
                                                     Other>>,
              "a fan-out over an RTT backend has its boot count");

void preMemoryInitReachesEverySink() {
    Epoch::bumps = 0;
    uc_log::FanOutBackendWith<FakeContexts, Epoch, Other, Epoch>::preMemoryInit();
    CHECK(Epoch::bumps == 2, "every sink with the hook gets it once");
    uc_log::FanOutBackendWith<FakeContexts, Epoch, Other, Epoch>::postMemoryInit();
    CHECK(Epoch::bumps == 4, "and the post-memory hook too");
    CHECK((uc_log::FanOutBackendWith<FakeContexts, Epoch, Other>::bootsThatLogged() == 42),
          "the boot count is the primary's");
}

static_assert(!HasLossMark<uc_log::FanOutBackendWith<FakeContexts,
                                                     NoCount,
                                                     Other>>);
static_assert(HasLossMark<Fan>);

// The other core's thread (or its interrupt, or a fault) logs while this core's thread writes its lost line: the
// flags are per core too - the other core's record reaches both sinks.
void lossPendingWhileTheOtherCoreLogs() {
    for(auto const otherCtx : {Context::thread, Context::isr, Context::fault}) {
        for(int at = 1; at <= 4; ++at) {
            resetAll();
            FakeContexts::coreId        = 0;
            Primary::pending[slotNow()] = 4;
            int  countdown              = at;
            bool fired                  = false;
            Primary::state().afterWrite = [&] {
                if(fired || FakeContexts::coreId != 0 || !Fan::primaryOnly[slotNow()]) { return; }
                if(--countdown != 0) { return; }
                fired                = true;
                FakeContexts::coreId = 1;
                FakeContexts::ctx    = otherCtx;
                UC_LOG_I("core 1 record");
                FakeContexts::coreId = 0;
                FakeContexts::ctx    = Context::thread;
            };
            UC_LOG_I("core 0 record");
            auto const o = texts(Other::records());
            // the lost line is written in a few pieces: `at` past the last one does not fire
            CHECK(at != 1 || fired, "the other core logged inside the lost line");
            CHECK(!fired || std::count(o.begin(), o.end(), std::string{"core 1 record"}) == 1,
                  "the other core's record reaches the other sink");
            CHECK(std::count_if(o.begin(), o.end(), isLostLine) == 0,
                  "no lost line in the other sink");
        }
    }
}

// A sink that takes the record's level, as an RTT backend that routes levels to channels does, or a queue that only
// takes warn and up: what it saw per level, and a pending loss per level (one ring per level).
using uc_log::LogLevel;
constexpr std::size_t levels = static_cast<std::size_t>(LogLevel::crit) + 1;

struct LevelSink {
    static inline std::array<int, levels>           inits{};
    static inline std::array<int, levels>           writes{};
    static inline std::array<int, levels>           finals{};
    static inline std::array<std::uint32_t, levels> pending{};

    struct Mark {
        std::uint32_t pending;
        LogLevel      level;
    };

    template<LogLevel L>
    static void initTransfer() {
        ++inits[static_cast<std::size_t>(L)];
    }

    template<LogLevel L>
    static void write(std::span<std::byte const>) {
        ++writes[static_cast<std::size_t>(L)];
    }

    template<LogLevel L>
    static void finalizeTransfer() {
        ++finals[static_cast<std::size_t>(L)];
    }

    template<LogLevel L>
    static Mark lossMark() {
        return {pending[static_cast<std::size_t>(L)], L};
    }

    static void lossSettle(Mark const m) { pending[static_cast<std::size_t>(m.level)] = 0; }

    static void reset() {
        inits   = {};
        writes  = {};
        finals  = {};
        pending = {};
    }
};

using LevelFan = uc_log::FanOutBackendWith<FakeContexts, LevelSink, Other>;

static_assert(!Fan::levelAware && !uc_log::detail::LevelAware<Fan>,
              "level-blind sinks: one record path for every level");
static_assert(LevelFan::levelAware && uc_log::detail::LevelAware<LevelFan>,
              "a sink that takes the level makes the fan-out take it");
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        Other,
                                        LevelSink>::levelAware,
              "also when it is not the primary");

template<typename B>
concept HasLevelLossMark = requires { B::template lossMark<LogLevel::warn>(); };
static_assert(HasLevelLossMark<LevelFan> && !HasLossMark<LevelFan>,
              "the primary's per-level lossMark, per level");
static_assert(!HasLevelLossMark<uc_log::FanOutBackendWith<FakeContexts,
                                                          Other,
                                                          LevelSink>>,
              "another sink's losses are not the fan-out's");

// The record's level reaches the sink that takes it; the level-blind sink beside it gets every record whole.
void levelReachesTheSinkThatTakesIt() {
    resetAll();
    LevelSink::reset();
    std::array<std::byte, 3> const bytes{};
    LevelFan::initTransfer<LogLevel::info>();
    LevelFan::write<LogLevel::info>(bytes);
    LevelFan::write<LogLevel::info>(bytes);
    LevelFan::finalizeTransfer<LogLevel::info>();
    LevelFan::initTransfer<LogLevel::error>();
    LevelFan::write<LogLevel::error>(bytes);
    LevelFan::finalizeTransfer<LogLevel::error>();
    auto const at = [](auto const& a, LogLevel l) { return a[static_cast<std::size_t>(l)]; };
    CHECK(at(LevelSink::inits, LogLevel::info) == 1 && at(LevelSink::writes, LogLevel::info) == 2
            && at(LevelSink::finals, LogLevel::info) == 1,
          "info: one record of two writes, at info");
    CHECK(at(LevelSink::inits, LogLevel::error) == 1 && at(LevelSink::writes, LogLevel::error) == 1
            && at(LevelSink::finals, LogLevel::error) == 1,
          "error: one record, at error");
    CHECK(at(LevelSink::writes, LogLevel::warn) == 0, "nothing at another level");
    CHECK(Other::records().size() == 2 && Other::records()[0].size() == 6
            && Other::records()[1].size() == 3,
          "the level-blind sink: both records, whole");
}

// A pending loss on the primary's warn ring: that record goes to the primary alone until it is settled, and a mark
// at another level (another ring) routes nothing.
void levelLossMarkRoutesThePrimaryOnly() {
    resetAll();
    LevelSink::reset();
    LevelSink::pending[static_cast<std::size_t>(LogLevel::warn)] = 2;
    std::array<std::byte, 4> const bytes{};
    auto const                     quiet = LevelFan::lossMark<LogLevel::info>();
    CHECK(quiet.pending == 0 && !Fan::primaryOnly[slotNow()], "info ring: nothing pending");
    LevelFan::lossSettle(quiet);
    auto const mark = LevelFan::lossMark<LogLevel::warn>();
    CHECK(mark.pending == 2 && LevelFan::primaryOnly[slotNow()], "warn ring: the lost line");
    LevelFan::initTransfer<LogLevel::warn>();
    LevelFan::write<LogLevel::warn>(bytes);
    LevelFan::finalizeTransfer<LogLevel::warn>();
    CHECK(Other::records().empty(), "the lost line stays in the primary");
    LevelFan::lossSettle(mark);
    CHECK(!LevelFan::primaryOnly[slotNow()]
            && LevelSink::pending[static_cast<std::size_t>(LogLevel::warn)] == 0,
          "settled");
    LevelFan::initTransfer<LogLevel::warn>();
    LevelFan::write<LogLevel::warn>(bytes);
    LevelFan::finalizeTransfer<LogLevel::warn>();
    CHECK(Other::records().size() == 1, "the record after it reaches the other sink");
}

// Startup's ISR contract (IsrPolicy.hpp): the one sink that declares it hands it on.
struct SingleContract {
    static constexpr bool               singleIsrPriorityLevel = true;
    static constexpr std::array<int, 1> silentIsrPriorityLevels{0};

    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

struct LevelsContract {
    static constexpr std::array<int, 2> isrPriorityLevels{1, 3};
    static constexpr std::array<int, 0> silentIsrPriorityLevels{};

    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

template<typename B>
concept HasSingleContract = requires { B::singleIsrPriorityLevel; };
template<typename B>
concept HasLevelsContract = requires { B::isrPriorityLevels; };

static_assert(!HasSingleContract<Fan> && !HasLevelsContract<Fan>,
              "no sink declares one: none");
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        NoCount,
                                        SingleContract>::singleIsrPriorityLevel
                && uc_log::FanOutBackendWith<FakeContexts,
                                             NoCount,
                                             SingleContract>::silentIsrPriorityLevels[0]
                     == 0,
              "single level, from another sink");
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        LevelsContract,
                                        Other>::isrPriorityLevels[1]
                  == 3
                && !HasSingleContract<uc_log::FanOutBackendWith<FakeContexts,
                                                                LevelsContract,
                                                                Other>>,
              "served levels, from the primary");

// the real things: the RTT backends' contracts and levels through a fan-out
using RouterRtt = uc_log::RttLog<
  uc_log::AlwaysAttached,
  uc_log::RingLayout<uc_log::SingleCore,
                     uc_log::LevelSplitRouter<LogLevel::warn, LogLevel::error, LogLevel::crit>>,
  uc_log::Rings<64>>;
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        RouterRtt,
                                        Other>::levelAware,
              "a level-routing RTT backend: the fan-out takes the level");
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        RouterRtt,
                                        Other>::singleIsrPriorityLevel,
              "and hands on its SingleLevel contract");
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        uc_log::RttLog<uc_log::AlwaysAttached,
                                                       uc_log::Rings<64>>,
                                        Other>::singleIsrPriorityLevel
                && !uc_log::FanOutBackendWith<FakeContexts,
                                              uc_log::RttLog<uc_log::AlwaysAttached,
                                                             uc_log::Rings<64>>,
                                              Other>::levelAware,
              "a level-blind RTT backend: its contract, no level");

// perCoreIsrContexts: handed on only when the fan-out's own flags are per core (Contexts::cores > 1)
struct CoreZero {
    [[maybe_unused]] constexpr std::uint32_t operator()() const { return 0; }
};

using MulticoreRtt = uc_log::
  RttLog<uc_log::AlwaysAttached, uc_log::RingLayout<uc_log::TwoCores<CoreZero>>, uc_log::Rings<64>>;
template<typename B>
concept HasPerCore = requires { B::perCoreIsrContexts; };
static_assert(uc_log::FanOutBackendWith<FakeContexts,
                                        MulticoreRtt,
                                        Other>::perCoreIsrContexts
                && uc_log::FanOutBackendWith<FakeContexts,
                                             MulticoreRtt,
                                             Other>::singleIsrPriorityLevel,
              "a multicore RTT primary behind per-core contexts: its contract held per core");
static_assert(!HasPerCore<uc_log::FanOutBackendWith<FakeContexts,
                                                    uc_log::RttLog<uc_log::AlwaysAttached,
                                                                   uc_log::Rings<64>>,
                                                    Other>>,
              "a single-core backend declares none, the fan-out hands on none");

// Duplex channels: the primary's
using DuplexRtt = uc_log::RttLog<uc_log::AlwaysAttached,
                                 uc_log::Rings<64>,
                                 uc_log::DuplexChannels<uc_log::DuplexChannelConfig<"shell", 32>>>;
using DuplexFan = uc_log::FanOutBackendWith<FakeContexts, DuplexRtt, Other>;
static_assert(DuplexFan::numDuplexChannels == 1 && DuplexFan::DuplexChannel<0>::name == "shell",
              "the primary's duplex channels, through the fan-out");
template<typename B>
concept HasDuplex = requires { B::numDuplexChannels; };
static_assert(!HasDuplex<Fan>,
              "a primary without duplex channels: none");

// kLevelFloor: handed on when every level-aware sink declares the same one
template<LogLevel Floor>
struct FloorSink {
    static constexpr LogLevel kLevelFloor = Floor;

    template<LogLevel L>
    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

template<typename B>
concept HasFloor = requires { B::kLevelFloor; };
using FloorFan   = uc_log::
  FanOutBackendWith<FakeContexts, FloorSink<LogLevel::warn>, Other, FloorSink<LogLevel::warn>>;
static_assert(FloorFan::kLevelFloor == LogLevel::warn
                && uc_log::detail::canonicalLevel<FloorFan,
                                                  LogLevel::error>()
                     == LogLevel::warn,
              "two equal floors: one, and error resolves to it");
static_assert(!HasFloor<uc_log::FanOutBackendWith<FakeContexts,
                                                  FloorSink<LogLevel::warn>,
                                                  FloorSink<LogLevel::error>>>,
              "two different floors: none");
static_assert(!HasFloor<uc_log::FanOutBackendWith<FakeContexts,
                                                  FloorSink<LogLevel::warn>,
                                                  RouterRtt>>,
              "a level-aware sink without one (a router): none");
static_assert(!HasFloor<Fan>,
              "level-blind sinks: none");

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(
      argc,
      argv,
      {
        {           "everyRecordInEverySink",            everyRecordInEverySink},
        {         "lostLineInThePrimaryOnly",          lostLineInThePrimaryOnly},
        {  "lossPendingAcrossAContextSwitch",   lossPendingAcrossAContextSwitch},
        { "lossPendingWhileTheOtherCoreLogs",  lossPendingWhileTheOtherCoreLogs},
        {    "preMemoryInitReachesEverySink",     preMemoryInitReachesEverySink},
        {   "levelReachesTheSinkThatTakesIt",    levelReachesTheSinkThatTakesIt},
        {"levelLossMarkRoutesThePrimaryOnly", levelLossMarkRoutesThePrimaryOnly},
    });
}
