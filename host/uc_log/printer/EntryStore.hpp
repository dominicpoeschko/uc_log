#pragma once

// The printer's store of decoded log lines, independent of any front end (no FTXUI): every line split into display
// lines, the filtered view the log tab shows, the metric series, the source locations and modules seen. The filter
// (printer/LogFilterState.hpp) is compiled to integers and a changed one is applied by a worker thread, so a front
// end never scans millions of lines itself; a front end reads the store through a per-frame Mirror.

#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LogFilterState.hpp"
#include "uc_log/printer/metric_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace uc_log::printer {

enum class LineType : std::uint8_t {
    singleLine,   // Complete log on one line
    first,        // First line of multiline log
    middle,       // Middle continuation line
    last          // Last line of multiline log
};

// Immutable data shared by all display lines of one original log message. File and
// function names live in the store's interning pools (referenced by id), so millions
// of entries do not repeat the same strings.
inline constexpr std::uint32_t noModuleId = std::numeric_limits<std::uint32_t>::max();

struct EntryCommon {
    std::chrono::system_clock::time_point recvTime;
    uc_log::printer::LogEntry::UcTime     ucTime;
    std::uint64_t                         locationKey;   // fileId << 32 | line
    std::size_t                           multilineGroupId;
    std::uint32_t                         functionId;
    std::uint32_t                         moduleId;   // noModuleId outside a module scope
    std::uint8_t                          channel;
    uc_log::LogLevel                      logLevel;
    bool                                  parsedOk;
    std::string                           message;   // full message, '\n' separated
};

// One display line: a shared header plus a slice of the shared message.
struct StoredLine {
    std::shared_ptr<EntryCommon const> common;
    std::uint32_t                      lineOffset{0};
    std::uint32_t                      lineLength{0};
    LineType                           lineType{LineType::singleLine};

    [[nodiscard]] std::string_view lineText() const {
        return std::string_view{common->message}.substr(lineOffset, lineLength);
    }

    [[nodiscard]] std::size_t multilineGroupId() const { return common->multilineGroupId; }

    [[nodiscard]] bool startsGroup() const {
        return lineType == LineType::singleLine || lineType == LineType::first;
    }
};

// Append-only chunked storage. Chunks have fixed capacity, so appends never move
// existing elements: a copy of the chunk pointers plus a size is a stable view that
// can be read without any lock while the owner keeps appending. Trimming only drops
// references; live snapshots keep trimmed chunks alive until they let go.
struct EntryChunkList {
    static constexpr std::size_t chunkSize = 64 * 1024;

    // A fixed block of chunkSize slots. Its fill count belongs to the owner (the one that
    // appends, under the store mutex); a snapshot reader indexes the slots below its own
    // size and never reads the count. A std::vector here raced: the owner's push_back
    // writes the vector's end pointer, and a lock-free reader's operator[] read it (the
    // hardened libc++ bounds check), both on the last chunk a snapshot shares.
    class Chunk {
    public:
        Chunk() = default;

        Chunk(Chunk const&)            = delete;
        Chunk& operator=(Chunk const&) = delete;

        ~Chunk() {
            // the last owner may be a reader dropping its snapshot; the shared_ptr count
            // orders the owner's appends before this
            for(std::size_t i = 0; i != used_; ++i) { std::destroy_at(&slots_[i].entry); }
        }

        // owner only
        [[nodiscard]] std::size_t size() const { return used_; }

        [[nodiscard]] bool full() const { return used_ == chunkSize; }

        void push_back(StoredLine entry) {
            std::construct_at(&slots_[used_].entry, std::move(entry));
            ++used_;
        }

        // owner, or a reader for an index below the size its snapshot was taken with
        StoredLine const& operator[](std::size_t i) const UC_LOG_LIFETIMEBOUND {
            return slots_[i].entry;
        }

    private:
        // raw room for one entry: constructed by push_back, destroyed by ~Chunk
        union Slot {
            Slot() {}

            Slot(Slot const&)            = delete;
            Slot& operator=(Slot const&) = delete;

            ~Slot() {}

            StoredLine entry;
        };

        std::array<Slot, chunkSize> slots_;   // its bounds check is a constant
        std::size_t                 used_{0};
    };

    std::vector<std::shared_ptr<Chunk>> chunks;
    std::size_t                         frontOffset{0};
    std::size_t                         totalSize{0};

    struct Snapshot {
        std::vector<std::shared_ptr<Chunk>> chunks;
        std::size_t                         frontOffset{0};
        std::size_t                         totalSize{0};

        [[nodiscard]] std::size_t size() const { return totalSize; }

        [[nodiscard]] bool empty() const { return totalSize == 0; }

        StoredLine const& operator[](std::size_t i) const {
            auto const j = i + frontOffset;
            return (*chunks[j / chunkSize])[j % chunkSize];
        }

        struct Iterator {
            using iterator_category = std::random_access_iterator_tag;
            using iterator_concept  = std::random_access_iterator_tag;
            using value_type        = StoredLine;
            using difference_type   = std::ptrdiff_t;
            using reference         = StoredLine const&;
            using pointer           = StoredLine const*;

            Snapshot const* snapshot{nullptr};
            std::size_t     index{0};

            reference operator*() const { return (*snapshot)[index]; }

            pointer operator->() const { return &(*snapshot)[index]; }

