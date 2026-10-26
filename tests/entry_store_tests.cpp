// Correctness harness for EntryStore: A/B of the compiled filter against the old
// string-based semantics, async refilter under concurrent ingest, clear/trim invariants,
// and a store-mutex latency probe.
//
// Build (from scratchpad):
#include "support/Check.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/EntryStore.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace uc_log::printer;
using uc_log::LogLevel;
using uc_log::printer::LogEntry;

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

namespace {

bool refPasses(RefGroup const&  g,
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

LogEntry makeEntry(std::size_t        channel,
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

void quiesce(EntryStore& store) {
    for(;;) {
        store.pollRefilterDeadline();
        if(store.settled()) { return; }
        uc_log::printer::Clock::sleep_for(std::chrono::milliseconds{1});
    }
}

// walk the filtered list and validate group structure + membership against the reference
void validateFiltered(EntryStore&                  store,
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

    store.withLines(
      [&](EntryStore::Counts const& counts, EntryChunkList const&, EntryChunkList const& filtered) {
          if(counts.filteredLines != expectedLines
             || counts.filteredOriginalLogs != expectedGroups) {
              std::printf("FAIL: %s: got %zu lines/%zu groups, expected %zu/%zu\n",
                          what,
                          counts.filteredLines,
                          counts.filteredOriginalLogs,
                          expectedLines,
                          expectedGroups);
              ++uc_log::test::failures;
              return;
          }
          // every group start in the filtered list must pass the reference filter
          // (groups are identified by multilineGroupId == 1-based index into `groups`)
          for(std::size_t i = 0; i < filtered.size(); ++i) {
              auto const& e = filtered[i];
              if(!e.startsGroup()) { continue; }
              auto const  gid = e.multilineGroupId();
              auto const& g   = groups[gid - 1];
              if(!refPasses(g, filter)) {
                  std::printf("FAIL: %s: group %zu should not pass\n", what, gid);
                  ++uc_log::test::failures;
                  return;
              }
          }
      });
}

}   // namespace

int main(int    argc,
         char** argv) {
    // the refilter debounce on the virtual clock: it passes while this thread sleeps (the store's worker and the
    // producer below never wait on the clock: they only work)
    uc_log::printer::VirtualTime const virtualTime;
    std::size_t                        total = 100'000;
    if(argc > 1) { total = std::strtoull(*std::next(argv, 1), nullptr, 10); }

    static constexpr std::array files{"main.cpp", "foo.hpp", "bar.c", "baz.hpp", "qux.cpp"};

    EntryStore               store;
    std::atomic<std::size_t> redraws{0};
    store.setRedrawRequest([&redraws]() { ++redraws; });

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
    for(std::size_t i = 0; i < total; ++i) {
        auto const g = randomGroup(i);
        groups.push_back(g);
        auto const msg
          = g.lineCount == 1 ? std::string{"message"} : std::string{"multi\nline\nmessage"};
        store.addEntry(std::chrono::system_clock::now(),
                       makeEntry(g.channel, g.level, g.file, g.line, g.ucSec, msg));
    }
    std::printf("-- ingested %zu groups (%zu redraw requests)\n", total, redraws.load());

    // ---- A/B filter scenarios ------------------------------------------------------------
    auto applyAndCheck = [&](RefFilter const& f, char const* what) {
        store.setFilterState(f.state);
        store.setSearchText(f.searchMode == 0   ? ""
                            : f.searchMode == 1 ? "MULTI"            // case-insensitive
                                                : "re:mu.ti\\nl");   // spans the newline
        store.updateUcTime([&f](EntryStore::UcTimeWindow& w, double /*dataMax*/) {
            w.enabled = f.ucEnabled;
            w.live    = false;
            w.minSec  = f.minSec;
            w.maxSec  = f.maxSec;
            return true;
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
                    std::scoped_lock<std::mutex> const lock{groupsMutex};
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
            uc_log::printer::Clock::sleep_for(std::chrono::milliseconds{5});
        }
        produce = false;
        producer.join();
        std::printf("-- concurrent producer added %zu groups during filter spam\n",
                    produced.load());

        RefFilter final;
        final.state.enabledLogLevels = {LogLevel::warn, LogLevel::error, LogLevel::crit};
        store.setFilterState(final.state);
        store.updateUcTime([](EntryStore::UcTimeWindow& w, double /*dataMax*/) {
            w.enabled = false;
            return true;
        });
        quiesce(store);
        validateFiltered(store, groups, final, "post-concurrency recount");
    }

    // ---- the store mutex is free while a big refilter scans -------------------------------
    // The scan is stopped in its middle (the redraw it asks for every few chunks, outside the lock) and the mutex is
    // taken right then: a scan under the lock would hold it for the whole store. No clock decides it.
    if(total >= 5 * EntryStore::refilterChunk) {
        store.setFilterState(FilterState{});
        quiesce(store);
        std::mutex              m;
        std::condition_variable cv;
        bool                    paused = false;
        bool                    resume = false;
        bool                    armed  = true;   // the first redraw from inside the scan pauses it
        store.setRedrawRequest([&]() {
            ++redraws;
            if(auto const p = store.refilterProgress(); !p.inProgress || p.scanned >= p.total) {
                return;
            }
            std::unique_lock<std::mutex> lock{m};
            if(!std::exchange(armed, false)) { return; }
            paused = true;
            cv.notify_all();
            cv.wait(lock, [&] { return resume; });
        });
        RefFilter f;
        f.state.enabledChannels = {1};
        store.setFilterState(f.state);
        store.pollRefilterDeadline();
        {
            std::unique_lock<std::mutex> lock{m};
            while(!paused) {
                lock.unlock();
                store.pollRefilterDeadline();   // the debounce, then the scan starts
                uc_log::printer::Clock::sleep_for(std::chrono::milliseconds{1});
                lock.lock();
            }
        }
        bool const free = store.lockIsFree();
        CHECK(free, "store mutex free in the middle of a refilter's scan");
        {
            std::scoped_lock<std::mutex> const lock{m};
            resume = true;
        }
        cv.notify_all();
        quiesce(store);
        store.setRedrawRequest([&redraws]() { ++redraws; });
    }

    // ---- clearAll mid-scan ----------------------------------------------------------------
    {
        RefFilter f;
        f.state.enabledLogLevels = {LogLevel::trace};
        store.setFilterState(f.state);   // kicks a big scan
        store.clearAll();                // races the scan on purpose
        quiesce(store);
        auto const counts = store.counts();
        CHECK(counts.lines == 0, "clearAll emptied the store");
        CHECK(counts.filteredLines == 0, "clearAll emptied the filtered view");
        CHECK(counts.originalLogs == 0 && counts.filteredOriginalLogs == 0, "counters reset");
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
        auto const counts = store.counts();
        CHECK(counts.lines == 500, "only the last boot retained");
        CHECK(counts.originalLogs == 500, "retained count recomputed");
        CHECK(counts.filteredLines == 500, "filtered view rebuilt");
    }

    // ---- trim invariants with an injected small cap --------------------------------------
    {
        static constexpr std::size_t maxLines = 50'000;
        EntryStore                   small{
          EntryStoreLimits{.maxLines = maxLines, .trimSlack = maxLines / 16}
        };
        small.setRedrawRequest([]() {});
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
        small.withLines([](EntryStore::Counts const& counts,
                           EntryChunkList const&     all,
                           EntryChunkList const&     filtered) {
            CHECK(counts.lines <= maxLines, "cap enforced");
            CHECK(counts.trimmedLogs > 0, "trim happened");
            CHECK(counts.trimmedLogs + counts.originalLogs == counts.addedLogs,
                  "trimmed + retained == produced");
            CHECK(all.front().startsGroup(), "trim landed on a group boundary");
            auto const minGroup = all.front().multilineGroupId();
            bool       ok       = true;
            for(std::size_t i = 0; i < filtered.size() && ok; ++i) {
                ok = filtered[i].multilineGroupId() >= minGroup;
            }
            CHECK(ok, "filtered view has no trimmed groups");
        });
    }

    {
        EntryStore mods;
        mods.setRedrawRequest([]() {});
        auto add = [&](std::string const& module, std::string const& msg) {
            auto e   = makeEntry(0, LogLevel::info, "m.cpp", 1, 0.0, msg);
            e.module = module;
            mods.addEntry(std::chrono::system_clock::now(), e);
        };
        auto shown = [&]() {
            std::vector<std::string> v;
            mods.withLines([&](EntryStore::Counts const&,
                               EntryChunkList const&,
                               EntryChunkList const& filtered) {
                for(std::size_t i = 0; i < filtered.size(); ++i) {
                    v.emplace_back(filtered[i].lineText());
                }
            });
            return v;
        };
        add("i2c", "i");
        add("i2c.bus", "ib");
        add("i2cx", "x");   // shares the letters, not the path
        add("usb.cdc", "uc");
        add("", "plain");

        FilterState f;
        f.excludedModules = {"i2c"};
        mods.setFilterState(f);
        quiesce(mods);
        CHECK((shown() == std::vector<std::string>{"x", "uc", "plain"}),
              "i2c hides i2c and i2c.bus only");

        add("i2c.scanner", "is");
        add("usb.device", "ud");
        CHECK((shown() == std::vector<std::string>{"x", "uc", "plain", "ud"}),
              "a new module is judged by its parents");

        f.excludedModules = {"usb.cdc"};
        mods.setFilterState(f);
        quiesce(mods);
        CHECK((shown() == std::vector<std::string>{"i", "ib", "x", "plain", "is", "ud"}),
              "a leaf hidden, its sibling shown");

        mods.setFilterState(FilterState{});
        quiesce(mods);
        CHECK(shown().size() == 7, "no module filter: everything");

        EntryStore::Mirror m;
        mods.refreshMirror(m);
        CHECK((m.moduleNamesById
               == std::vector<
                 std::string>{"i2c", "i2c.bus", "i2cx", "usb.cdc", "i2c.scanner", "usb.device"}),
              "the mirror lists every module seen, by id (a filter's names are not modules)");
        CHECK(m.moduleNameOf(uc_log::printer::noModuleId).empty(), "no module: empty name");
        CHECK(uc_log::printer::moduleUnder("i2c.bus", "i2c")
                && uc_log::printer::moduleUnder("i2c", "i2c")
                && !uc_log::printer::moduleUnder("i2cx", "i2c")
                && !uc_log::printer::moduleUnder("i2c", "i2c.bus"),
              "prefix on '.' boundaries");
    }

    // ---- a snapshot is read without the lock while the owner appends to the chunk it shares ----
    // The writer appends under the mutex and hands out a snapshot every few entries; the reader
    // walks each snapshot unlocked, as refilterWorker does. Both work on the same last chunk, and
    // the writer fills a second one too. Under TSan this caught the std::vector chunk (the
    // owner's push_back wrote the end pointer that the reader's hardened operator[] read).
    {
        static constexpr std::size_t entries = EntryChunkList::chunkSize + 4096;
        static constexpr std::size_t every   = 64;

        std::mutex                              m;
        std::condition_variable                 cv;
        EntryChunkList                          list;
        std::optional<EntryChunkList::Snapshot> handed;
        bool                                    done{false};
        std::size_t                             readerBad{0};
        std::size_t                             readerSnapshots{0};

        auto entryFor = [](std::size_t i) {
            auto common              = std::make_shared<EntryCommon>();
            common->multilineGroupId = i;
            return StoredLine{std::move(common), 0, 0, LineType::singleLine};
        };

        std::thread reader{[&] {
            while(true) {
                std::optional<EntryChunkList::Snapshot> snap;
                {
                    std::unique_lock<std::mutex> lock{m};
                    cv.wait(lock, [&] { return handed.has_value() || done; });
                    if(!handed) { return; }
                    snap.swap(handed);
                }
                ++readerSnapshots;
                for(std::size_t i = 0; i != snap->size(); ++i) {
                    if((*snap)[i].multilineGroupId() != i) { ++readerBad; }
                }
            }
        }};
        for(std::size_t i = 0; i != entries; ++i) {
            std::scoped_lock<std::mutex> const lock{m};
            list.push_back(entryFor(i));
            if(i % every == 0) {
                handed = list.snapshot();
                cv.notify_one();
            }
        }
        {
            std::scoped_lock<std::mutex> const lock{m};
            done = true;
        }
        cv.notify_one();
        reader.join();
        CHECKF(readerBad == 0,
               "a snapshot reads the entries it was taken with (%zu wrong)",
               readerBad);
        CHECK(readerSnapshots > 0, "the reader saw snapshots");
        CHECK(list.size() == entries && list[entries - 1].multilineGroupId() == entries - 1,
              "the owner's view holds every entry");
        auto const kept = list.snapshot();
        list.eraseFront(EntryChunkList::chunkSize + 1);
        CHECK(list.chunks.size() == 1, "a full first chunk is dropped by eraseFront");
        CHECK(kept[0].multilineGroupId() == 0
                && kept[entries - 1].multilineGroupId() == entries - 1,
              "a snapshot keeps a dropped chunk alive");
    }

    return uc_log::test::report();
}
