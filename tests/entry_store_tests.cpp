// Correctness harness for GuiEntryStore: A/B of the compiled filter against the old
// string-based semantics, async refilter under concurrent ingest, clear/trim invariants,
// and a store-mutex latency probe.
//
// Build (from scratchpad):
#include "uc_log/detail/GuiEntryStore.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <random>
#include <thread>
#include <vector>

using namespace uc_log::FTXUIGui;
using uc_log::LogLevel;
using uc_log::detail::LogEntry;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

// ---- reference model: the old per-entry string-based semantics -------------------------

struct RefGroup {
    std::size_t channel;
    LogLevel    level;
    std::string file;
    std::size_t line;
    double      ucSec;
    std::size_t lineCount;
};

struct RefFilter {
    FilterState state;
    bool        ucEnabled{false};
    double      minSec{0.0};
    double      maxSec{std::numeric_limits<double>::infinity()};
    // reference for the search: multiline groups carry "multi\nline\nmessage",
    // single-line ones "message" (or "live" from the producer)
    int searchMode{0};   // 0 none, 1 needle "multi", 2 regex matching multiline only
};

static bool refPasses(RefGroup const&  g,
                      RefFilter const& f) {
    if(f.searchMode != 0 && g.lineCount != 3) { return false; }
    if(!f.state.enabledLogLevels.empty() && !f.state.enabledLogLevels.contains(g.level)) {
        return false;
    }
    if(!f.state.enabledChannels.empty() && !f.state.enabledChannels.contains(g.channel)) {
        return false;
    }
    if(f.ucEnabled && (g.ucSec < f.minSec || g.ucSec > f.maxSec)) { return false; }

    SourceLocation const lineLoc{g.file, g.line};
    SourceLocation const fileLoc{g.file, 0};
    auto const&          inc = f.state.includedLocations;
    auto const&          exc = f.state.excludedLocations;

    if(!exc.empty() && exc.contains(lineLoc)) { return false; }
    if(!inc.empty() && inc.contains(lineLoc)) { return true; }
    if(!exc.empty() && exc.contains(fileLoc)) { return false; }
    if(!exc.empty()) { return true; }
    if(!inc.empty()) { return inc.contains(fileLoc); }
    return true;
}

// ---- helpers ---------------------------------------------------------------------------

static LogEntry makeEntry(std::size_t        channel,
                          LogLevel           level,
                          std::string const& file,
                          std::size_t        line,
                          double             ucSec,
                          std::string const& msg,
                          std::string const& function = "fn") {
    LogEntry e{channel, {}};
    e.logLevel = level;
    e.fileName = file;
    e.line     = line;
    e.ucTime.time
      = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>{ucSec});
    e.logMsg       = msg;
    e.functionName = function;
    e.parsedOk     = true;
    return e;
}