            reference operator[](difference_type n) const {
                return (*snapshot)[index + static_cast<std::size_t>(n)];
            }

            Iterator& operator++() UC_LOG_LIFETIMEBOUND {
                ++index;
                return *this;
            }

            Iterator operator++(int) {
                auto tmp = *this;
                ++index;
                return tmp;
            }

            Iterator& operator--() UC_LOG_LIFETIMEBOUND {
                --index;
                return *this;
            }

            Iterator operator--(int) {
                auto tmp = *this;
                --index;
                return tmp;
            }

            Iterator& operator+=(difference_type n) {
                index = static_cast<std::size_t>(static_cast<difference_type>(index) + n);
                return *this;
            }

            Iterator& operator-=(difference_type n) { return *this += -n; }

            friend Iterator operator+(Iterator        it,
                                      difference_type n) {
                return it += n;
            }

            friend Iterator operator+(difference_type n,
                                      Iterator        it) {
                return it += n;
            }

            friend Iterator operator-(Iterator        it,
                                      difference_type n) {
                return it -= n;
            }

            friend difference_type operator-(Iterator const& lhs,
                                             Iterator const& rhs) {
                return static_cast<difference_type>(lhs.index)
                     - static_cast<difference_type>(rhs.index);
            }

            friend bool operator==(Iterator const& lhs,
                                   Iterator const& rhs) {
                return lhs.index == rhs.index;
            }

            friend auto operator<=>(Iterator const& lhs,
                                    Iterator const& rhs) {
                return lhs.index <=> rhs.index;
            }
        };

        [[nodiscard]] Iterator begin() const { return {this, 0}; }

        [[nodiscard]] Iterator end() const { return {this, totalSize}; }
    };

    [[nodiscard]] Snapshot snapshot() const { return Snapshot{chunks, frontOffset, totalSize}; }

    [[nodiscard]] std::size_t size() const { return totalSize; }

    [[nodiscard]] bool empty() const { return totalSize == 0; }

    StoredLine const& operator[](std::size_t i) const {
        auto const j = i + frontOffset;
        return (*chunks[j / chunkSize])[j % chunkSize];
    }

    [[nodiscard]] StoredLine const& front() const { return (*this)[0]; }

    void push_back(StoredLine entry) {
        if(chunks.empty() || chunks.back()->full()) { chunks.push_back(std::make_shared<Chunk>()); }
        chunks.back()->push_back(std::move(entry));
        ++totalSize;
    }

    void eraseFront(std::size_t n) {
        n = std::min(n, totalSize);
        frontOffset += n;
        totalSize -= n;
        // only a full first chunk can be dropped: a partial one is still the append
        // target and the index arithmetic relies on non-last chunks being full
        while(!chunks.empty() && chunks.front()->full() && frontOffset >= chunkSize) {
            chunks.erase(chunks.begin());
            frontOffset -= chunkSize;
        }
    }

    void clear() {
        chunks.clear();
        frontOffset = 0;
        totalSize   = 0;
    }
};

// FilterState compiled to integer form: location strings interned to keys
// (fileId << 32 | line), sets to bitmasks/sorted vectors. passes() does no
// allocation and no string comparison, so scanning millions of entries is cheap.
struct CompiledFilter {
    static constexpr std::uint64_t fileMask = 0xFFFF'FFFF'0000'0000ULL;

    bool                       levelFilterActive{false};
    bool                       channelFilterActive{false};
    std::uint8_t               levelMask{0};
    std::uint8_t               channelMask{0};
    std::vector<std::uint64_t> includedKeys;        // sorted; file-wide keys have line == 0
    std::vector<std::uint64_t> excludedKeys;        // sorted
    std::vector<bool>          excludedModuleIds;   // by module id; ids past the end pass
    std::set<std::string>      excludedModules;     // for a module interned after compiling

    void noteModule(std::uint32_t    id,
                    std::string_view module) {
        if(excludedModules.empty() || !moduleHiddenBy(module, excludedModules)) { return; }
        if(excludedModuleIds.size() <= id) { excludedModuleIds.resize(id + 1, false); }
        excludedModuleIds[id] = true;
    }

    bool   ucTimeEnabled{false};
    double minUcTimeSec{0.0};
    double maxUcTimeSec{std::numeric_limits<double>::infinity()};

    // text search over the whole (multiline) message: lower-cased needle for
    // case-insensitive substring, or a pre-compiled regex ("re:" prefix in the input)
    bool                              searchActive{false};
    std::string                       searchNeedle;   // lower-cased
    std::shared_ptr<std::regex const> searchRegex;

