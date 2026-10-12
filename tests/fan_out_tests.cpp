// uc_log::FanOutBackend: every record whole in every sink, the primary's lost-record line in the primary only, for
// every context (thread / interrupt / NMI-HardFault x two cores), also when a loss is pending in one context while
// another context's record preempts it. The UC_LOG macros in catalog mode, as on a target; the records are decoded
// with this binary's own catalog (the post-build step writes it).
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/AlwaysAttached.hpp"
#include "uc_log/DefaultRttComBackend.hpp"
#include "uc_log/FanOutBackend.hpp"
#include "uc_log/detail/LogEntry.hpp"
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

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

using uc_log::fanout::Context;

struct FakeContexts {
    static constexpr std::size_t Cores = 2;
    static inline Context        ctx{Context::thread};
    static inline std::size_t    coreId{0};

    static Context context() { return ctx; }

    static std::size_t core() { return coreId; }
};

constexpr std::size_t Slots = FakeContexts::Cores * uc_log::fanout::ContextsPerCore;

std::size_t slotNow() {
    return FakeContexts::coreId * uc_log::fanout::ContextsPerCore
         + static_cast<std::size_t>(FakeContexts::ctx);
}

// A sink that gathers each context's record on its own (as a staging backend does) and keeps the finished ones.
// Leaked on purpose (here and below): a namespace-scope object with a destructor is an exit-time destructor.
template<int Id>
struct Capture {
    struct State {
        std::array<std::vector<std::byte>, Slots> open{};
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

    static inline std::array<std::uint32_t, Slots> pending{};
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
        auto result = remote_fmt::parseStringConstantsFromJsonFile(UC_LOG_TEST_CATALOG_JSON);
        if(!result) {
            std::printf("FAIL: catalog: %s\n", result.error().c_str());
            ++failures;
            return std::unordered_map<std::uint16_t, std::string>{};
        }
        return *result;
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
    return uc_log::detail::LogEntry{0, *parsed.message}.logMsg;
}

std::vector<std::string> texts(std::vector<std::vector<std::byte>> const& records) {
    std::vector<std::string> out;
    for(auto const& r : records) { out.push_back(text(r)); }
    return out;
}

bool isLostLine(std::string const& t) {
    return t.find("log records lost on this channel") != std::string::npos;
}

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
    for(std::size_t core = 0; core != FakeContexts::Cores; ++core) {
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
            for(std::size_t core = 0; core != FakeContexts::Cores; ++core) {
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
                for(std::size_t s = 0; s != Slots; ++s) {
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
static_assert(
  HasPreMemoryInit<uc_log::FanOutBackendWith<FakeContexts,
                                             uc_log::DefaultRttComBackend<uc_log::AlwaysAttached,
                                                                          64>,
                                             Other>>,
  "a fan-out over an RTT backend has its pre-memory hook");

void preMemoryInitReachesEverySink() {
    Epoch::bumps = 0;
    uc_log::FanOutBackendWith<FakeContexts, Epoch, Other, Epoch>::preMemoryInit();
    CHECK(Epoch::bumps == 2, "every sink with the hook gets it once");
    uc_log::FanOutBackendWith<FakeContexts, Epoch, Other, Epoch>::postMemoryInit();
    CHECK(Epoch::bumps == 4, "and the post-memory hook too");
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

}   // namespace

int main() {
    everyRecordInEverySink();
    lostLineInThePrimaryOnly();
    lossPendingAcrossAContextSwitch();
    lossPendingWhileTheOtherCoreLogs();
    preMemoryInitReachesEverySink();
    if(failures != 0) {
        std::printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("fan-out tests passed\n");
    return 0;
}