static void quiesce(GuiEntryStore& store) {
    for(;;) {
        store.pollRefilterDeadline();
        {
            std::lock_guard<std::mutex> const lock{store.mutex};
            if(!store.refilterInProgress.load() && !store.refilterDeadline
               && store.refilterGeneration.load() == store.displayedGeneration)
            {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

// walk the filtered list and validate group structure + membership against the reference
static void validateFiltered(GuiEntryStore&               store,
                             std::vector<RefGroup> const& groups,
                             RefFilter const&             filter,
                             char const*                  what) {
    std::size_t expectedGroups = 0;
    std::size_t expectedLines  = 0;
    for(auto const& g : groups) {
        if(refPasses(g, filter)) {
            ++expectedGroups;
            expectedLines += g.lineCount;
        }
    }

    std::lock_guard<std::mutex> const lock{store.mutex};
    if(store.filteredEntries.size() != expectedLines
       || store.filteredOriginalLogCount != expectedGroups)
    {
        std::printf("FAIL: %s: got %zu lines/%zu groups, expected %zu/%zu\n",
                    what,
                    store.filteredEntries.size(),
                    store.filteredOriginalLogCount,
                    expectedLines,
                    expectedGroups);
        ++failures;
        return;
    }
    // every group start in the filtered list must pass the reference filter
    // (groups are identified by multilineGroupId == 1-based index into `groups`)
    for(std::size_t i = 0; i < store.filteredEntries.size(); ++i) {
        auto const& e = store.filteredEntries[i];
        if(!e.startsGroup()) { continue; }
        auto const  gid = e.multilineGroupId();
        auto const& g   = groups[gid - 1];
        if(!refPasses(g, filter)) {
            std::printf("FAIL: %s: group %zu should not pass\n", what, gid);
            ++failures;
            return;
        }
    }
}

int main(int    argc,
         char** argv) {
    std::size_t total = 100'000;
    if(argc > 1) { total = std::strtoull(*std::next(argv, 1), nullptr, 10); }

    static constexpr std::array files{"main.cpp", "foo.hpp", "bar.c", "baz.hpp", "qux.cpp"};

    GuiEntryStore            store;
    std::atomic<std::size_t> redraws{0};
    store.requestRedraw = [&redraws]() { ++redraws; };

    std::vector<RefGroup> groups;
    groups.reserve(total);

    std::mt19937 gen{42};
    auto         randomGroup = [&](std::size_t index) {
        RefGroup g;
        g.channel   = std::uniform_int_distribution<std::size_t>{0, 5}(gen);
        g.level     = static_cast<LogLevel>(std::uniform_int_distribution<int>{0, 5}(gen));
        g.file      = files[std::uniform_int_distribution<std::size_t>{0, files.size() - 1}(gen)];
        g.line      = 10 + (index % 50) * 10;
        g.ucSec     = static_cast<double>(index) * 0.001;
        g.lineCount = (index % 100 == 0) ? 3 : 1;
        return g;
    };

    // ---- ingest ------------------------------------------------------------------------
    auto const t0 = std::chrono::steady_clock::now();
    for(std::size_t i = 0; i < total; ++i) {
        auto const g = randomGroup(i);
        groups.push_back(g);
        auto const msg
          = g.lineCount == 1 ? std::string{"message"} : std::string{"multi\nline\nmessage"};
        store.addEntry(std::chrono::system_clock::now(),
                       makeEntry(g.channel, g.level, g.file, g.line, g.ucSec, msg));
    }
    auto const ingestMs
      = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
    std::printf("-- ingested %zu groups in %lld ms (%zu redraw requests)\n",
                total,
                static_cast<long long>(ingestMs),
                redraws.load());

    // ---- A/B filter scenarios ------------------------------------------------------------
    auto applyAndCheck = [&](RefFilter const& f, char const* what) {
        store.setFilterState(f.state);
        store.setSearchText(f.searchMode == 0   ? ""
                            : f.searchMode == 1 ? "MULTI"            // case-insensitive
                                                : "re:mu.ti\\nl");   // spans the newline
        store.updateUcTime([&f](GuiEntryStore& s) {
            s.ucTimeFilterEnabled = f.ucEnabled;
            s.ucTimeLiveMode      = false;
            s.minUcTimeSec        = f.minSec;
            s.maxUcTimeSec        = f.maxSec;
            s.requestRefilterLocked();
        });
        quiesce(store);
        validateFiltered(store, groups, f, what);
    };

    {
        RefFilter f;
        applyAndCheck(f, "no filter");
    }
    {
        RefFilter f;
        f.state.enabledLogLevels = {LogLevel::error, LogLevel::crit};
        applyAndCheck(f, "level subset");
    }
    {
        RefFilter f;
        f.state.enabledChannels = {1, 3};
        applyAndCheck(f, "channel subset");
    }
    {
        RefFilter f;
        f.state.includedLocations = {
          SourceLocation{"main.cpp",   0},
          SourceLocation{ "foo.hpp", 110}
        };
        f.state.excludedLocations = {
          SourceLocation{"main.cpp", 210}
        };
        applyAndCheck(f, "include file + exclude line");
    }
    {
        RefFilter f;
        f.state.excludedLocations = {
          SourceLocation{"bar.c", 0}
        };
        applyAndCheck(f, "exclude file");
    }
    {
        RefFilter f;
        f.ucEnabled = true;
        f.minSec    = 50.0;
        f.maxSec    = 200.0;
        applyAndCheck(f, "uc-time window");
    }
    {
        RefFilter f;
        f.state.enabledLogLevels  = {LogLevel::info, LogLevel::warn, LogLevel::error};
        f.state.enabledChannels   = {0, 2, 4};
        f.state.excludedLocations = {
          SourceLocation{"baz.hpp", 0}
        };
        f.ucEnabled = true;
        f.minSec    = 10.0;
        applyAndCheck(f, "combined");
    }
    {
        RefFilter f;
        f.searchMode = 1;
        applyAndCheck(f, "case-insensitive substring search");
    }
    {
        RefFilter f;
        f.searchMode             = 2;
        f.state.enabledLogLevels = {LogLevel::error, LogLevel::crit};
        applyAndCheck(f, "regex search across lines + level");
    }
    {
        RefFilter f;   // search cleared again
        applyAndCheck(f, "search cleared");
    }

    // ---- concurrent producer during refilter --------------------------------------------
    {
        std::atomic<bool>        produce{true};
        std::atomic<std::size_t> produced{0};
        std::mutex               groupsMutex;
        std::thread              producer{[&]() {
            std::mt19937 pgen{7};
            std::size_t  index = total;
            while(produce.load()) {
                RefGroup g;
                g.channel   = std::uniform_int_distribution<std::size_t>{0, 5}(pgen);
                g.level     = static_cast<LogLevel>(std::uniform_int_distribution<int>{0, 5}(pgen));
                g.file      = files[index % files.size()];
                g.line      = 10 + (index % 50) * 10;
                g.ucSec     = static_cast<double>(index) * 0.001;
                g.lineCount = 1;
                {
                    std::lock_guard<std::mutex> const lock{groupsMutex};
                    groups.push_back(g);
                }
                store.addEntry(std::chrono::system_clock::now(),
                               makeEntry(g.channel, g.level, g.file, g.line, g.ucSec, "live"));
                ++produced;
                ++index;
            }
        }};

        // spam filter changes while the producer runs
        for(int round = 0; round < 20; ++round) {
            RefFilter f;
            if(round % 2 == 0) { f.state.enabledLogLevels = {LogLevel::error, LogLevel::crit}; }
            if(round % 3 == 0) { f.state.enabledChannels = {0, 1, 2}; }
            store.setFilterState(f.state);
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        produce = false;
        producer.join();
        std::printf("-- concurrent producer added %zu groups during filter spam\n",
                    produced.load());

        RefFilter final;
        final.state.enabledLogLevels = {LogLevel::warn, LogLevel::error, LogLevel::crit};
        store.setFilterState(final.state);
        store.updateUcTime([](GuiEntryStore& s) {
            s.ucTimeFilterEnabled = false;
            s.requestRefilterLocked();
        });
        quiesce(store);
        validateFiltered(store, groups, final, "post-concurrency recount");
    }

    // ---- mutex latency probe while big refilters run -------------------------------------
    {
        store.setFilterState(FilterState{});
        quiesce(store);
        std::atomic<bool> running{true};
        std::atomic<long> maxAcquireUs{0};
        std::thread       probe{[&]() {
            while(running.load()) {
                auto const start = std::chrono::steady_clock::now();
                { std::lock_guard<std::mutex> const lock{store.mutex}; }
                auto const us       = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count();
                long       expected = maxAcquireUs.load();
                while(us > expected && !maxAcquireUs.compare_exchange_weak(expected, us)) {}
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            }
        }};
        for(int i = 0; i < 10; ++i) {
            RefFilter f;
            f.state.enabledChannels = {static_cast<std::size_t>(i % 6)};
            store.setFilterState(f.state);
            quiesce(store);
        }
        running = false;
        probe.join();
        std::printf("-- max store-mutex acquire latency during refilters: %ld us\n",
                    maxAcquireUs.load());
        CHECK(maxAcquireUs.load() < 20'000, "store mutex never held long");
    }

    // ---- clearAll mid-scan ----------------------------------------------------------------
    {
        RefFilter f;
        f.state.enabledLogLevels = {LogLevel::trace};
        store.setFilterState(f.state);   // kicks a big scan
        store.clearAll();                // races the scan on purpose
        quiesce(store);
        std::lock_guard<std::mutex> const lock{store.mutex};
        CHECK(store.allEntries.empty(), "clearAll emptied the store");
        CHECK(store.filteredEntries.empty(), "clearAll emptied the filtered view");
        CHECK(store.originalLogCount == 0 && store.filteredOriginalLogCount == 0, "counters reset");
    }

    // ---- clearBeforeLastBoot with a ucTime reset -----------------------------------------
    {
        store.setFilterState(FilterState{});   // drop the trace-only filter from above
        quiesce(store);
        for(std::size_t i = 0; i < 1000; ++i) {
            store.addEntry(
              std::chrono::system_clock::now(),
              makeEntry(0, LogLevel::info, "main.cpp", 10, static_cast<double>(i), "boot1"));
        }
        // target reset: ucTime starts over
        for(std::size_t i = 0; i < 500; ++i) {
            store.addEntry(
              std::chrono::system_clock::now(),
              makeEntry(0, LogLevel::info, "main.cpp", 10, static_cast<double>(i) * 0.5, "boot2"));
        }
        store.clearBeforeLastBoot();
        quiesce(store);
        std::lock_guard<std::mutex> const lock{store.mutex};
        CHECK(store.allEntries.size() == 500, "only the last boot retained");
        CHECK(store.originalLogCount == 500, "retained count recomputed");
        CHECK(store.filteredEntries.size() == 500, "filtered view rebuilt");
    }

    // ---- trim invariants with an injected small cap --------------------------------------
    {
        GuiEntryStore small;
        small.requestRedraw = []() {};
        small.maxLogEntries = 50'000;
        small.trimSlack     = 50'000 / 16;
        for(std::size_t i = 0; i < 120'000; ++i) {
            auto const msg
              = (i % 100 == 0) ? std::string{"multi\nline\nmessage"} : std::string{"message"};
            small.addEntry(std::chrono::system_clock::now(),
                           makeEntry(i % 6,
                                     LogLevel::info,
                                     "main.cpp",
                                     10,
                                     static_cast<double>(i) * 0.001,
                                     msg));
        }
        std::lock_guard<std::mutex> const lock{small.mutex};
        CHECK(small.allEntries.size() <= small.maxLogEntries, "cap enforced");
        CHECK(small.trimmedLogCount > 0, "trim happened");
        CHECK(small.trimmedLogCount + small.originalLogCount == small.nextMultilineGroupId,
              "trimmed + retained == produced");
        CHECK(small.allEntries.front().startsGroup(), "trim landed on a group boundary");
        auto const minGroup = small.allEntries.front().multilineGroupId();
        bool       ok       = true;
        for(std::size_t i = 0; i < small.filteredEntries.size() && ok; ++i) {
            ok = small.filteredEntries[i].multilineGroupId() >= minGroup;
        }
        CHECK(ok, "filtered view has no trimmed groups");
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