    static bool containsCaseInsensitive(std::string_view haystack,
                                        std::string_view loweredNeedle) {
        if(loweredNeedle.empty()) { return true; }
        auto const found = std::ranges::search(haystack, loweredNeedle, [](char a, char b) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) == b;
        });
        return !found.empty();
    }

    [[nodiscard]] bool passes(StoredLine const& entry) const {
        auto const& common = *entry.common;
        if(levelFilterActive) {
            auto const bit = static_cast<std::uint8_t>(common.logLevel);
            if(bit >= 8 || (levelMask & (1U << bit)) == 0) { return false; }
        }
        if(channelFilterActive) {
            auto const channel = common.channel;
            if(channel >= 8 || (channelMask & (1U << channel)) == 0) { return false; }
        }
        if(common.moduleId < excludedModuleIds.size() && excludedModuleIds[common.moduleId]) {
            return false;
        }
        if(ucTimeEnabled) {
            auto const s = std::chrono::duration<double>(common.ucTime.time).count();
            if(s < minUcTimeSec || s > maxUcTimeSec) { return false; }
        }
        if(searchActive) {
            if(searchRegex) {
                if(!std::regex_search(common.message, *searchRegex)) { return false; }
            } else if(!containsCaseInsensitive(common.message, searchNeedle)) {
                return false;
            }
        }

        auto const entryKey = common.locationKey;
        auto const fileKey  = entryKey & fileMask;

        bool const hasExclusions = !excludedKeys.empty();
        bool const hasInclusions = !includedKeys.empty();

        if(hasExclusions && std::ranges::binary_search(excludedKeys, entryKey)) { return false; }
        if(hasInclusions && std::ranges::binary_search(includedKeys, entryKey)) { return true; }
        if(hasExclusions && std::ranges::binary_search(excludedKeys, fileKey)) { return false; }
        if(hasExclusions) { return true; }
        if(hasInclusions) { return std::ranges::binary_search(includedKeys, fileKey); }
        return true;
    }
};

// The limits of one store (EntryStore's constructor); the tests make them small.
struct EntryStoreLimits {
    std::size_t maxLines{10'000'000};
    // Trims are batched: once maxLines is exceeded, drop down to maxLines - trimSlack so the O(N) front-erase
    // amortizes over many appends.
    std::size_t trimSlack{10'000'000 / 16};
};

// Owns all log-derived data behind its own mutex so the producer (addEntry) and the refilter worker never contend
// with a front end's rendering. A front end reads through a per-frame Mirror (refreshMirror) and the calls below,
// never the members.
//
// Lock order: a front end's own mutex (if any) is always taken before this mutex, never after. The redraw request
// (setRedrawRequest) must be safe to call from any thread and must not take a front end's locks.
//
// Invariant: allEntries may only be appended to while a scan generation is live; any erase/clear/trim must bump
// refilterGeneration so the worker abandons its snapshot.
class EntryStore {
public:
    static constexpr std::size_t refilterChunk        = 16 * 1024;
    static constexpr auto        filterDebounce       = std::chrono::milliseconds{300};
    static constexpr auto        liveRefilterThrottle = std::chrono::milliseconds{250};
    // metric series are bounded per key so a chatty metric cannot grow without limit
    static constexpr std::size_t maxMetricEntriesPerKey = 100'000;
    // location labels contain live counts; count-only updates are rebuilt at most this often, new locations at once
    static constexpr auto locationLabelRefresh = std::chrono::milliseconds{500};

    struct LogStats {
        std::size_t                           peakLogsPerSecond{0};
        std::chrono::system_clock::time_point lastLogRateUpdate{std::chrono::system_clock::now()};
        std::size_t                           logsInCurrentSecond{0};
        std::optional<uc_log::printer::LogEntry::UcTime> lastUcTime;
        std::size_t                                      detectedResetCount{0};
        std::size_t                                      parseFailureCount{0};
    };

    /// The target-time window of the filter (the Filter tab's uc time section); live: [newest - liveWindowSecs, inf)
    struct UcTimeWindow {
        bool   enabled{false};
        bool   live{false};
        double liveWindowSecs{10.0};
        double minSec{0.0};
        double maxSec{std::numeric_limits<double>::infinity()};
    };

    /// What the store holds right now, counted in display lines and in original log lines (groups)
    struct Counts {
        std::size_t lines{0};
        std::size_t filteredLines{0};
        std::size_t originalLogs{0};           // retained
        std::size_t filteredOriginalLogs{0};   // retained and passing the filter
        std::size_t trimmedLogs{0};
        std::size_t addedLogs{0};   // since the start: trimmed + retained + cleared
    };

    struct RefilterProgress {
        bool        inProgress{false};
        std::size_t scanned{0};
        std::size_t total{0};
    };

    explicit EntryStore(EntryStoreLimits const& limits = {})
      : maxLogEntries{limits.maxLines}
      , trimSlack{limits.trimSlack} {}

    EntryStore(EntryStore const&)            = delete;
    EntryStore& operator=(EntryStore const&) = delete;

    /// Set by the owner before data flows (a test may set it again while the store is settled); called from the
    /// producer and the worker thread, outside the store's lock
    void setRedrawRequest(std::function<void()> request) { requestRedraw = std::move(request); }

private:
    std::mutex mutable mutex;

    // interning pools, guarded by mutex; the ById vectors only ever grow
    std::map<std::string, std::uint32_t, std::less<>> fileIds;
    std::vector<std::string>                          fileNamesById;
    // keyed by short name and signature: instantiations that abbreviate alike stay apart
    std::map<std::string, std::uint32_t, std::less<>> functionIds;
    std::vector<std::string>                          functionNamesById;
    std::vector<std::string>                          functionSignaturesById;
    std::map<std::string, std::uint32_t, std::less<>> moduleIds;
    std::vector<std::string>                          moduleNamesById;

    std::map<SourceLocation, std::size_t> allSourceLocations;
    std::uint64_t                         locationsVersion{0};

    EntryChunkList allEntries;
    EntryChunkList filteredEntries;
    std::uint64_t  displayVersion{0};   // bumped whenever the filtered view changes

