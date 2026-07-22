#pragma once

#include "uc_log/FTXUI_Utils.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/metric_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace uc_log { namespace FTXUIGui {

    enum class LineType : std::uint8_t {
        SingleLine,   // Complete log on one line
        First,        // First line of multiline log
        Middle,       // Middle continuation line
        Last          // Last line of multiline log
    };

    // Immutable data shared by all display lines of one original log message. File and
    // function names live in the store's interning pools (referenced by id), so millions
    // of entries do not repeat the same strings.
    struct EntryCommon {
        std::chrono::system_clock::time_point recvTime;
        uc_log::detail::LogEntry::UcTime      ucTime;
        std::uint64_t                         locationKey;   // fileId << 32 | line
        std::size_t                           multilineGroupId;
        std::uint32_t                         functionId;
        std::uint8_t                          channel;
        uc_log::LogLevel                      logLevel;
        bool                                  parsedOk;
        std::string                           message;   // full message, '\n' separated
    };

    // One display line: a shared header plus a slice of the shared message.
    struct GuiLogEntry {
        std::shared_ptr<EntryCommon const> common;
        std::uint32_t                      lineOffset{0};
        std::uint32_t                      lineLength{0};
        LineType                           lineType{LineType::SingleLine};

        std::string_view lineText() const {
            return std::string_view{common->message}.substr(lineOffset, lineLength);
        }

        std::size_t multilineGroupId() const { return common->multilineGroupId; }

        bool startsGroup() const {
            return lineType == LineType::SingleLine || lineType == LineType::First;
        }
    };

    // Append-only chunked storage. Chunks have fixed capacity, so appends never move
    // existing elements: a copy of the chunk pointers plus a size is a stable view that
    // can be read without any lock while the owner keeps appending. Trimming only drops
    // references; live snapshots keep trimmed chunks alive until they let go.
    struct EntryChunkList {
        static constexpr std::size_t ChunkSize = 64 * 1024;

        using Chunk = std::vector<GuiLogEntry>;

        std::vector<std::shared_ptr<Chunk>> chunks;
        std::size_t                         frontOffset{0};
        std::size_t                         totalSize{0};

        struct Snapshot {
            std::vector<std::shared_ptr<Chunk>> chunks;
            std::size_t                         frontOffset{0};
            std::size_t                         totalSize{0};

            std::size_t size() const { return totalSize; }

            bool empty() const { return totalSize == 0; }

            GuiLogEntry const& operator[](std::size_t i) const {
                auto const j = i + frontOffset;
                return (*chunks[j / ChunkSize])[j % ChunkSize];
            }

            struct iterator {
                using iterator_category = std::random_access_iterator_tag;
                using iterator_concept  = std::random_access_iterator_tag;
                using value_type        = GuiLogEntry;
                using difference_type   = std::ptrdiff_t;
                using reference         = GuiLogEntry const&;
                using pointer           = GuiLogEntry const*;

                Snapshot const* snapshot{nullptr};
                std::size_t     index{0};

                reference operator*() const { return (*snapshot)[index]; }

                pointer operator->() const { return &(*snapshot)[index]; }

                reference operator[](difference_type n) const {
                    return (*snapshot)[index + static_cast<std::size_t>(n)];
                }

                iterator& operator++() {
                    ++index;
                    return *this;
                }

                iterator operator++(int) {
                    auto tmp = *this;
                    ++index;
                    return tmp;
                }

                iterator& operator--() {
                    --index;
                    return *this;
                }

                iterator operator--(int) {
                    auto tmp = *this;
                    --index;
                    return tmp;
                }

                iterator& operator+=(difference_type n) {
                    index = static_cast<std::size_t>(static_cast<difference_type>(index) + n);
                    return *this;
                }

                iterator& operator-=(difference_type n) { return *this += -n; }

                friend iterator operator+(iterator        it,
                                          difference_type n) {
                    return it += n;
                }

                friend iterator operator+(difference_type n,
                                          iterator        it) {
                    return it += n;
                }

                friend iterator operator-(iterator        it,
                                          difference_type n) {
                    return it -= n;
                }

                friend difference_type operator-(iterator const& lhs,
                                                 iterator const& rhs) {
                    return static_cast<difference_type>(lhs.index)
                         - static_cast<difference_type>(rhs.index);
                }

                friend bool operator==(iterator const& lhs,
                                       iterator const& rhs) {
                    return lhs.index == rhs.index;
                }

                friend auto operator<=>(iterator const& lhs,
                                        iterator const& rhs) {
                    return lhs.index <=> rhs.index;
                }
            };

            iterator begin() const { return {this, 0}; }

            iterator end() const { return {this, totalSize}; }
        };

        Snapshot snapshot() const { return Snapshot{chunks, frontOffset, totalSize}; }

        std::size_t size() const { return totalSize; }

        bool empty() const { return totalSize == 0; }

        GuiLogEntry const& operator[](std::size_t i) const {
            auto const j = i + frontOffset;
            return (*chunks[j / ChunkSize])[j % ChunkSize];
        }

        GuiLogEntry const& front() const { return (*this)[0]; }

        void push_back(GuiLogEntry entry) {
            if(chunks.empty() || chunks.back()->size() == ChunkSize) {
                auto chunk = std::make_shared<Chunk>();
                chunk->reserve(ChunkSize);
                chunks.push_back(std::move(chunk));
            }
            chunks.back()->push_back(std::move(entry));
            ++totalSize;
        }

        void eraseFront(std::size_t n) {
            n = std::min(n, totalSize);
            frontOffset += n;
            totalSize -= n;
            // only a full first chunk can be dropped: a partial one is still the append
            // target and the index arithmetic relies on non-last chunks being full
            while(!chunks.empty() && chunks.front()->size() == ChunkSize
                  && frontOffset >= ChunkSize)
            {
                chunks.erase(chunks.begin());
                frontOffset -= ChunkSize;
            }
        }

        void clear() {
            chunks.clear();
            frontOffset = 0;
            totalSize   = 0;
        }
    };

    struct FilterState {
        std::set<uc_log::LogLevel> enabledLogLevels;
        std::set<std::size_t>      enabledChannels;
        std::set<SourceLocation>   includedLocations;
        std::set<SourceLocation>   excludedLocations;

        bool operator==(FilterState const&) const = default;
    };

    // FilterState compiled to integer form: location strings interned to keys
    // (fileId << 32 | line), sets to bitmasks/sorted vectors. passes() does no
    // allocation and no string comparison, so scanning millions of entries is cheap.
    struct CompiledFilter {
        static constexpr std::uint64_t FileMask = 0xFFFF'FFFF'0000'0000ULL;

        bool                       levelFilterActive{false};
        bool                       channelFilterActive{false};
        std::uint8_t               levelMask{0};
        std::uint8_t               channelMask{0};
        std::vector<std::uint64_t> includedKeys;   // sorted; file-wide keys have line == 0
        std::vector<std::uint64_t> excludedKeys;   // sorted
        bool                       ucTimeEnabled{false};
        double                     minUcTimeSec{0.0};
        double                     maxUcTimeSec{std::numeric_limits<double>::infinity()};

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

        bool passes(GuiLogEntry const& entry) const {
            auto const& common = *entry.common;
            if(levelFilterActive) {
                auto const bit = static_cast<std::uint8_t>(common.logLevel);
                if(bit >= 8 || (levelMask & (1U << bit)) == 0) { return false; }
            }
            if(channelFilterActive) {
                auto const channel = common.channel;
                if(channel >= 8 || (channelMask & (1U << channel)) == 0) { return false; }
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
            auto const fileKey  = entryKey & FileMask;

            bool const hasExclusions = !excludedKeys.empty();
            bool const hasInclusions = !includedKeys.empty();

            if(hasExclusions && std::ranges::binary_search(excludedKeys, entryKey)) {
                return false;
            }
            if(hasInclusions && std::ranges::binary_search(includedKeys, entryKey)) { return true; }
            if(hasExclusions && std::ranges::binary_search(excludedKeys, fileKey)) { return false; }
            if(hasExclusions) { return true; }
            if(hasInclusions) { return std::ranges::binary_search(includedKeys, fileKey); }
            return true;
        }
    };

    // Owns all log-derived data behind its own mutex so the producer (add) and the
    // refilter worker never contend with whole-frame UI rendering. The UI reads through a
    // per-frame Mirror refresh instead of locking inside renderers.
    //
    // Lock order: the UI mutex (if any) is always taken before this mutex, never after.
    // requestRedraw must be safe to call from any thread and must not take UI locks.
    //
    // Invariant: allEntries may only be appended to while a scan generation is live; any
    // erase/clear/trim must bump refilterGeneration so the worker abandons its snapshot.
    struct GuiEntryStore {
        struct LogStats {
            std::size_t                           peakLogsPerSecond{0};
            std::chrono::system_clock::time_point lastLogRateUpdate{
              std::chrono::system_clock::now()};
            std::size_t                                     logsInCurrentSecond{0};
            std::optional<uc_log::detail::LogEntry::UcTime> lastUcTime;
            std::size_t                                     detectedResetCount{0};
            std::size_t                                     parseFailureCount{0};
        };

        std::mutex mutex;

        // interning pools, guarded by mutex; the ById vectors only ever grow
        std::map<std::string, std::uint32_t, std::less<>> fileIds;
        std::vector<std::string>                          fileNamesById;
        std::map<std::string, std::uint32_t, std::less<>> functionIds;
        std::vector<std::string>                          functionNamesById;

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

        // uc-time data range and live-window state (data-derived, written by addEntry)
        double ucTimeDataMin{std::numeric_limits<double>::infinity()};
        double ucTimeDataMax{-std::numeric_limits<double>::infinity()};
        bool   ucTimeFilterEnabled{false};
        bool   ucTimeLiveMode{false};
        double ucTimeLiveWindowSecs{10.0};
        double minUcTimeSec{0.0};
        double maxUcTimeSec{std::numeric_limits<double>::infinity()};

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
        std::optional<std::chrono::steady_clock::time_point> refilterDeadline;
        std::condition_variable_any                          refilterCv;

        // set once by the owner before data flows; must be cheap and lock-free towards UI
        std::function<void()> requestRedraw;

        // injectable for tests; production uses the GUI_Constants defaults
        std::size_t maxLogEntries{GUI_Constants::MaxLogEntries};
        std::size_t trimSlack{GUI_Constants::TrimSlack};

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

        std::uint32_t internFunctionLocked(std::string_view function) {
            auto it = functionIds.find(function);
            if(it == functionIds.end()) {
                it = functionIds
                       .emplace(std::string{function},
                                static_cast<std::uint32_t>(functionIds.size()))
                       .first;
                functionNamesById.emplace_back(function);
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
            f.ucTimeEnabled = ucTimeFilterEnabled;
            f.minUcTimeSec  = minUcTimeSec;
            f.maxUcTimeSec  = maxUcTimeSec;
            if(!searchText.empty()) {
                f.searchActive = true;
                if(searchText.starts_with("re:")) {
                    try {
                        f.searchRegex = std::make_shared<std::regex const>(
                          searchText.substr(3),
                          std::regex::icase | std::regex::optimize);
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

        void setSearchText(std::string const& text) {
            std::lock_guard<std::mutex> const lock{mutex};
            if(searchText == text) { return; }
            searchText       = text;
            refilterDeadline = std::chrono::steady_clock::now() + GUI_Constants::FilterDebounce;
        }

        void requestRefilterLocked() {
            pendingFilter = compileFilterLocked();
            refilterDeadline.reset();
            refilterGeneration.store(refilterGeneration.load(std::memory_order_relaxed) + 1,
                                     std::memory_order_relaxed);
            refilterInProgress = true;
            refilterCv.notify_one();
        }

        void requestRefilter() {
            std::lock_guard<std::mutex> const lock{mutex};
            requestRefilterLocked();
        }

        void setFilterState(FilterState const& fs) {
            std::lock_guard<std::mutex> const lock{mutex};
            if(activeFilterState == fs) { return; }
            activeFilterState = fs;
            requestRefilterLocked();
        }

        // Trailing-edge debounce for text inputs: each keystroke pushes the deadline.
        void scheduleRefilterDebounced() {
            std::lock_guard<std::mutex> const lock{mutex};
            refilterDeadline = std::chrono::steady_clock::now() + GUI_Constants::FilterDebounce;
        }

        // Leading-edge throttle for continuous triggers (uc-time live mode): a pending
        // deadline is never pushed back, so a steady stream cannot starve the refilter.
        void scheduleRefilterThrottledLocked() {
            if(!refilterDeadline) {
                refilterDeadline
                  = std::chrono::steady_clock::now() + GUI_Constants::LiveRefilterThrottle;
            }
        }

        // called periodically from the UI loop; fires the deferred refilter when due
        void pollRefilterDeadline() {
            std::lock_guard<std::mutex> const lock{mutex};
            if(refilterDeadline && std::chrono::steady_clock::now() >= *refilterDeadline) {
                requestRefilterLocked();
            }
        }

        // ---------- uc-time filter state (called from UI callbacks) ----------

        template<typename F>
        void updateUcTime(F&& f) {
            std::lock_guard<std::mutex> const lock{mutex};
            f(*this);
        }

        // ---------- ingest (producer thread) ----------

        void addEntry(std::chrono::system_clock::time_point recvTime,
                      uc_log::detail::LogEntry const&       entry) {
            {
                std::lock_guard<std::mutex> const lock{mutex};

                ++originalLogCount;
                updateLogRateStatisticsLocked();
                if(!entry.parsedOk) { ++logStats.parseFailureCount; }

                // Detect target reset: ucTime jumping backwards by more than a second
                if(logStats.lastUcTime.has_value() && entry.ucTime < logStats.lastUcTime.value()
                   && logStats.lastUcTime.value().time - entry.ucTime.time
                        > std::chrono::seconds{1})
                {
                    ++logStats.detectedResetCount;
                }
                logStats.lastUcTime = entry.ucTime;

                {
                    auto const ucSecs = std::chrono::duration<double>(entry.ucTime.time).count();
                    ucTimeDataMin     = std::min(ucTimeDataMin, ucSecs);
                    bool const newMax = ucSecs > ucTimeDataMax;
                    ucTimeDataMax     = std::max(ucTimeDataMax, ucSecs);
                    if(ucTimeLiveMode && newMax) {
                        minUcTimeSec        = std::max(0.0, ucTimeDataMax - ucTimeLiveWindowSecs);
                        maxUcTimeSec        = std::numeric_limits<double>::infinity();
                        ucTimeFilterEnabled = true;
                        // Judge new entries against the fresh window immediately; sliding
                        // old entries out of view is done by a throttled background rescan.
                        displayedFilter.ucTimeEnabled = true;
                        displayedFilter.minUcTimeSec  = minUcTimeSec;
                        displayedFilter.maxUcTimeSec  = maxUcTimeSec;
                        scheduleRefilterThrottledLocked();
                    }
                }

                for(auto const& metric : uc_log::extractMetrics(recvTime, entry)) {
                    auto& values = metricEntries[metric.first];
                    values.push_back(metric.second);
                    // bounded per key: drop the oldest quarter in one amortized batch
                    if(values.size() > GUI_Constants::MaxMetricEntriesPerKey) {
                        values.erase(values.begin(),
                                     values.begin()
                                       + static_cast<std::ptrdiff_t>(
                                         GUI_Constants::MaxMetricEntriesPerKey / 4));
                    }
                }

                allSourceLocations[SourceLocation{entry.fileName, entry.line}]++;
                ++locationsVersion;

                auto common              = std::make_shared<EntryCommon>();
                common->recvTime         = recvTime;
                common->ucTime           = entry.ucTime;
                common->locationKey      = internLocationKeyLocked(entry.fileName, entry.line);
                common->multilineGroupId = ++nextMultilineGroupId;
                common->functionId       = internFunctionLocked(entry.functionName);
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
                    GuiLogEntry line{shared,
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
                    pushLine(0, msg.size(), LineType::SingleLine);
                } else {
                    std::size_t offset = 0;
                    while(true) {
                        auto const nl   = msg.find('\n', offset);
                        bool const last = nl == std::string_view::npos;
                        auto const len  = last ? msg.size() - offset : nl - offset;
                        auto const type = offset == 0 ? LineType::First
                                        : last        ? LineType::Last
                                                      : LineType::Middle;
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
            std::lock_guard<std::mutex> const lock{mutex};
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
            std::lock_guard<std::mutex> const lock{mutex};
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
            std::lock_guard<std::mutex> const lock{mutex};
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

            std::vector<std::pair<SourceLocation, std::size_t>> locationList;
            std::vector<std::string>                            locationLabels;

            std::uint64_t seenDisplayVersion{std::numeric_limits<std::uint64_t>::max()};
            std::uint64_t seenLocationsVersion{std::numeric_limits<std::uint64_t>::max()};
            std::chrono::steady_clock::time_point lastLocationRebuild{};

            std::string fileNameOf(std::uint64_t locationKey) const {
                auto const id = static_cast<std::size_t>(locationKey >> 32U);
                return id < fileNamesById.size() ? fileNamesById[id] : std::string{"?"};
            }

            static std::size_t lineOf(std::uint64_t locationKey) {
                return static_cast<std::size_t>(locationKey & 0xFFFF'FFFFULL);
            }

            std::string functionNameOf(std::uint32_t id) const {
                return id < functionNamesById.size() ? functionNamesById[id] : std::string{"?"};
            }
        };

        // Called once per UI frame, before rendering: everything the renderers touch is
        // copied out here so no renderer ever takes the store mutex.
        void refreshMirror(Mirror& m) {
            std::lock_guard<std::mutex> const lock{mutex};
            m.originalLogCount         = originalLogCount;
            m.filteredOriginalLogCount = filteredOriginalLogCount;
            m.trimmedLogCount          = trimmedLogCount;
            m.logStats                 = logStats;
            m.ucTimeDataMin            = ucTimeDataMin;
            m.ucTimeDataMax            = ucTimeDataMax;
            m.ucTimeFilterEnabled      = ucTimeFilterEnabled;
            m.ucTimeLiveMode           = ucTimeLiveMode;
            m.ucTimeLiveWindowSecs     = ucTimeLiveWindowSecs;
            m.minUcTimeSec             = minUcTimeSec;
            m.maxUcTimeSec             = maxUcTimeSec;
            m.filterActive
              = !(activeFilterState == FilterState{}) || ucTimeFilterEnabled || !searchText.empty();
            m.searchText = searchText;
            m.searchNeedleLower
              = displayedFilter.searchRegex ? std::string{} : displayedFilter.searchNeedle;

            if(m.seenDisplayVersion != displayVersion) {
                m.seenDisplayVersion = displayVersion;
                m.entries            = filteredEntries.snapshot();
                if(m.fileNamesById.size() != fileNamesById.size()) {
                    m.fileNamesById = fileNamesById;
                }
                if(m.functionNamesById.size() != functionNamesById.size()) {
                    m.functionNamesById = functionNamesById;
                }
            }

            if(m.seenLocationsVersion != locationsVersion) {
                // labels contain live counts, so rebuilding on every entry would be O(N)
                // per frame again; new locations rebuild immediately, count updates are
                // throttled
                auto const now         = std::chrono::steady_clock::now();
                bool const newLocation = m.locationList.size() != allSourceLocations.size();
                if(newLocation || now - m.lastLocationRebuild > GUI_Constants::LocationLabelRefresh)
                {
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

    private:
        void updateLogRateStatisticsLocked() {
            auto const now = std::chrono::system_clock::now();
            auto const elapsed
              = std::chrono::duration_cast<std::chrono::seconds>(now - logStats.lastLogRateUpdate);

            if(elapsed.count() >= 1) {
                if(logStats.logsInCurrentSecond > logStats.peakLogsPerSecond) {
                    logStats.peakLogsPerSecond = logStats.logsInCurrentSecond;
                }
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
            std::size_t const minGroupId = allEntries.empty()
                                           ? nextMultilineGroupId + 1
                                           : allEntries.front().multilineGroupId();
            std::size_t       lo         = 0;
            std::size_t       hi         = filteredEntries.size();
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

        void refilterWorker(std::stop_token stoken) {
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
                auto const filter = pendingFilter;
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
                    if((i % GUI_Constants::RefilterChunk) == 0 && i != 0) {
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

    public:
        // Declared last so it is destroyed (stopped + joined) before any state it uses.
        std::jthread refilterThread{[this](std::stop_token stoken) { refilterWorker(stoken); }};
    };

}}   // namespace uc_log::FTXUIGui