    std::size_t nextMultilineGroupId{0};
    std::size_t originalLogCount{0};           // original logs currently retained
    std::size_t filteredOriginalLogCount{0};   // original logs passing the filter
    std::size_t trimmedLogCount{0};

    std::map<MetricInfo, std::vector<MetricEntry>> metricEntries;
    LogStats                                       logStats;

    // uc-time data range (data-derived, written by addEntry) and the window (the front end's, and addEntry's in
    // live mode)
    double       ucTimeDataMin{std::numeric_limits<double>::infinity()};
    double       ucTimeDataMax{-std::numeric_limits<double>::infinity()};
    UcTimeWindow ucTime;

    FilterState activeFilterState;
    std::string searchText;   // raw search input, "re:" prefix selects regex mode

    // Filter currently reflected in filteredEntries; addEntry judges new entries with
    // it. pendingFilter is the job spec for the refilter worker.
    CompiledFilter displayedFilter;
    CompiledFilter pendingFilter;

    std::atomic<std::uint64_t> refilterGeneration{0};   // written under mutex only
    std::uint64_t              displayedGeneration{0};
    std::atomic<bool>          refilterInProgress{false};
    std::atomic<std::size_t>   refilterScanned{0};
    std::atomic<std::size_t>   refilterTotal{0};
    std::optional<uc_log::printer::Clock::time_point> refilterDeadline;
    std::condition_variable_any                       refilterCv;

    std::function<void()> requestRedraw;

    std::size_t maxLogEntries;
    std::size_t trimSlack;

    // ---------- interning (mutex held) ----------

    std::uint32_t internFileLocked(std::string_view file) {
        auto it = fileIds.find(file);
        if(it == fileIds.end()) {
            it = fileIds.emplace(std::string{file}, static_cast<std::uint32_t>(fileIds.size()))
                   .first;
            fileNamesById.emplace_back(file);
        }
        return it->second;
    }

    std::uint32_t internFunctionLocked(std::string_view function,
                                       std::string_view signature) {
        std::string key{function};
        key += '\0';
        key += signature;
        auto it = functionIds.find(key);
        if(it == functionIds.end()) {
            it = functionIds.emplace(std::move(key), static_cast<std::uint32_t>(functionIds.size()))
                   .first;
            functionNamesById.emplace_back(function);
            functionSignaturesById.emplace_back(signature);
        }
        return it->second;
    }

    std::uint32_t internModuleLocked(std::string_view module) {
        if(module.empty()) { return noModuleId; }
        auto it = moduleIds.find(module);
        if(it == moduleIds.end()) {
            it
              = moduleIds.emplace(std::string{module}, static_cast<std::uint32_t>(moduleIds.size()))
                  .first;
            moduleNamesById.emplace_back(module);
            displayedFilter.noteModule(it->second, module);
            pendingFilter.noteModule(it->second, module);
        }
        return it->second;
    }

    std::uint64_t internLocationKeyLocked(std::string_view file,
                                          std::size_t      line) {
        return (static_cast<std::uint64_t>(internFileLocked(file)) << 32U)
             | (line & 0xFFFF'FFFFULL);
    }

    // ---------- filtering ----------

    CompiledFilter compileFilterLocked() {
        CompiledFilter f{};
        f.levelFilterActive = !activeFilterState.enabledLogLevels.empty();
        for(auto const level : activeFilterState.enabledLogLevels) {
            f.levelMask |= static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(level));
        }
        f.channelFilterActive = !activeFilterState.enabledChannels.empty();
        for(auto const channel : activeFilterState.enabledChannels) {
            if(channel < 8) { f.channelMask |= static_cast<std::uint8_t>(1U << channel); }
        }
        for(auto const& [file, line] : activeFilterState.includedLocations) {
            f.includedKeys.push_back(internLocationKeyLocked(file, line));
        }
        for(auto const& [file, line] : activeFilterState.excludedLocations) {
            f.excludedKeys.push_back(internLocationKeyLocked(file, line));
        }
        std::ranges::sort(f.includedKeys);
        std::ranges::sort(f.excludedKeys);
        f.excludedModules = activeFilterState.excludedModules;
        for(std::uint32_t id = 0; id < moduleNamesById.size(); ++id) {
            f.noteModule(id, moduleNamesById[id]);
        }
        f.ucTimeEnabled = ucTime.enabled;
        f.minUcTimeSec  = ucTime.minSec;
        f.maxUcTimeSec  = ucTime.maxSec;
        if(!searchText.empty()) {
            f.searchActive = true;
            if(searchText.starts_with("re:")) {
                try {
                    f.searchRegex = std::make_shared<std::regex const>(searchText.substr(3),
                                                                       std::regex::icase
                                                                         | std::regex::optimize);
                } catch(std::regex_error const&) {
                    // invalid regex: fall back to a literal match of the raw input so
                    // typing "re:[" mid-expression never throws or matches everything
                    f.searchRegex.reset();
                }
            }
            if(!f.searchRegex) {
                f.searchNeedle.resize(searchText.size());
                std::ranges::transform(searchText, f.searchNeedle.begin(), [](char c) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
            }
        }
        return f;
    }

    void requestRefilterLocked() {
        pendingFilter = compileFilterLocked();
        refilterDeadline.reset();
        refilterGeneration.store(refilterGeneration.load(std::memory_order_relaxed) + 1,
                                 std::memory_order_relaxed);
        refilterInProgress = true;
        refilterCv.notify_one();
    }

    // Leading-edge throttle for continuous triggers (uc-time live mode): a pending
    // deadline is never pushed back, so a steady stream cannot starve the refilter.
    void scheduleRefilterThrottledLocked() {
        if(!refilterDeadline) {
            refilterDeadline = uc_log::printer::Clock::now() + liveRefilterThrottle;
        }
    }

public:
    void setSearchText(std::string const& text) {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(searchText == text) { return; }
        searchText       = text;
        refilterDeadline = uc_log::printer::Clock::now() + filterDebounce;
    }

    void setFilterState(FilterState const& fs) {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(activeFilterState == fs) { return; }
        activeFilterState = fs;
        requestRefilterLocked();
    }

    // Trailing-edge debounce for text inputs: each keystroke pushes the deadline.
    void scheduleRefilterDebounced() {
        std::scoped_lock<std::mutex> const lock{mutex};
        refilterDeadline = uc_log::printer::Clock::now() + filterDebounce;
    }

    // called periodically from the UI loop; fires the deferred refilter when due
    void pollRefilterDeadline() {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(refilterDeadline && uc_log::printer::Clock::now() >= *refilterDeadline) {
            requestRefilterLocked();
        }
    }

    // ---------- uc-time filter state (called from UI callbacks) ----------

    /// f(UcTimeWindow& window, double dataMax) under the lock, dataMax the newest target time seen; f returns true
    /// to refilter at once (false: the caller debounces with scheduleRefilterDebounced, or nothing to do)
    template<typename F>
    void updateUcTime(F&& f) {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(std::forward<F>(f)(ucTime, ucTimeDataMax)) { requestRefilterLocked(); }
    }

    // ---------- ingest (producer thread) ----------

    void addEntry(std::chrono::system_clock::time_point recvTime,
                  uc_log::printer::LogEntry const&      entry) {
        {
            std::scoped_lock<std::mutex> const lock{mutex};

            ++originalLogCount;
            updateLogRateStatisticsLocked();
            if(!entry.parsedOk) { ++logStats.parseFailureCount; }

            // Detect target reset: ucTime jumping backwards by more than a second
            if(logStats.lastUcTime.has_value() && entry.ucTime < logStats.lastUcTime.value()
               && logStats.lastUcTime.value().time - entry.ucTime.time > std::chrono::seconds{1})
            {
                ++logStats.detectedResetCount;
            }
            logStats.lastUcTime = entry.ucTime;

            {
                auto const ucSecs = std::chrono::duration<double>(entry.ucTime.time).count();
                ucTimeDataMin     = std::min(ucTimeDataMin, ucSecs);
                bool const newMax = ucSecs > ucTimeDataMax;
                ucTimeDataMax     = std::max(ucTimeDataMax, ucSecs);
                if(ucTime.live && newMax) {
                    ucTime.minSec  = std::max(0.0, ucTimeDataMax - ucTime.liveWindowSecs);
                    ucTime.maxSec  = std::numeric_limits<double>::infinity();
                    ucTime.enabled = true;
                    // Judge new entries against the fresh window immediately; sliding
                    // old entries out of view is done by a throttled background rescan.
                    displayedFilter.ucTimeEnabled = true;
                    displayedFilter.minUcTimeSec  = ucTime.minSec;
                    displayedFilter.maxUcTimeSec  = ucTime.maxSec;
                    scheduleRefilterThrottledLocked();
                }
            }

            for(auto const& metric : uc_log::printer::extractMetrics(recvTime, entry)) {
                auto& values = metricEntries[metric.first];
                values.push_back(metric.second);
                // bounded per key: drop the oldest quarter in one amortized batch
                if(values.size() > maxMetricEntriesPerKey) {
                    values.erase(values.begin(),
                                 values.begin()
                                   + static_cast<std::ptrdiff_t>(maxMetricEntriesPerKey / 4));
                }
            }

            allSourceLocations[SourceLocation{entry.fileName, entry.line}]++;
            ++locationsVersion;

            auto common              = std::make_shared<EntryCommon>();
            common->recvTime         = recvTime;
            common->ucTime           = entry.ucTime;
            common->locationKey      = internLocationKeyLocked(entry.fileName, entry.line);
            common->multilineGroupId = ++nextMultilineGroupId;
            common->functionId       = internFunctionLocked(entry.functionName, entry.signature);
            common->moduleId         = internModuleLocked(entry.module);
            common->channel          = static_cast<std::uint8_t>(entry.channel.channel);
            common->logLevel         = entry.logLevel;
            common->parsedOk         = entry.parsedOk;
            common->message          = entry.logMsg;

            std::shared_ptr<EntryCommon const> const shared = std::move(common);

            // split into display lines by offset, mirroring the old splitIntoLines:
            // trailing newlines dropped, empty message yields one empty line
            std::string_view msg{shared->message};
            while(!msg.empty() && msg.back() == '\n') { msg.remove_suffix(1); }

            bool groupPasses = false;
            auto pushLine    = [&](std::size_t offset, std::size_t length, LineType type) {
                StoredLine line{shared,
                                static_cast<std::uint32_t>(offset),
                                static_cast<std::uint32_t>(length),
                                type};
                if(line.startsGroup()) {
                    groupPasses = displayedFilter.passes(line);
                    if(groupPasses) { ++filteredOriginalLogCount; }
                }
                allEntries.push_back(line);
                if(groupPasses) { filteredEntries.push_back(std::move(line)); }
            };

            auto const firstNewline = msg.find('\n');
            if(firstNewline == std::string_view::npos) {
                pushLine(0, msg.size(), LineType::singleLine);
            } else {
                std::size_t offset = 0;
                while(true) {
                    auto const nl   = msg.find('\n', offset);
                    bool const last = nl == std::string_view::npos;
                    auto const len  = last ? msg.size() - offset : nl - offset;
                    auto const type = offset == 0 ? LineType::first
                                    : last        ? LineType::last
                                                  : LineType::middle;
                    pushLine(offset, len, type);
                    if(last) { break; }
                    offset = nl + 1;
                }
            }

            enforceMaxLogEntriesLocked();
            ++displayVersion;
        }
        if(requestRedraw) { requestRedraw(); }
    }

    // ---------- maintenance ----------

    void clearAll() {
        std::scoped_lock<std::mutex> const lock{mutex};
        allEntries.clear();
        filteredEntries.clear();
        originalLogCount         = 0;
        filteredOriginalLogCount = 0;
        trimmedLogCount          = 0;
        ucTimeDataMin            = std::numeric_limits<double>::infinity();
        ucTimeDataMax            = -std::numeric_limits<double>::infinity();
        ++displayVersion;
        // bump the generation so a mid-scan worker abandons its snapshot
        requestRefilterLocked();
    }

    void clearBeforeLastBoot() {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(allEntries.empty()) { return; }
        std::size_t bootStart = 0;
        for(std::size_t i = 1; i < allEntries.size(); ++i) {
            if(allEntries[i].common->ucTime.time < allEntries[i - 1].common->ucTime.time) {
                bootStart = i;
            }
        }
        if(bootStart == 0) { return; }
        allEntries.eraseFront(bootStart);
        ucTimeDataMin    = std::numeric_limits<double>::infinity();
        ucTimeDataMax    = -std::numeric_limits<double>::infinity();
        originalLogCount = 0;
        for(std::size_t i = 0; i < allEntries.size(); ++i) {
            auto const& e = allEntries[i];
            if(e.startsGroup()) { ++originalLogCount; }
            auto const ucSecs = std::chrono::duration<double>(e.common->ucTime.time).count();
            ucTimeDataMin     = std::min(ucTimeDataMin, ucSecs);
            ucTimeDataMax     = std::max(ucTimeDataMax, ucSecs);
        }
        ++displayVersion;
        requestRefilterLocked();
    }

    void resetLogStats() {
        std::scoped_lock<std::mutex> const lock{mutex};
        logStats = LogStats{};
    }

    // ---------- UI mirror ----------

    struct Mirror {
        EntryChunkList::Snapshot entries;
        std::size_t              originalLogCount{0};
        std::size_t              filteredOriginalLogCount{0};
        std::size_t              trimmedLogCount{0};
        LogStats                 logStats{};
        double                   ucTimeDataMin{std::numeric_limits<double>::infinity()};
        double                   ucTimeDataMax{-std::numeric_limits<double>::infinity()};
        bool                     ucTimeFilterEnabled{false};
        bool                     ucTimeLiveMode{false};
        double                   ucTimeLiveWindowSecs{10.0};
        double                   minUcTimeSec{0.0};
        double                   maxUcTimeSec{std::numeric_limits<double>::infinity()};
        bool                     filterActive{false};
        std::string              searchText;
        std::string              searchNeedleLower;   // empty in regex mode

        std::vector<std::string> fileNamesById;
        std::vector<std::string> functionNamesById;
        std::vector<std::string> functionSignaturesById;
        // every module seen, even if all its lines are filtered out
        std::vector<std::string> moduleNamesById;

        std::vector<std::pair<SourceLocation, std::size_t>> locationList;
        std::vector<std::string>                            locationLabels;

        std::uint64_t seenDisplayVersion{std::numeric_limits<std::uint64_t>::max()};
        std::uint64_t seenLocationsVersion{std::numeric_limits<std::uint64_t>::max()};
        uc_log::printer::Clock::time_point lastLocationRebuild{};

        [[nodiscard]] std::string fileNameOf(std::uint64_t locationKey) const {
            auto const id = static_cast<std::size_t>(locationKey >> 32U);
            return id < fileNamesById.size() ? fileNamesById[id] : std::string{"?"};
        }

        static std::size_t lineOf(std::uint64_t locationKey) {
            return static_cast<std::size_t>(locationKey & 0xFFFF'FFFFULL);
        }

        [[nodiscard]] std::string functionNameOf(std::uint32_t id) const {
            return id < functionNamesById.size() ? functionNamesById[id] : std::string{"?"};
        }

        [[nodiscard]] std::string functionSignatureOf(std::uint32_t id) const {
            if(id < functionSignaturesById.size() && !functionSignaturesById[id].empty()) {
                return functionSignaturesById[id];
            }
            return functionNameOf(id);
        }

        [[nodiscard]] std::string moduleNameOf(std::uint32_t id) const {
            return id < moduleNamesById.size() ? moduleNamesById[id] : std::string{};
        }
    };

    // Called once per UI frame, before rendering: everything the renderers touch is
    // copied out here so no renderer ever takes the store mutex.
    void refreshMirror(Mirror& m) {
        std::scoped_lock<std::mutex> const lock{mutex};
        m.originalLogCount         = originalLogCount;
        m.filteredOriginalLogCount = filteredOriginalLogCount;
        m.trimmedLogCount          = trimmedLogCount;
        m.logStats                 = logStats;
        m.ucTimeDataMin            = ucTimeDataMin;
        m.ucTimeDataMax            = ucTimeDataMax;
        m.ucTimeFilterEnabled      = ucTime.enabled;
        m.ucTimeLiveMode           = ucTime.live;
        m.ucTimeLiveWindowSecs     = ucTime.liveWindowSecs;
        m.minUcTimeSec             = ucTime.minSec;
        m.maxUcTimeSec             = ucTime.maxSec;
        m.filterActive
          = !(activeFilterState == FilterState{}) || ucTime.enabled || !searchText.empty();
        m.searchText = searchText;
        m.searchNeedleLower
          = displayedFilter.searchRegex ? std::string{} : displayedFilter.searchNeedle;

        if(m.seenDisplayVersion != displayVersion) {
            m.seenDisplayVersion = displayVersion;
            m.entries            = filteredEntries.snapshot();
            if(m.fileNamesById.size() != fileNamesById.size()) { m.fileNamesById = fileNamesById; }
            if(m.functionNamesById.size() != functionNamesById.size()) {
                m.functionNamesById      = functionNamesById;
                m.functionSignaturesById = functionSignaturesById;
            }
        }

        if(m.moduleNamesById.size() != moduleNamesById.size()) {
            m.moduleNamesById = moduleNamesById;
        }

        if(m.seenLocationsVersion != locationsVersion) {
            // labels contain live counts, so rebuilding on every entry would be O(N)
            // per frame again; new locations rebuild immediately, count updates are
            // throttled
            auto const now         = uc_log::printer::Clock::now();
            bool const newLocation = m.locationList.size() != allSourceLocations.size();
            if(newLocation || now - m.lastLocationRebuild > locationLabelRefresh) {
                m.seenLocationsVersion = locationsVersion;
                m.lastLocationRebuild  = now;
                m.locationList.assign(allSourceLocations.begin(), allSourceLocations.end());
                m.locationLabels.clear();
                m.locationLabels.reserve(m.locationList.size());
                for(auto const& [location, count] : m.locationList) {
                    m.locationLabels.push_back(
                      fmt::format("{}:{} -> {}", location.first, location.second, count));
                }
            }
        }
    }

    // ---------- metrics ----------

    std::size_t metricCount() const {
        std::scoped_lock<std::mutex> const lock{mutex};
        return metricEntries.size();
    }

    std::vector<MetricInfo> metricInfos() const {
        std::scoped_lock<std::mutex> const lock{mutex};
        std::vector<MetricInfo>            infos;
        infos.reserve(metricEntries.size());
        for(auto const& [info, values] : metricEntries) { infos.push_back(info); }
        return infos;
    }

    /// f(values) under the lock for a known metric; false: not known (f not called)
    template<typename F>
    bool withMetric(MetricInfo const& metric,
                    F&&               f) const {
        std::scoped_lock<std::mutex> const lock{mutex};
        auto const                         it = metricEntries.find(metric);
        if(it == metricEntries.end()) { return false; }
        std::forward<F>(f)(std::as_const(it->second));
        return true;
    }

    void clearMetrics() {
        std::scoped_lock<std::mutex> const lock{mutex};
        metricEntries.clear();
    }

    /// the metric stays known, its values go
    void clearMetric(MetricInfo const& metric) {
        std::scoped_lock<std::mutex> const lock{mutex};
        if(auto const it = metricEntries.find(metric); it != metricEntries.end()) {
            it->second.clear();
        }
    }

    // ---------- state (the status line, tests) ----------

    /// lock-free: read by a renderer every frame
    RefilterProgress refilterProgress() const {
        return {.inProgress = refilterInProgress.load(),
                .scanned    = refilterScanned.load(),
                .total      = refilterTotal.load()};
    }

    Counts counts() const {
        std::scoped_lock<std::mutex> const lock{mutex};
        return countsLocked();
    }

    /// no refilter running, requested or waiting for its deadline: the filtered view is the filter's
    bool settled() const {
        std::scoped_lock<std::mutex> const lock{mutex};
        return !refilterInProgress.load() && !refilterDeadline
            && refilterGeneration.load() == displayedGeneration;
    }

    /// f(counts, all lines, filtered lines) under the lock, one consistent view (tests check the views' structure)
    template<typename F>
    void withLines(F&& f) const {
        std::scoped_lock<std::mutex> const lock{mutex};
        std::forward<F>(f)(countsLocked(), allEntries, filteredEntries);
    }

    /// whether the store's lock can be taken right now (a test: a refilter scans without it)
    bool lockIsFree() const {
        return std::unique_lock<std::mutex>{mutex, std::try_to_lock}.owns_lock();
    }

private:
    Counts countsLocked() const {
        return {.lines                = allEntries.size(),
                .filteredLines        = filteredEntries.size(),
                .originalLogs         = originalLogCount,
                .filteredOriginalLogs = filteredOriginalLogCount,
                .trimmedLogs          = trimmedLogCount,
                .addedLogs            = nextMultilineGroupId};
    }

    void updateLogRateStatisticsLocked() {
        auto const now = std::chrono::system_clock::now();
        auto const elapsed
          = std::chrono::duration_cast<std::chrono::seconds>(now - logStats.lastLogRateUpdate);

        if(elapsed.count() >= 1) {
            logStats.peakLogsPerSecond
              = std::max(logStats.logsInCurrentSecond, logStats.peakLogsPerSecond);
            logStats.logsInCurrentSecond = 0;
            logStats.lastLogRateUpdate   = now;
        }

        ++logStats.logsInCurrentSecond;
    }

    void enforceMaxLogEntriesLocked() {
        if(allEntries.size() <= maxLogEntries) { return; }
        std::size_t trim = allEntries.size() - maxLogEntries + trimSlack;
        // Never split a multiline group: advance to the next group start.
        while(trim < allEntries.size() && !allEntries[trim].startsGroup()) { ++trim; }

        std::size_t removedGroups = 0;
        for(std::size_t i = 0; i != trim; ++i) {
            if(allEntries[i].startsGroup()) { ++removedGroups; }
        }
        allEntries.eraseFront(trim);
        originalLogCount -= removedGroups;
        trimmedLogCount += removedGroups;

        // filteredEntries is ordered by groupId; drop the same prefix there
        std::size_t const minGroupId
          = allEntries.empty() ? nextMultilineGroupId + 1 : allEntries.front().multilineGroupId();
        std::size_t lo = 0;
        std::size_t hi = filteredEntries.size();
        while(lo < hi) {
            auto const mid = lo + (hi - lo) / 2;
            if(filteredEntries[mid].multilineGroupId() < minGroupId) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        std::size_t removedFilteredGroups = 0;
        for(std::size_t i = 0; i != lo; ++i) {
            if(filteredEntries[i].startsGroup()) { ++removedFilteredGroups; }
        }
        filteredEntries.eraseFront(lo);
        filteredOriginalLogCount -= removedFilteredGroups;

        // the front-erase invalidated worker snapshots' index alignment; force a
        // mid-scan worker to restart
        if(refilterInProgress) {
            refilterGeneration.store(refilterGeneration.load(std::memory_order_relaxed) + 1,
                                     std::memory_order_relaxed);
            refilterCv.notify_one();
        }
    }

    void refilterWorker(std::stop_token const& stoken) {
        std::unique_lock<std::mutex> lock{mutex};
        EntryChunkList               result;
        while(true) {
            if(!refilterCv.wait(lock,
                                stoken,
                                [&] {
                                    return refilterGeneration.load(std::memory_order_relaxed)
                                        != displayedGeneration;
                                }))
            {
                return;
            }
            auto const gen    = refilterGeneration.load(std::memory_order_relaxed);
            auto       filter = pendingFilter;
            auto const snap   = allEntries.snapshot();
            refilterTotal     = snap.size();
            refilterScanned   = 0;

            // The scan runs entirely without the lock: the snapshot is stable against
            // appends, and any erase/clear/trim bumps the generation, which is checked
            // per chunk and again before committing.
            lock.unlock();

            std::size_t passingGroups      = 0;
            bool        currentGroupPasses = false;
            bool        cancelled          = false;
            std::size_t sinceRedraw        = 0;
            for(std::size_t i = 0; i != snap.size(); ++i) {
                auto const& e = snap[i];
                if(e.startsGroup()) {
                    currentGroupPasses = filter.passes(e);
                    if(currentGroupPasses) { ++passingGroups; }
                }
                if(currentGroupPasses) { result.push_back(e); }
                if((i % refilterChunk) == 0 && i != 0) {
                    refilterScanned = i;
                    if(++sinceRedraw >= 4) {
                        sinceRedraw = 0;
                        if(requestRedraw) { requestRedraw(); }
                    }
                    if(stoken.stop_requested()
                       || refilterGeneration.load(std::memory_order_relaxed) != gen)
                    {
                        cancelled = true;
                        break;
                    }
                }
            }
            refilterScanned = snap.size();

            lock.lock();
            if(stoken.stop_requested()) { return; }
            if(!cancelled && refilterGeneration.load(std::memory_order_relaxed) == gen) {
                // modules first seen during the scan went to pendingFilter, not this copy
                for(std::uint32_t id = 0; id < moduleNamesById.size(); ++id) {
                    filter.noteModule(id, moduleNamesById[id]);
                }
                // Catch-up: entries appended by addEntry during the scan. Appends never
                // shift indices and erases would have bumped the generation, so
                // [snap.size(), allEntries.size()) is exactly the appended suffix.
                for(std::size_t j = snap.size(); j != allEntries.size(); ++j) {
                    auto const& e = allEntries[j];
                    if(e.startsGroup()) {
                        currentGroupPasses = filter.passes(e);
                        if(currentGroupPasses) { ++passingGroups; }
                    }
                    if(currentGroupPasses) { result.push_back(e); }
                }
                std::swap(filteredEntries, result);
                filteredOriginalLogCount = passingGroups;
                displayedFilter          = filter;
                displayedGeneration      = gen;
                refilterInProgress       = false;
                ++displayVersion;
                if(requestRedraw) { requestRedraw(); }
            }
            lock.unlock();
            result = EntryChunkList{};   // free the old/cancelled list outside the lock
            lock.lock();
        }
    }

    // Declared last so it is destroyed (stopped + joined) before any state it uses.
    std::jthread refilterThread{[this](std::stop_token const& stoken) { refilterWorker(stoken); }};
};

}   // namespace uc_log::printer
