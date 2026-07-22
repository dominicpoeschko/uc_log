#pragma once

#include "uc_log/FTXUI_Utils.hpp"
#include "uc_log/detail/BuildRunner.hpp"
#include "uc_log/detail/DuplexChannelInfo.hpp"
#include "uc_log/detail/GuiEntryStore.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/LogFormat.hpp"
#include "uc_log/detail/TcpPortStatus.hpp"
#include "uc_log/metric_utils.hpp"
#include "uc_log/theme.hpp"

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wredundant-decls"
    #pragma GCC diagnostic ignored "-Woverloaded-virtual"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
    #pragma GCC diagnostic ignored "-Wshadow"
    #pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
    #pragma clang diagnostic ignored "-Wunused-parameter"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
    #pragma clang diagnostic ignored "-Wreserved-macro-identifier"
    #pragma clang diagnostic ignored "-Wsuggest-override"
    #pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
    #pragma clang diagnostic ignored "-Wmissing-noreturn"
    #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma clang diagnostic ignored "-Wglobal-constructors"
    #pragma clang diagnostic ignored "-Wdocumentation"
    #pragma clang diagnostic ignored "-Wsuggest-destructor-override"
    #pragma clang diagnostic ignored "-Wshorten-64-to-32"
    #pragma clang diagnostic ignored "-Wswitch-default"
    #pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
    #pragma clang diagnostic ignored "-Wswitch-enum"
    #pragma clang diagnostic ignored "-Wimplicit-fallthrough"
    #pragma clang diagnostic ignored "-Wexit-time-destructors"
    #pragma clang diagnostic ignored "-Wextra-semi"
    #pragma clang diagnostic ignored "-Wextra-semi-stmt"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wnewline-eof"
    #pragma clang diagnostic ignored "-Wredundant-parens"
    #pragma clang diagnostic ignored "-Winconsistent-missing-destructor-override"
    #pragma clang diagnostic ignored "-Wundef"
#endif

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif

#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fmt/std.h>
#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

namespace glz {
/// Registers every non-`std::byte` enum type with glaze using
/// `enchantum`-derived names, satisfying `glaze_enum_t<T>`.
template<typename T>
    requires(std::is_enum_v<T> && !std::is_same_v<T, std::byte>)
struct meta<T> {
    static constexpr auto value = []<std::size_t... Is>(std::index_sequence<Is...>) {
        constexpr auto names  = enchantum::names<T>;
        constexpr auto values = enchantum::values<T>;
        return std::apply(
          [](auto&&... args) { return glz::enumerate(std::forward<decltype(args)>(args)...); },
          std::tuple_cat(std::make_tuple(names[Is], values[Is])...));
    }(std::make_index_sequence<enchantum::count<T>>{});
};

/// Serialises std::set<std::pair<K,V>> as a JSON array-of-arrays [[k,v],...].
/// Glaze's default treats any container of pair<string,T> as a sorted map → {}
/// which then fails to round-trip.  Direct to/from specialisations bypass that.
template<typename K, typename V>
struct to<JSON, std::set<std::pair<K, V>>> {
    template<auto Opts,
             class B>
    static void op(std::set<std::pair<K,
                                      V>> const& value,
                   is_context auto&&             ctx,
                   B&&                           b,
                   auto&                         ix) {
        dump('[', b, ix);
        bool first_elem = true;
        for(auto const& [k, v] : value) {
            if(!first_elem) { dump(',', b, ix); }
            first_elem = false;
            dump('[', b, ix);
            serialize<JSON>::op<Opts>(k, ctx, b, ix);
            dump(',', b, ix);
            serialize<JSON>::op<Opts>(v, ctx, b, ix);
            dump(']', b, ix);
        }
        dump(']', b, ix);
    }
};

/// Reads a JSON array-of-arrays [[k,v],...] back into std::set<std::pair<K,V>>.
/// Delegates to glaze's built-in vector<tuple> reader (tuples are always arrays).
template<typename K, typename V>
struct from<JSON, std::set<std::pair<K, V>>> {
    template<auto Opts>
    static void op(std::set<std::pair<K,
                                      V>>& value,
                   is_context auto&&       ctx,
                   auto&&                  it,
                   auto&&                  end) {
        std::vector<std::tuple<K, V>> tmp;
        from<JSON, std::vector<std::tuple<K, V>>>::template op<Opts>(tmp, ctx, it, end);
        for(auto& [k, v] : tmp) { value.emplace(std::move(k), std::move(v)); }
    }
};
}   // namespace glz

namespace uc_log { namespace FTXUIGui {

    struct Gui {
        Gui() {
            store.requestRedraw         = [this]() { requestRedrawFromAnywhere(); };
            buildRunner.requestRedraw   = [this]() { requestRedrawFromAnywhere(); };
            buildRunner.onBuildFinished = [this](bool success) {
                std::lock_guard<std::mutex> const lock{mutex};
                if(success) {
                    ++statistics.successfulBuilds;
                } else {
                    ++statistics.failedBuilds;
                }
            };
        }

        ~Gui() = default;

        Gui(Gui const&)            = delete;
        Gui& operator=(Gui const&) = delete;

        Gui(Gui&&)            = delete;
        Gui& operator=(Gui&&) = delete;

    private:
        // LineType, GuiLogEntry, FilterState, CompiledFilter and all log-entry storage
        // moved to detail/GuiEntryStore.hpp: the store owns them behind its own mutex.

        struct MessageEntry {
            enum class Level : std::uint8_t { Fatal, Error, Status, ToolError, ToolStatus };

            Level                                 level;
            std::chrono::system_clock::time_point time;
            std::string                           message;
        };

        enum class OutlierMethod : std::uint8_t {
            IQRTukey,           // cutoff = Q3 + k × (Q3 - Q1)
            TopNPercent,        // exclude the top N% most frequent locations
            AbsoluteThreshold   // exclude locations with count > N
        };

        struct OutlierResult {
            std::size_t                 cutoff{0};
            std::size_t                 q1{0};
            std::size_t                 median{0};
            std::size_t                 q3{0};
            std::vector<SourceLocation> wouldExclude;
            bool                        valid{false};
        };

        struct Statistics {
            std::chrono::system_clock::time_point sessionStartTime{
              std::chrono::system_clock::now()};

            // JLink connection statistics
            std::size_t jlinkReconnectionCount{0};
            std::size_t jlinkDisconnectionCount{0};
            bool        lastJLinkState{false};

            // Build statistics
            std::size_t totalBuildsStarted{0};
            std::size_t successfulBuilds{0};
            std::size_t failedBuilds{0};
            std::size_t cancelledBuilds{0};

            // Target control statistics
            std::size_t flashCount{0};
            std::size_t resetRequestCount{0};

            // Log-derived statistics (rate, reset detection, parse failures) live in
            // GuiEntryStore::LogStats, written by the producer under the store mutex.

            // Data statistics
            std::size_t maxBytesRead{0};
            std::size_t maxOverflowCount{0};
        };

        std::mutex mutex;

        // Actions posted by button callbacks and drained after loop.RunOnce() releases gui.mutex.
        std::vector<std::function<void()>> pendingActions;

        // Cross-thread redraw requests: coalesced via redrawPending, screenPointer guarded
        // by the leaf mutex screenMutex (never take another lock while holding it).
        std::mutex                screenMutex;
        ftxui::ScreenInteractive* screenPointer = nullptr;   // guarded by screenMutex
        std::atomic<bool>         redrawPending{false};

        // All log-entry data (entries, filter, metrics, interning pools) lives here behind
        // its own mutex; renderers read only from the per-frame mirror below.
        GuiEntryStore         store;
        GuiEntryStore::Mirror display;

        FTXUIGui::MetricPlotWidget metricPlotWidget;
        // UI-side copy of the plotted series, filled under the store mutex per frame
        std::vector<MetricEntry> plotDataBuffer;

        FilterState editedFilterState;

        // UC time filter input strings (the numeric state lives in the store)
        std::string      minUcTimeStr;
        std::string      maxUcTimeStr;
        ftxui::Component ucTimeMinInput;
        ftxui::Component ucTimeMaxInput;

        std::string      ucTimeLiveWindowStr{"10"};
        ftxui::Component ucTimeLiveWindowInput;

        // Logs-tab text search ('/' shows and focuses it) and jump-to-uc-time.
        // searchRowShown is an explicit flag instead of Focused() checks: querying
        // Focused() from the Maybe's show-lambda recurses (Focusable -> show -> Focused)
        bool                     searchRowShown{false};
        std::string              searchStr;
        ftxui::Component         searchInput;
        std::string              jumpToStr;
        ftxui::Component         jumpToInput;
        std::function<void(int)> logScrollerJump;
        int                      exportFormatSelection{0};   // 0 = .rttlog, 1 = .txt

        bool showSysTime{true};
        bool showFunctionName{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showChannel{true};
        bool showLogLevel{true};
        bool showMetricString{false};
        bool showTypenameString{false};

        // Parsed-message Element cache: ANSI parsing + marker processing of a line is a
        // pure function of (message slice, metric/typename toggles), so visible rows are
        // re-parsed only when they first appear or a toggle changes. The stored shared_ptr
        // keeps the entry alive, which makes address-reuse false hits impossible.
        static constexpr std::size_t MaxRenderCacheEntries = 16 * 1024;
        std::map<std::tuple<void const*, std::uint32_t, std::uint8_t>,
                 std::pair<std::shared_ptr<EntryCommon const>, ftxui::Element>>
          renderCache;

        std::uint8_t messageToggleBits() const {
            return static_cast<std::uint8_t>((showMetricString ? 1U : 0U)
                                             | (showTypenameString ? 2U : 0U));
        }

        std::size_t lastMetricCount{0};
        bool        hasLastSelectedInfo{false};
        MetricInfo  lastSelectedInfo;

        std::vector<MessageEntry> statusMessages;

        // build execution lives behind its own mutex in the runner; the UI renders from
        // the versioned snapshot below
        BuildRunner             buildRunner;
        std::vector<BuildEntry> buildOutputDisplay;
        std::uint64_t           buildOutputSeen{std::numeric_limits<std::uint64_t>::max()};

        int              selectedLocationIndex{};
        SourceLocation   selectedSourceLocation;
        std::string      locationFilterInput;
        ftxui::Component manualLocationInput;
        ftxui::Component filterConfigInput;
        ftxui::Component iqrInput;
        ftxui::Component topNInput;
        ftxui::Component absInput;

        std::string filterConfigPath{"filter.json"};
        std::string filterConfigStatus;

        std::string   noiseExcludeStatus;
        OutlierMethod outlierMethod{OutlierMethod::IQRTukey};
        int           selectedOutlierMethod{0};
        double        iqrMultiplier{1.5};
        std::string   iqrMultiplierStr{"1.5"};
        double        topNPercent{10.0};
        std::string   topNPercentStr{"10"};
        std::size_t   absoluteThreshold{100};
        std::string   absoluteThresholdStr{"100"};

        // Outlier preview memo: recomputed only when the location list or the parameters
        // change instead of a full sort per rendered frame.
        OutlierResult cachedOutlierResult;
        std::uint64_t cachedOutlierVersion{std::numeric_limits<std::uint64_t>::max()};
        OutlierMethod cachedOutlierMethod{OutlierMethod::IQRTukey};
        double        cachedOutlierIqrK{0.0};
        double        cachedOutlierTopN{0.0};
        std::size_t   cachedOutlierAbs{0};

        int selectedTab{};
        int tabCount{0};

        int selectedMetricTab{};

        // Every text input component, so hotkey handling can generically check whether
        // the user is typing (see trackInput / anyTextInputFocused).
        std::vector<ftxui::Component> trackedTextInputs;

        TcpPortStatus                      tcpPortStatus{TcpPortStatus::NotStarted};
        std::uint16_t                      tcpCurrentPort{0};
        std::string                        tcpPortInput;
        std::function<void(std::uint16_t)> onTcpPortChange;
        std::function<std::size_t()>       tcpClientCountGetter;
        ftxui::Component                   tcpPortInputComponent;

        std::function<std::vector<uc_log::detail::DuplexChannelInfo>()> duplexInfoGetter;
        std::function<void(std::size_t, std::uint16_t)>                 onDuplexPortChange;
        std::function<void(std::size_t, bool)>                          onDuplexEnable;
        std::function<void(std::uint16_t)>                              onDuplexBasePortChange;
        std::array<std::string, GUI_Constants::MaxDuplexChannels>       duplexPortInputs;
        std::array<ftxui::Component, GUI_Constants::MaxDuplexChannels>  duplexPortInputComponents;
        std::string                                                     duplexBasePortInput;
        ftxui::Component duplexBasePortInputComponent;

        LogFileStatus                           logFileStatus{LogFileStatus::NotStarted};
        std::string                             logFileCurrentPath;
        std::string                             logDirInput;
        std::function<void(std::string const&)> onLogDirChange;
        std::function<void(bool)>               onLogFileEnable;
        std::function<void(bool)>               onTcpEnable;
        bool                                    logFileEnabled{true};
        bool                                    tcpEnabled{true};
        std::string                             networkBindAddress{"127.0.0.1"};
        std::string                             bindAddressInput;
        ftxui::Component                        bindAddressInputComponent;
        // returns true when the address parsed and was applied to all sockets
        std::function<bool(std::string const&)> onNetworkBindAddressChange;
        std::string                             bindAddressStatus;
        ftxui::Component                        logDirInputComponent;

        // loopback addresses keep the target's ports off the network; anything else
        // exposes the unauthenticated duplex shell to other hosts
        static bool isLoopbackAddress(std::string_view address) {
            return address == "127.0.0.1" || address == "::1" || address == "localhost"
                || address.starts_with("127.");
        }

        std::string      exportDirInput;
        ftxui::Component exportDirInputComponent;
        std::string      lastExportPath;
        std::size_t      lastExportCount{0};
        bool             lastExportOk{false};

        Statistics statistics;

        int              selectedResetType{0};
        int              connectionTypeSelection{0};   // 0 = USB, 1 = IP
        std::string      ipAddressInput{};
        ftxui::Component ipAddressInputComponent;
        std::string      noLogTimeoutStr{"15"};
        ftxui::Component noLogTimeoutInput;

        // Cross-thread redraw request. ftxui's PostEvent is NOT thread-safe (the event
        // buffer has no internal synchronization), so producers only set this flag; the
        // UI loop posts the actual event to itself once per iteration. Latency is bounded
        // by GUI_Constants::UpdateInterval, same as the loop granularity.
        void requestRedrawFromAnywhere() { redrawPending.store(true, std::memory_order_relaxed); }

        // Register a text input (including its decorators) for the hotkey focus guard.
        ftxui::Component trackInput(ftxui::Component component) {
            trackedTextInputs.push_back(component);
            return component;
        }

        bool anyTextInputFocused() const {
            return std::ranges::any_of(trackedTextInputs,
                                       [](auto const& c) { return c && c->Focused(); });
        }

        static ftxui::ComponentDecorator numericFilter(bool allowDot) {
            return ftxui::CatchEvent([allowDot](ftxui::Event const& e) {
                if(!e.is_character()) { return false; }
                auto const& s = e.character();
                if(s.size() != 1) { return true; }
                char const c = s[0];
                return !((c >= '0' && c <= '9') || (allowDot && c == '.'));
            });
        }

        std::size_t calculatePrefixWidth() const {
            std::size_t width = 0;

            if(showSysTime) {
                width += 13;   // "HH:MM:SS.mmm "
            }

            if(showChannel) {
                width += 2;   // "C "
            }

            if(showUcTime) {
                width += 21;   // "00:00:00.000.000.000 "
            }

            if(showLogLevel) {
                width += 7;   // "level| "
            }

            return width;
        }

        // build machinery lives in detail/BuildRunner.hpp; these thin wrappers keep the
        // statistics (UI mutex domain) in sync
        void executeBuild() {
            if(buildRunner.getStatus() == BuildStatus::Running || buildRunner.thread.joinable()) {
                return;
            }
            ++statistics.totalBuildsStarted;
            buildRunner.execute();
        }

        void executeBuildAndFlash() {
            if(buildRunner.getStatus() == BuildStatus::Running || buildRunner.thread.joinable()) {
                return;
            }
            ++statistics.totalBuildsStarted;
            buildRunner.executeAndFlash();
        }

        template<typename Reader>
        void resetTargetWithStats(Reader& rttReader) {
            ++statistics.resetRequestCount;
            rttReader.resetTarget();
        }

        template<typename Reader>
        void flashWithStats(Reader& rttReader) {
            ++statistics.flashCount;
            rttReader.flash();
        }

        template<typename Reader>
        void updateJLinkStatistics(Reader& rttReader) {
            auto const rttStatus         = rttReader.getStatus();
            bool const currentJLinkState = (rttStatus.isRunning != 0);

            // Track state transitions
            if(currentJLinkState && !statistics.lastJLinkState) {
                // Transition from disconnected to connected
                ++statistics.jlinkReconnectionCount;
            } else if(!currentJLinkState && statistics.lastJLinkState) {
                // Transition from connected to disconnected
                ++statistics.jlinkDisconnectionCount;
            }

            statistics.lastJLinkState = currentJLinkState;

            // Update max values
            if(rttStatus.numBytesRead > statistics.maxBytesRead) {
                statistics.maxBytesRead = rttStatus.numBytesRead;
            }
            if(static_cast<std::size_t>(rttStatus.hostOverflowCount) > statistics.maxOverflowCount)
            {
                statistics.maxOverflowCount = static_cast<std::size_t>(rttStatus.hostOverflowCount);
            }
        }

        std::string processLogMessage(std::string_view originalMsg) const {
            std::string processedMsg{originalMsg};
            std::size_t pos = 0;

            // Process @METRIC(...) markers
            if(!showMetricString) {
                pos = 0;
                while((pos = processedMsg.find("@METRIC(", pos)) != std::string::npos) {
                    std::size_t const start_pos = pos;
                    pos += 8;

                    std::size_t const end_pos = processedMsg.find(')', pos);
                    if(end_pos == std::string::npos) { break; }

                    std::string_view const metric_content
                      = std::string_view{processedMsg}.substr(pos, end_pos - pos);

                    std::size_t const equals_pos = metric_content.find('=');
                    if(equals_pos != std::string_view::npos) {
                        std::string const value{metric_content.substr(equals_pos + 1)};
                        processedMsg.replace(start_pos, end_pos - start_pos + 1, value);
                        pos = start_pos + value.length();
                    } else {
                        pos = end_pos + 1;
                    }
                }
            }

            // Process @TYPENAME(...) markers
            pos = 0;
            while((pos = processedMsg.find("@TYPENAME(", pos)) != std::string::npos) {
                std::size_t const start_pos = pos;
                pos += 10;

                std::size_t const end_pos = processedMsg.find(')', pos);
                if(end_pos == std::string::npos) { break; }

                if(showTypenameString) {
                    // Show the typename content
                    std::string const typename_content{processedMsg.substr(pos, end_pos - pos)};
                    processedMsg.replace(start_pos, end_pos - start_pos + 1, typename_content);
                    pos = start_pos + typename_content.length();
                } else {
                    // Hide the entire @TYPENAME(...) marker
                    processedMsg.erase(start_pos, end_pos - start_pos + 1);
                    pos = start_pos;
                }
            }

            return processedMsg;
        }

        // ANSI parsing + marker processing of a line is a pure function of the message
        // slice and the metric/typename toggles: cache the resulting element so visible
        // rows are parsed once instead of every frame.
        ftxui::Element renderMessageElement(GuiLogEntry const& entry) {
            auto const key
              = std::tuple<void const*, std::uint32_t, std::uint8_t>{entry.common.get(),
                                                                     entry.lineOffset,
                                                                     messageToggleBits()};
            if(auto const it = renderCache.find(key); it != renderCache.end()) {
                return it->second.second;
            }
            if(renderCache.size() >= MaxRenderCacheEntries) { renderCache.clear(); }
            auto element = ansiColoredTextToFtxui(processLogMessage(entry.lineText()));
            renderCache.emplace(key, std::pair{entry.common, element});
            return element;
        }

        auto defaultRender(GuiLogEntry const& entry) {
            auto const&     common = *entry.common;
            ftxui::Elements elements;
            elements.reserve(12);

            bool const showPrefix = entry.startsGroup();

            if(showPrefix) {
                // Show full prefix for first/single lines
                if(showSysTime) {
                    elements.push_back(
                      ftxui::text(to_time_string_with_milliseconds(common.recvTime))
                      | ftxui::color(Theme::Text::timestamp()));
                    elements.push_back(ftxui::text(" "));
                }

                if(showChannel) {
                    elements.push_back(
                      toElement(uc_log::detail::LogEntry::Channel{common.channel}));
                    elements.push_back(ftxui::text(" "));
                }

                if(showUcTime) {
                    elements.push_back(ftxui::text(fmt::format("{}", common.ucTime))
                                       | ftxui::color(Theme::Text::ucTime()));
                    elements.push_back(ftxui::text(" "));
                }

                if(showLogLevel) {
                    elements.push_back(toElement(common.logLevel));
                    elements.push_back(ftxui::text("| ") | ftxui::color(Theme::Text::separator()));
                }
            } else {
                // Indent continuation lines to align with message column
                std::size_t const indentWidth = calculatePrefixWidth();
                elements.push_back(ftxui::text(std::string(indentWidth, ' ')));
            }

            // An unparsed entry carries defaults (trace, time 0, no location): mark it so
            // it is never mistaken for a trusted trace entry
            if(!common.parsedOk && showPrefix) {
                elements.push_back(ftxui::text("⚠ unparsed ") | ftxui::color(Theme::Status::error())
                                   | ftxui::bold);
            }

            auto messageElement = renderMessageElement(entry);
            // matching lines get a visible marker while a substring search is active (the
            // decoration wraps the cached element, so the cache stays search-agnostic)
            if(!display.searchNeedleLower.empty()
               && CompiledFilter::containsCaseInsensitive(entry.lineText(),
                                                          display.searchNeedleLower))
            {
                messageElement = messageElement | ftxui::bold | ftxui::underlined;
            }
            elements.push_back(std::move(messageElement));

            auto scrollableContent = ftxui::hbox(elements) | ftxui::flex;

            // Metadata: only show on single/last lines
            bool const showMetadata
              = (entry.lineType == LineType::SingleLine || entry.lineType == LineType::Last);

            ftxui::Element metadataElement;
            if(showMetadata) {
                ftxui::Elements metadata;
                if(showFunctionName) {
                    metadata.push_back(ftxui::text(display.functionNameOf(common.functionId))
                                       | ftxui::color(Theme::Text::functionName()));
                }

                if(showLocation) {
                    if(showFunctionName) { metadata.push_back(ftxui::text(" ")); }
                    metadata.push_back(
                      ftxui::text(fmt::format("{}:{}",
                                              display.fileNameOf(common.locationKey),
                                              GuiEntryStore::Mirror::lineOf(common.locationKey)))
                      | ftxui::color(Theme::Text::metadata()));
                }
                // Add filler to push content to the right and ensure consistent width
                metadata.insert(metadata.begin(), ftxui::filler());
                metadataElement = ftxui::hbox(metadata);
            } else {
                // For continuation lines, use filler to match the same behavior
                metadataElement = ftxui::hbox({ftxui::filler()});
            }

            return std::make_shared<ScrollableWithMetadata>(std::move(scrollableContent),
                                                            std::move(metadataElement));
        }

        ftxui::Element renderMessage(MessageEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(to_time_string_with_milliseconds(entry.time))
                               | ftxui::color(Theme::Text::timestamp()));

            elements.push_back(ftxui::text(" | ") | ftxui::color(Theme::Text::normal()));

            ftxui::Decorator messageColor;
            if(entry.level == MessageEntry::Level::Fatal) {
                messageColor = ftxui::color(Theme::Message::fatal());
            } else if(entry.level == MessageEntry::Level::Error) {
                messageColor = ftxui::color(Theme::Message::error());
            } else if(entry.level == MessageEntry::Level::Status) {
                messageColor = ftxui::color(ftxui::Color::White);
            } else if(entry.level == MessageEntry::Level::ToolError) {
                messageColor = ftxui::color(ftxui::Color::RedLight);
            } else {
                messageColor = ftxui::color(ftxui::Color::Cyan);
            }

            elements.push_back(ftxui::text(entry.message) | messageColor | ftxui::flex);

            return ftxui::hbox(elements);
        }

        void exportFilteredLogs(std::string              dir,
                                EntryChunkList::Snapshot entries,
                                bool                     asPlainText) {
            namespace lf       = uc_log::detail::logformat;
            namespace fs       = std::filesystem;
            auto const    path = fs::path{dir}
                               / fmt::format("filtered_{}.{}",
                                             lf::toIso8601Utc(std::chrono::system_clock::now()),
                                             asPlainText ? "txt" : "rttlog");
            std::ofstream f{path};
            if(!f.is_open()) {
                lastExportPath = path.string();
                lastExportOk   = false;
                errorMessage(fmt::format("Export failed — cannot write: {:?}", path));
                return;
            }
            if(!asPlainText) { lf::writeHeader(f); }
            for(auto const& e : entries) {
                auto const& c = *e.common;
                if(asPlainText) {
                    fmt::print(f,
                               "{} {} {} {:<5}| {} ({}:{} {})\n",
                               to_time_string_with_milliseconds(c.recvTime),
                               c.channel,
                               c.ucTime,
                               enchantum::to_string(c.logLevel),
                               e.lineText(),
                               display.fileNameOf(c.locationKey),
                               GuiEntryStore::Mirror::lineOf(c.locationKey),
                               display.functionNameOf(c.functionId));
                    continue;
                }
                uc_log::detail::LogEntry line{c.channel, {}};
                line.ucTime       = c.ucTime;
                line.fileName     = display.fileNameOf(c.locationKey);
                line.line         = GuiEntryStore::Mirror::lineOf(c.locationKey);
                line.logLevel     = c.logLevel;
                line.functionName = display.functionNameOf(c.functionId);
                line.logMsg       = std::string{e.lineText()};
                line.parsedOk     = c.parsedOk;
                lf::writeEntry(f, c.recvTime, line);
            }
            lastExportPath  = path.string();
            lastExportCount = entries.size();
            lastExportOk    = true;
            statusMessage(fmt::format("{} entries saved to {}", entries.size(), path.string()));
        }

        void updateCurrentFilter() { store.setFilterState(editedFilterState); }

        void saveFilterConfig(std::string const& path,
                              FilterState const& fs) {
            std::string buffer{};
            if(auto err = glz::write_json(fs, buffer); err) {
                filterConfigStatus = "Error serializing: " + glz::format_error(err, buffer);
                return;
            }
            auto const    pretty = glz::prettify_json(buffer);
            std::ofstream out(path);
            if(!out) {
                filterConfigStatus = fmt::format("Error: cannot open '{}' for writing", path);
                return;
            }
            out << pretty;
            if(!out) {
                filterConfigStatus = "Error: write failed";
                return;
            }
            filterConfigStatus = "Saved.";
        }

        void loadFilterConfig(std::string const& path,
                              FilterState&       fs) {
            std::ifstream in(path);
            if(!in) {
                filterConfigStatus = fmt::format("Error: cannot open '{}'", path);
                return;
            }
            std::string const buffer(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>{});
            FilterState       loaded{};
            if(auto err = glz::read_json(loaded, buffer); err) {
                filterConfigStatus = "Error parsing: " + glz::format_error(err, buffer);
                return;
            }
            fs = std::move(loaded);
            updateCurrentFilter();
            filterConfigStatus = "Loaded.";
        }

        [[nodiscard]] static OutlierResult
        computeOutliers(std::vector<std::pair<SourceLocation,
                                              std::size_t>> const& locations,
                        OutlierMethod                              method,
                        double                                     iqrK,
                        double                                     topNPct,
                        std::size_t                                absThreshold) {
            if(locations.size() < 3) { return {}; }

            auto counts = locations | std::views::values | std::ranges::to<std::vector>();
            std::ranges::sort(counts);
            auto const n = counts.size();

            auto medianOf = [&counts](std::size_t lo, std::size_t hi) -> std::size_t {
                auto const len = hi - lo;
                if(len % 2 == 1) { return counts[lo + len / 2]; }
                return (counts[lo + len / 2 - 1] + counts[lo + len / 2]) / 2;
            };
            std::size_t const medianVal = medianOf(0, n);
            std::size_t const q1        = medianOf(0, n / 2);
            std::size_t const q3        = medianOf((n % 2 == 1) ? n / 2 + 1 : n / 2, n);

            std::size_t cutoff{};
            switch(method) {
            case OutlierMethod::IQRTukey:
                {
                    double const iqr = (q3 >= q1) ? static_cast<double>(q3 - q1) : 0.0;
                    cutoff           = static_cast<std::size_t>(static_cast<double>(q3)
                                                                + iqrK * (iqr > 0.0 ? iqr : 1.0));
                    cutoff           = std::max(cutoff, std::size_t{1});
                    break;
                }
            case OutlierMethod::TopNPercent:
                {
                    if(topNPct <= 0.0 || topNPct >= 100.0) {
                        cutoff = counts.back();
                        break;
                    }
                    auto const keep = static_cast<std::size_t>(
                      std::floor((1.0 - topNPct / 100.0) * static_cast<double>(n)));
                    cutoff = (keep > 0 && keep < n) ? counts[keep] : counts.back();
                    break;
                }
            case OutlierMethod::AbsoluteThreshold: cutoff = absThreshold; break;
            }

            return OutlierResult{.cutoff       = cutoff,
                                 .q1           = q1,
                                 .median       = medianVal,
                                 .q3           = q3,
                                 .wouldExclude = locations
                                               | std::views::filter([cutoff](auto const& kv) {
                                                     return kv.second > cutoff;
                                                 })
                                               | std::views::keys | std::ranges::to<std::vector>(),
                                 .valid        = true};
        }

        // recompute only when the location list or the parameters changed: the full
        // copy + sort used to run once per rendered frame
        OutlierResult const& currentOutliers() {
            // bitwise compare: this is change detection of the exact stored value, not a
            // numeric tolerance question
            auto const bitsDiffer = [](double a, double b) {
                return std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b);
            };
            if(cachedOutlierVersion != display.seenLocationsVersion
               || cachedOutlierMethod != outlierMethod
               || bitsDiffer(cachedOutlierIqrK, iqrMultiplier)
               || bitsDiffer(cachedOutlierTopN, topNPercent)
               || cachedOutlierAbs != absoluteThreshold)
            {
                cachedOutlierVersion = display.seenLocationsVersion;
                cachedOutlierMethod  = outlierMethod;
                cachedOutlierIqrK    = iqrMultiplier;
                cachedOutlierTopN    = topNPercent;
                cachedOutlierAbs     = absoluteThreshold;
                cachedOutlierResult  = computeOutliers(display.locationList,
                                                       outlierMethod,
                                                       iqrMultiplier,
                                                       topNPercent,
                                                       absoluteThreshold);
            }
            return cachedOutlierResult;
        }

        void autoExcludeNoisyLocations() {
            auto const& result = currentOutliers();
            if(!result.valid) {
                noiseExcludeStatus = "Need ≥ 3 known locations";
                return;
            }

            std::ranges::copy(result.wouldExclude,
                              std::inserter(editedFilterState.excludedLocations,
                                            editedFilterState.excludedLocations.end()));
            updateCurrentFilter();

            noiseExcludeStatus = result.wouldExclude.empty()
                                 ? "No outliers found"
                                 : fmt::format("{} location{} excluded",
                                               result.wouldExclude.size(),
                                               result.wouldExclude.size() == 1 ? "" : "s");
        }

        // base64 for the OSC 52 clipboard escape
        static std::string base64Encode(std::string_view input) {
            static constexpr char Alphabet[]
              = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((input.size() + 2) / 3) * 4);
            for(std::size_t i = 0; i < input.size(); i += 3) {
                std::uint32_t     block = 0;
                std::size_t const n     = std::min<std::size_t>(3, input.size() - i);
                for(std::size_t j = 0; j < n; ++j) {
                    block |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + j]))
                          << (16U - 8U * j);
                }
                out.push_back(Alphabet[(block >> 18U) & 0x3FU]);
                out.push_back(Alphabet[(block >> 12U) & 0x3FU]);
                out.push_back(n > 1 ? Alphabet[(block >> 6U) & 0x3FU] : '=');
                out.push_back(n > 2 ? Alphabet[block & 0x3FU] : '=');
            }
            return out;
        }

        void yankEntryToClipboard(std::size_t index) {
            if(index >= display.entries.size()) { return; }
            auto const& e    = display.entries[index];
            auto const& c    = *e.common;
            auto        text = fmt::format("{} {} {} {}| {} ({}:{} {})",
                                           to_time_string_with_milliseconds(c.recvTime),
                                           c.channel,
                                           c.ucTime,
                                           enchantum::to_string(c.logLevel),
                                           e.lineText(),
                                           display.fileNameOf(c.locationKey),
                                           GuiEntryStore::Mirror::lineOf(c.locationKey),
                                           display.functionNameOf(c.functionId));
            // OSC 52 goes to the same terminal ftxui renders on, but only after the frame
            // is done (pendingActions run outside RunOnce), so it cannot interleave with
            // ftxui's own escape output
            pendingActions.push_back([this, payload = std::move(text)]() {
                auto const sequence = fmt::format("\033]52;c;{}\a", base64Encode(payload));
                std::fwrite(sequence.data(), 1, sequence.size(), stdout);
                std::fflush(stdout);
                statusMessage("line copied to clipboard (OSC 52)");
            });
        }

        void jumpToUcTime() {
            double target{};
            try {
                target = std::stod(jumpToStr);
            } catch(...) { return; }
            auto const targetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>{target});
            for(std::size_t i = 0; i < display.entries.size(); ++i) {
                if(display.entries[i].common->ucTime.time >= targetNs) {
                    if(logScrollerJump) { logScrollerJump(static_cast<int>(i)); }
                    return;
                }
            }
            statusMessage(fmt::format("no entry at or after uc-time {:.3f} s", target));
        }

        ftxui::Component getLogComponent() {
            auto scroller = MakeScroller(
              [this]() -> EntryChunkList::Snapshot const& { return display.entries; },
              [this](auto const& entry) { return defaultRender(entry); });
            scroller->onYank = [this](std::size_t index) { yankEntryToClipboard(index); };
            logScrollerJump  = [scroller](int index) { scroller->JumpTo(index); };

            ftxui::InputOption searchOpts;
            searchOpts.multiline = false;
            searchOpts.on_change = [this]() {
                searchRowShown = true;
                store.setSearchText(searchStr);
            };
            searchInput = trackInput(ftxui::Input(&searchStr, "text or re:pattern", searchOpts));

            ftxui::InputOption jumpOpts;
            jumpOpts.multiline = false;
            jumpOpts.on_enter  = [this]() { jumpToUcTime(); };
            jumpToInput = trackInput(ftxui::Input(&jumpToStr, "s", jumpOpts) | numericFilter(true));

            auto searchRow = ftxui::Container::Horizontal(
              {ftxui::Renderer([]() { return ftxui::text(" 🔎 [/] "); }),
               searchInput | ftxui::border | ftxui::flex,
               ftxui::Renderer([]() { return ftxui::text("  ⏱ go to "); }),
               jumpToInput | ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10),
               ftxui::Renderer([]() { return ftxui::text(" s ⏎ "); })});

            // hidden until '/' focuses it (or text is present), so it costs no space while idle
            auto searchRowVisible
              = [this]() { return searchRowShown || !searchStr.empty() || !jumpToStr.empty(); };
            auto logsTab
              = ftxui::Container::Vertical({ftxui::Maybe(searchRow, std::move(searchRowVisible)),
                                            ftxui::Component{scroller} | ftxui::flex});

            // Esc leaves the search/jump inputs, hides the row (unless a filter is
            // active) and returns to the log list, so the hotkeys (y, q, ...) work
            // again immediately
            return ftxui::CatchEvent(logsTab, [this, scroller](ftxui::Event const& event) {
                if(event == ftxui::Event::Escape
                   && ((searchInput && searchInput->Focused())
                       || (jumpToInput && jumpToInput->Focused())))
                {
                    searchRowShown = false;
                    scroller->TakeFocus();
                    return true;
                }
                return false;
            });
        }

        ftxui::Component getStatusComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear messages",
              [this]() { statusMessages.clear(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            return ftxui::Container::Vertical(
              {clearButton,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Vertical(
                 {Scroller([&]() -> std::vector<MessageEntry> const& { return statusMessages; },
                           [&](auto const& entry) { return renderMessage(entry); })})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("💬 Status Messages") | ftxui::bold
                                             | ftxui::color(Theme::Header::secondary())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           std::move(inner)});
                   })});
        }

        ftxui::Element renderBuildEntry(BuildEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(to_time_string_with_milliseconds(entry.time))
                               | ftxui::color(Theme::Text::timestamp()));

            elements.push_back(ftxui::text(" | ") | ftxui::color(Theme::Text::normal()));

            if(entry.fromTool) {
                elements.push_back(ansiColoredTextToFtxui(entry.line) | ftxui::flex);
            } else {
                auto lineColor = entry.isError ? ftxui::color(Theme::Status::error())
                                               : ftxui::color(Theme::Text::normal());
                elements.push_back(ftxui::text(entry.line) | lineColor | ftxui::flex);
            }
            return ftxui::hbox(elements);
        }

        ftxui::Element buildStatusToElement() {
            std::string  statusText;
            ftxui::Color statusColor;

            switch(buildRunner.getStatus()) {
            case BuildStatus::Idle:
                statusText  = "⚪ Idle";
                statusColor = Theme::Status::inactive();
                break;
            case BuildStatus::Running:
                statusText  = "🟡 Building...";
                statusColor = Theme::Status::running();
                break;
            case BuildStatus::Success:
                statusText  = "✅ Success";
                statusColor = Theme::Status::success();
                break;
            case BuildStatus::Failed:
                statusText  = "❌ Failed";
                statusColor = Theme::Status::failed();
                break;
            }
            return ftxui::text(statusText) | ftxui::color(statusColor) | ftxui::bold;
        }

        ftxui::Component getMetricPlotComponent() {
            // the metric series live behind the store mutex and the producer appends to
            // them; copy the selected series out under the lock so the plot renders from
            // stable data
            auto dataProvider
              = [this](MetricInfo const& metric) -> std::optional<std::vector<MetricEntry> const*> {
                std::lock_guard<std::mutex> const lock{store.mutex};
                auto                              iter = store.metricEntries.find(metric);
                if(iter != store.metricEntries.end() && !iter->second.empty()) {
                    plotDataBuffer = iter->second;
                    return &plotDataBuffer;
                }
                return std::nullopt;
            };

            auto clearCallback = [this]() {
                if(auto selectedMetric = metricPlotWidget.getSelectedMetric()) {
                    std::lock_guard<std::mutex> const lock{store.mutex};
                    auto iter = store.metricEntries.find(*selectedMetric);
                    if(iter != store.metricEntries.end()) { iter->second.clear(); }
                }
            };

            return metricPlotWidget.createComponent(dataProvider, clearCallback);
        }

        ftxui::Component getBuildComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Output",
              [this]() { buildRunner.clearOutput(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto stopButton = ftxui::Button(
              "⏸ Stop Build",
              [this]() { buildRunner.cancel(); },
              createButtonStyle(Theme::Button::Background::danger(), Theme::Button::text()));

            auto buildButton = ftxui::Button(
              "🔨 Start Build [b]",
              [this]() { executeBuild(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto buildAndFlashButton = ftxui::Button(
              "🔨⚡ Build & Flash [shift+F]",
              [this]() { executeBuildAndFlash(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto outputScroller
              = Scroller([this]() -> std::vector<BuildEntry> const& { return buildOutputDisplay; },
                         [&](auto const& entry) { return renderBuildEntry(entry); });

            auto statusDisplay
              = ftxui::Container::Vertical({outputScroller | ftxui::flex})
              | ftxui::Renderer([this](ftxui::Element inner) {
                    return ftxui::vbox(
                      {ftxui::text("🔨 Build Status") | ftxui::bold
                         | ftxui::color(Theme::Header::primary()) | ftxui::center,
                       ftxui::separator(),
                       ftxui::hbox({ftxui::text("Status: ") | ftxui::bold, buildStatusToElement()}),
                       ftxui::hbox({ftxui::text("Output Lines: ") | ftxui::bold,
                                    ftxui::text(fmt::format("{}", buildOutputDisplay.size()))
                                      | ftxui::color(Theme::Status::info())}),
                       ftxui::separator(),
                       std::move(inner)});
                });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({buildButton | ftxui::flex,
                                             buildAndFlashButton | ftxui::flex,
                                             stopButton | ftxui::flex,
                                             clearButton | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               statusDisplay | ftxui::flex});
        }

        ftxui::Component getMetricOverviewComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Metrics",
              [this]() {
                  {
                      std::lock_guard<std::mutex> const storeLock{store.mutex};
                      store.metricEntries.clear();
                  }
                  metricPlotWidget.setSelectedMetric(std::nullopt);
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            std::vector<ftxui::Component> components;
            components.push_back(clearButton);
            components.push_back(ftxui::Renderer([]() { return ftxui::separator(); }));

            components.push_back(ftxui::Renderer([this]() {
                std::size_t count{};
                {
                    std::lock_guard<std::mutex> const storeLock{store.mutex};
                    count = store.metricEntries.size();
                }
                return ftxui::text(fmt::format("📈 Metrics ({} entries)", count)) | ftxui::bold
                     | ftxui::color(Theme::Header::primary()) | ftxui::center;
            }));
            components.push_back(ftxui::Renderer([]() { return ftxui::separator(); }));

            auto metricsContainer = ftxui::Container::Vertical({});

            auto dynamicMetricsList
              = metricsContainer
              | ftxui::Renderer([this, metricsContainer](ftxui::Element const&) mutable {
                    auto currentSelected = metricPlotWidget.getSelectedMetric();

                    std::vector<MetricInfo> metricInfos;
                    bool                    needsRebuild{};
                    {
                        std::lock_guard<std::mutex> const storeLock{store.mutex};
                        needsRebuild = (store.metricEntries.size() != lastMetricCount)
                                    || (!hasLastSelectedInfo && currentSelected.has_value())
                                    || (hasLastSelectedInfo && !currentSelected.has_value())
                                    || (hasLastSelectedInfo && currentSelected.has_value()
                                        && *currentSelected != lastSelectedInfo);
                        if(needsRebuild) {
                            lastMetricCount = store.metricEntries.size();
                            for(auto const& [info, values] : store.metricEntries) {
                                metricInfos.push_back(info);
                            }
                        }
                    }

                    if(needsRebuild) {
                        metricsContainer->DetachAllChildren();

                        if(metricInfos.empty()) {
                            metricsContainer->Add(ftxui::Renderer([]() {
                                return ftxui::text("No metrics available")
                                     | ftxui::color(Theme::Status::inactive()) | ftxui::center;
                            }));
                        } else {
                            for(auto const& metricInfo : metricInfos) {
                                bool const isSelected
                                  = metricPlotWidget.getSelectedMetric()
                                 && metricPlotWidget.getSelectedMetric() == metricInfo;

                                auto selectButton = ftxui::Button(
                                  isSelected ? "📈 Selected" : "📊 Select",
                                  [this, metricInfo]() {
                                      metricPlotWidget.setSelectedMetric(metricInfo);
                                  },
                                  createButtonStyle(isSelected
                                                      ? Theme::Button::Background::positive()
                                                      : Theme::Button::Background::build(),
                                                    Theme::Button::text()));

                                auto metricRow = ftxui::Container::Horizontal(
                                  {ftxui::Renderer([this, metricInfo]() {
                                       double      latestValue{};
                                       std::size_t valueCount{};
                                       bool        found{};
                                       {
                                           std::lock_guard<std::mutex> const storeLock{store.mutex};
                                           auto iter = store.metricEntries.find(metricInfo);
                                           if(iter != store.metricEntries.end()) {
                                               found       = true;
                                               valueCount  = iter->second.size();
                                               latestValue = iter->second.empty()
                                                             ? 0.0
                                                             : iter->second.back().value;
                                           }
                                       }
                                       if(!found) {
                                           return ftxui::text("Metric not found")
                                                | ftxui::color(Theme::Status::error());
                                       }

                                       return ftxui::hbox(
                                                {ftxui::text("📊 ")
                                                   | ftxui::color(Theme::Data::icon()),
                                                 ftxui::text(metricInfo.scope)
                                                   | ftxui::color(Theme::Data::scope()),
                                                 ftxui::text("::")
                                                   | ftxui::color(Theme::UI::separator()),
                                                 ftxui::text(metricInfo.name)
                                                   | ftxui::color(Theme::Data::name())
                                                   | ftxui::bold,
                                                 ftxui::text(
                                                   metricInfo.unit.empty()
                                                     ? ""
                                                     : fmt::format(" [{}]", metricInfo.unit))
                                                   | ftxui::color(Theme::Data::unit()),
                                                 ftxui::text(fmt::format(" = {:.3f}", latestValue))
                                                   | ftxui::color(Theme::Status::info())
                                                   | ftxui::bold,
                                                 ftxui::text(
                                                   fmt::format(" ({} values)", valueCount))
                                                   | ftxui::color(Theme::Data::count())})
                                            | ftxui::flex;
                                   }) | ftxui::flex,
                                   selectButton});

                                metricsContainer->Add(metricRow);
                            }
                        }

                        if(currentSelected.has_value()) {
                            hasLastSelectedInfo = true;
                            lastSelectedInfo    = *currentSelected;
                        } else {
                            hasLastSelectedInfo = false;
                        }
                    }

                    return metricsContainer->Render();
                });

            components.push_back(dynamicMetricsList);

            return ftxui::Container::Vertical(components);
        }

        ftxui::Component getMetricComponent() {
            auto metricTabs = generateMetricTabsComponent({
              { "📋 Overview", getMetricOverviewComponent()},
              {"📈 Live Plot",     getMetricPlotComponent()}
            });

            return ftxui::Container::Vertical({metricTabs | ftxui::flex});
        }

        ftxui::Component getLogLevelFilterComponent() {
            static constexpr std::array levels{uc_log::LogLevel::trace,
                                               uc_log::LogLevel::debug,
                                               uc_log::LogLevel::info,
                                               uc_log::LogLevel::warn,
                                               uc_log::LogLevel::error,
                                               uc_log::LogLevel::crit};

            std::vector<ftxui::Component> logLevel_components{};

            auto allButton = ftxui::Button(
              "📋 Enable All Levels",
              [this]() {
                  editedFilterState.enabledLogLevels.clear();
                  updateCurrentFilter();
              },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            logLevel_components.push_back(allButton);

            for(auto level : levels) {
                auto checkbox = FunctionCheckbox(
                  std::string{enchantum::to_string(level)},
                  [level, this]() {
                      return editedFilterState.enabledLogLevels.contains(level)
                          || editedFilterState.enabledLogLevels.empty();
                  },
                  [level, this]() {
                      if(editedFilterState.enabledLogLevels.empty()) {
                          editedFilterState.enabledLogLevels.insert(levels.begin(), levels.end());
                          editedFilterState.enabledLogLevels.erase(level);
                      } else {
                          if(editedFilterState.enabledLogLevels.contains(level)) {
                              editedFilterState.enabledLogLevels.erase(level);
                          } else {
                              editedFilterState.enabledLogLevels.insert(level);
                          }
                      }

                      if(editedFilterState.enabledLogLevels.size() == levels.size()) {
                          editedFilterState.enabledLogLevels.clear();
                      }

                      updateCurrentFilter();
                  });

                logLevel_components.push_back(checkbox);
            }

            return ftxui::Container::Vertical(logLevel_components) | ftxui::border;
        }

        ftxui::Component getChannelFilterComponent() {
            static constexpr auto channels
              = std::ranges::iota_view{std::size_t{0}, GUI_Constants::MaxChannels};
            std::vector<ftxui::Component> channel_components{};

            auto allButton = ftxui::Button(
              "📡 Enable All Channels",
              [this]() {
                  editedFilterState.enabledChannels.clear();
                  updateCurrentFilter();
              },
              createButtonStyle(Theme::Button::Background::build(), Theme::Button::text()));

            channel_components.push_back(allButton);

            for(auto channel : channels) {
                auto checkbox = FunctionCheckbox(
                  fmt::format("Channel {}", channel),
                  [channel, this]() {
                      return editedFilterState.enabledChannels.contains(channel)
                          || editedFilterState.enabledChannels.empty();
                  },
                  [channel, this]() {
                      if(editedFilterState.enabledChannels.empty()) {
                          editedFilterState.enabledChannels.insert(channels.begin(),
                                                                   channels.end());
                          editedFilterState.enabledChannels.erase(channel);
                      } else {
                          if(editedFilterState.enabledChannels.contains(channel)) {
                              editedFilterState.enabledChannels.erase(channel);
                          } else {
                              editedFilterState.enabledChannels.insert(channel);
                          }
                      }

                      if(editedFilterState.enabledChannels.size() == channels.size()) {
                          editedFilterState.enabledChannels.clear();
                      }

                      updateCurrentFilter();
                  });

                channel_components.push_back(checkbox);
            }

            return ftxui::Container::Vertical(channel_components) | ftxui::border;
        }

        ftxui::Component getLocationFilterComponent() {
            auto addIncludeEntry = [this](SourceLocation const& sourceLocation) {
                if(!editedFilterState.includedLocations.contains(sourceLocation)) {
                    editedFilterState.includedLocations.insert(sourceLocation);
                    updateCurrentFilter();
                }
            };

            auto addExcludeEntry = [this](SourceLocation const& sourceLocation) {
                if(!editedFilterState.excludedLocations.contains(sourceLocation)) {
                    editedFilterState.excludedLocations.insert(sourceLocation);
                    updateCurrentFilter();
                }
            };

            auto stringToSourceLocation
              = [](std::string const& input) -> std::optional<SourceLocation> {
                auto colonPosition = std::ranges::find(input, ':');
                if(colonPosition == input.end()) {
                    if(input.empty()) { return std::nullopt; }
                    return SourceLocation{input, 0};
                }
                std::size_t line{};
                auto parseResult = std::from_chars(&(*(colonPosition + 1)), &(*input.end()), line);
                if(parseResult.ec == std::errc{} && parseResult.ptr == &(*input.end())) {
                    return SourceLocation{
                      std::string_view{input.begin(), colonPosition},
                      line
                    };
                }
                return std::nullopt;
            };

            std::vector<ftxui::Component> manualInputComponents;
            manualInputComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📝 Manual:") | ftxui::bold
                     | ftxui::color(Theme::Header::accent());
            }));
            ftxui::InputOption manualLocationOpts;
            manualLocationOpts.multiline = false;
            manualLocationInput
              = trackInput(ftxui::Input(&locationFilterInput, "filename:line", manualLocationOpts)
                           | ftxui::flex);
            manualInputComponents.push_back(manualLocationInput);
            manualInputComponents.push_back(ftxui::Maybe(
              ftxui::Button(
                "🟢 Include",
                [this, addIncludeEntry, stringToSourceLocation]() {
                    auto sourceLocation = stringToSourceLocation(locationFilterInput);
                    if(sourceLocation) {
                        addIncludeEntry(*sourceLocation);
                        locationFilterInput.clear();
                    }
                },
                createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text())),
              [this, stringToSourceLocation]() {
                  return stringToSourceLocation(locationFilterInput).has_value();
              }));
            manualInputComponents.push_back(ftxui::Maybe(
              ftxui::Button(
                "🔴 Exclude",
                [this, addExcludeEntry, stringToSourceLocation]() {
                    auto sourceLocation = stringToSourceLocation(locationFilterInput);
                    if(sourceLocation) {
                        addExcludeEntry(*sourceLocation);
                        locationFilterInput.clear();
                    }
                },
                createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text())),
              [this, stringToSourceLocation]() {
                  return stringToSourceLocation(locationFilterInput).has_value();
              }));

            auto manualInputComponent = ftxui::Container::Horizontal(manualInputComponents);

            std::vector<ftxui::Component> dropdownComponents;
            dropdownComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📋 Known:") | ftxui::bold | ftxui::color(Theme::Status::info());
            }));

            ftxui::DropdownOption dropdownOptions;
            dropdownOptions.radiobox.entries
              = std::make_unique<StringVectorAdapter>(display.locationLabels);
            dropdownOptions.radiobox.selected  = &selectedLocationIndex;
            dropdownOptions.radiobox.on_change = [this]() {
                auto const index = static_cast<std::size_t>(selectedLocationIndex);
                if(index < display.locationList.size()) {
                    selectedSourceLocation = display.locationList[index].first;
                }
            };

            dropdownOptions.radiobox.transform
              = [this](ftxui::EntryState const& state) -> ftxui::Element {
                auto const index = static_cast<std::size_t>(state.index);
                if(index >= display.locationList.size()) { return ftxui::text(state.label); }
                SourceLocation const& location = display.locationList[index].first;

                bool const isIncluded = editedFilterState.includedLocations.contains(location);
                bool const isExcluded = editedFilterState.excludedLocations.contains(location);

                auto element = ftxui::text(state.label);

                if(state.active) { element |= ftxui::bold; }
                if(state.focused) { element |= ftxui::inverted; }

                if(isIncluded) {
                    element = ftxui::hbox(
                      {ftxui::text("🟢 ") | ftxui::color(Theme::Status::success()), element});
                } else if(isExcluded) {
                    element = ftxui::hbox(
                      {ftxui::text("🔴 ") | ftxui::color(Theme::Status::error()), element});
                } else {
                    element = ftxui::hbox(
                      {ftxui::text("⚪ ") | ftxui::color(Theme::Status::inactive()), element});
                }

                return element;
            };

            dropdownComponents.push_back(ftxui::Dropdown(dropdownOptions));

            auto getSelectedLocation = [this]() -> SourceLocation {
                auto const index = static_cast<std::size_t>(selectedLocationIndex);
                if(index < display.locationList.size()) {
                    return display.locationList[index].first;
                }
                return {};
            };

            auto hasSelectedLocation
              = [getSelectedLocation]() { return !getSelectedLocation().first.empty(); };

            auto includeLineButton = ftxui::Maybe(
              ftxui::Button(
                "🟢 Include Line",
                [addIncludeEntry, getSelectedLocation]() {
                    addIncludeEntry(getSelectedLocation());
                },
                createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text())),
              hasSelectedLocation);
            auto excludeLineButton = ftxui::Maybe(
              ftxui::Button(
                "🔴 Exclude Line",
                [addExcludeEntry, getSelectedLocation]() {
                    addExcludeEntry(getSelectedLocation());
                },
                createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text())),
              hasSelectedLocation);

            auto includeFileButton = ftxui::Maybe(
              ftxui::Button(
                "📁🟢 Include File",
                [addIncludeEntry, getSelectedLocation]() {
                    addIncludeEntry(SourceLocation{getSelectedLocation().first, 0});
                },
                createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text())),
              hasSelectedLocation);
            auto excludeFileButton = ftxui::Maybe(
              ftxui::Button(
                "📁🔴 Exclude File",
                [addExcludeEntry, getSelectedLocation]() {
                    addExcludeEntry(SourceLocation{getSelectedLocation().first, 0});
                },
                createButtonStyle(Theme::Button::Background::danger(), Theme::Button::text())),
              hasSelectedLocation);

            auto lineButtons = ftxui::Container::Horizontal({includeLineButton, excludeLineButton});
            auto fileButtons = ftxui::Container::Horizontal({includeFileButton, excludeFileButton});

            auto buttonRows
              = ftxui::Container::Vertical({lineButtons, fileButtons})
              | ftxui::Renderer([lineButtons, fileButtons](ftxui::Element) {
                    return ftxui::vbox({lineButtons->Render(),
                                        ftxui::separator() | ftxui::color(Theme::UI::separator()),
                                        fileButtons->Render()});
                });

            dropdownComponents.push_back(buttonRows);

            auto dropdownComponent = ftxui::Container::Horizontal(dropdownComponents);

            std::vector<ftxui::Component> inputSectionComponents;
            inputSectionComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📍 Location Filters") | ftxui::bold
                     | ftxui::color(Theme::Header::accent()) | ftxui::center;
            }));
            inputSectionComponents.push_back(manualInputComponent);
            inputSectionComponents.push_back(dropdownComponent);

            auto inputComponent
              = ftxui::Container::Vertical(inputSectionComponents) | ftxui::border;

            auto includedContainer = ftxui::Container::Vertical({});
            auto includedContainerWithBorder
              = includedContainer
              | ftxui::Renderer([this, includedContainer](
                                  ftxui::Element const&) mutable -> ftxui::Element {
                    includedContainer->DetachAllChildren();

                    includedContainer->Add(ftxui::Renderer([this]() {
                        return ftxui::text(fmt::format("✅ Included Locations ({})",
                                                       editedFilterState.includedLocations.size()))
                             | ftxui::bold | ftxui::color(Theme::Status::success());
                    }));

                    if(editedFilterState.includedLocations.empty()) {
                        includedContainer->Add(ftxui::Renderer([]() {
                            return ftxui::text("(none)") | ftxui::color(Theme::Status::inactive())
                                 | ftxui::center;
                        }));
                    } else {
                        for(auto const& location : editedFilterState.includedLocations) {
                            std::string locationStr = location.first;
                            if(location.second != 0) {
                                locationStr += ":" + std::to_string(location.second);
                            } else {
                                locationStr += ":*";
                            }

                            auto removeButton = ftxui::Button(
                              "🟢 " + locationStr + " ❌",
                              [this, location]() {
                                  editedFilterState.includedLocations.erase(location);
                                  updateCurrentFilter();
                              },
                              createButtonStyle(Theme::Button::Background::build(),
                                                Theme::Button::text()));

                            includedContainer->Add(removeButton);
                        }
                    }

                    return includedContainer->Render();
                })
              | ftxui::border;

            auto excludedContainer = ftxui::Container::Vertical({});
            auto excludedContainerWithBorder
              = excludedContainer
              | ftxui::Renderer([this, excludedContainer](
                                  ftxui::Element const&) mutable -> ftxui::Element {
                    excludedContainer->DetachAllChildren();

                    excludedContainer->Add(ftxui::Renderer([this]() {
                        return ftxui::text(fmt::format("❌ Excluded Locations ({})",
                                                       editedFilterState.excludedLocations.size()))
                             | ftxui::bold | ftxui::color(Theme::Status::error());
                    }));

                    if(editedFilterState.excludedLocations.empty()) {
                        excludedContainer->Add(ftxui::Renderer([]() {
                            return ftxui::text("(none)") | ftxui::color(Theme::Status::inactive())
                                 | ftxui::center;
                        }));
                    } else {
                        for(auto const& location : editedFilterState.excludedLocations) {
                            std::string locationStr = location.first;
                            if(location.second != 0) {
                                locationStr += ":" + std::to_string(location.second);
                            } else {
                                locationStr += ":*";
                            }

                            auto removeButton = ftxui::Button(
                              "🔴 " + locationStr + " ❌",
                              [this, location]() {
                                  editedFilterState.excludedLocations.erase(location);
                                  updateCurrentFilter();
                              },
                              createButtonStyle(Theme::Button::Background::build(),
                                                Theme::Button::text()));

                            excludedContainer->Add(removeButton);
                        }
                    }

                    return excludedContainer->Render();
                })
              | ftxui::border;

            auto locationsContainer
              = ftxui::Container::Horizontal({includedContainerWithBorder | ftxui::flex,
                                              excludedContainerWithBorder | ftxui::flex});

            std::vector<ftxui::Component> finalComponents;
            finalComponents.push_back(inputComponent);
            finalComponents.push_back(locationsContainer);

            return ftxui::Container::Vertical(finalComponents);
        }

        ftxui::Component getFilterComponent() {
            ftxui::InputOption exportOpts;
            exportOpts.multiline = false;
            exportDirInputComponent
              = trackInput(ftxui::Input(&exportDirInput, "directory", exportOpts));

            auto exportBtn = ftxui::Button(
              " Export Filtered ",
              [this]() {
                  if(exportDirInput.empty()) { return; }
                  // the display snapshot is a cheap copy of chunk pointers and stays
                  // valid regardless of concurrent appends/trims
                  pendingActions.push_back([this,
                                            dir       = exportDirInput,
                                            entries   = display.entries,
                                            plainText = exportFormatSelection == 1]() {
                      exportFilteredLogs(std::move(dir), std::move(entries), plainText);
                  });
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto exportFormatToggle = ftxui::Toggle(std::vector<std::string>{" .rttlog ", " .txt "},
                                                    &exportFormatSelection);

            auto exportSection
              = ftxui::Container::Horizontal(
                  {ftxui::Renderer([]() { return ftxui::text(" Export dir: "); }),
                   exportDirInputComponent | ftxui::border | ftxui::flex,
                   ftxui::Renderer([]() { return ftxui::text(" as "); }),
                   exportFormatToggle,
                   ftxui::Renderer([]() { return ftxui::text(" "); }),
                   exportBtn})
              | ftxui::Renderer([this, exportBtn](ftxui::Element inner) {
                    auto resultEl
                      = lastExportPath.empty()
                        ? ftxui::text("")
                        : ftxui::text(lastExportOk
                                        ? fmt::format(" {} entries saved to {}",
                                                      lastExportCount,
                                                      lastExportPath)
                                        : fmt::format(" Export failed: {}", lastExportPath))
                            | ftxui::color(lastExportOk ? Theme::Status::success()
                                                        : Theme::Status::error());
                    return ftxui::vbox({ftxui::text("💾 Export Filtered Logs") | ftxui::bold
                                          | ftxui::color(Theme::Header::accent()) | ftxui::center,
                                        ftxui::separator(),
                                        std::move(inner),
                                        std::move(resultEl)})
                         | ftxui::border;
                });

            auto clearButton = ftxui::Button(
              "🗑️ Clear All Filters",
              [this]() {
                  editedFilterState   = FilterState{};
                  ucTimeLiveWindowStr = "10";
                  minUcTimeStr.clear();
                  maxUcTimeStr.clear();
                  store.updateUcTime([](GuiEntryStore& s) {
                      s.ucTimeFilterEnabled  = false;
                      s.ucTimeLiveMode       = false;
                      s.ucTimeLiveWindowSecs = 10.0;
                      s.minUcTimeSec         = 0.0;
                      s.maxUcTimeSec         = std::numeric_limits<double>::infinity();
                      s.requestRefilterLocked();
                  });
                  updateCurrentFilter();
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            // file I/O runs as a pending action after the UI mutex is released, so a slow
            // filesystem cannot stall input handling or the log producer
            auto saveButton = ftxui::Button(
              "💾 Save",
              [this]() {
                  pendingActions.push_back(
                    [this, path = filterConfigPath, filterState = editedFilterState]() {
                        saveFilterConfig(path, filterState);
                    });
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto loadButton = ftxui::Button(
              "📂 Load",
              [this]() {
                  pendingActions.push_back([this, path = filterConfigPath]() {
                      loadFilterConfig(path, editedFilterState);
                  });
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            ftxui::InputOption filterConfigOpts;
            filterConfigOpts.multiline = false;
            filterConfigInput
              = trackInput(ftxui::Input(&filterConfigPath, "filter.json", filterConfigOpts));

            // File row: label + input + status
            auto fileRow = ftxui::Container::Horizontal({filterConfigInput})
                         | ftxui::Renderer([this](ftxui::Element inner) {
                               ftxui::Element statusEl = ftxui::text("");
                               if(!filterConfigStatus.empty()) {
                                   bool isError = filterConfigStatus.rfind("Error", 0) == 0;
                                   statusEl     = ftxui::text("  " + filterConfigStatus)
                                                | ftxui::color(isError ? Theme::Status::error()
                                                                       : Theme::Status::success());
                               }
                               return ftxui::hbox(
                                 {ftxui::text(" 📁 File: ") | ftxui::color(Theme::Status::info()),
                                  std::move(inner) | ftxui::flex,
                                  std::move(statusEl)});
                           });

            // Buttons row: indented to align under the input, with gap between buttons
            auto buttonsRow = ftxui::Container::Horizontal({saveButton, loadButton})
                            | ftxui::Renderer([saveButton, loadButton](ftxui::Element) {
                                  return ftxui::hbox({ftxui::text(" 📁       "),
                                                      saveButton->Render(),
                                                      ftxui::text("  "),
                                                      loadButton->Render()});
                              });

            // Info hint
            auto infoBox = ftxui::Renderer([] {
                return ftxui::vbox({ftxui::separator(),
                                    ftxui::hbox({ftxui::text(" ℹ ") | ftxui::bold
                                                   | ftxui::color(Theme::Status::info()),
                                                 ftxui::text("Filter File") | ftxui::bold
                                                   | ftxui::color(Theme::Status::info())}),
                                    ftxui::text("   Saves/loads filter state as JSON. Relative "
                                                "paths use the working directory.")
                                      | ftxui::color(Theme::Status::inactive())});
            });

            auto saveLoadSection
              = ftxui::Container::Vertical({fileRow, buttonsRow})
              | ftxui::Renderer([infoBox](ftxui::Element inner) {
                    return ftxui::vbox({ftxui::text("💾 Filter Configuration") | ftxui::bold
                                          | ftxui::color(Theme::Header::secondary())
                                          | ftxui::center,
                                        ftxui::separator(),
                                        std::move(inner),
                                        infoBox->Render()})
                         | ftxui::border;
                });

            std::vector<ftxui::Component> mainComponents;

            std::vector<ftxui::Component> levelComponents;
            levelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📊 Log Levels") | ftxui::bold
                     | ftxui::color(Theme::Header::primary()) | ftxui::center;
            }));
            levelComponents.push_back(getLogLevelFilterComponent());

            std::vector<ftxui::Component> channelComponents;
            channelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📡 Channels") | ftxui::bold
                     | ftxui::color(Theme::Header::warning()) | ftxui::center;
            }));
            channelComponents.push_back(getChannelFilterComponent());

            std::vector<ftxui::Component> horizontalComponents;
            horizontalComponents.push_back(ftxui::Container::Vertical(levelComponents)
                                           | ftxui::flex);
            horizontalComponents.push_back(ftxui::Container::Vertical(channelComponents)
                                           | ftxui::flex);

            mainComponents.push_back(ftxui::Container::Horizontal(horizontalComponents));
            mainComponents.push_back(getLocationFilterComponent());

            // --- Noisy-exclude section ---
            // Method selector toggle
            std::vector<std::string> methodLabels{"Statistical", "Top Percent", "Count Limit"};
            auto methodToggle = ftxui::Toggle(std::move(methodLabels), &selectedOutlierMethod);
            methodToggle = ftxui::CatchEvent(methodToggle, [this](ftxui::Event const&) -> bool {
                outlierMethod = static_cast<OutlierMethod>(selectedOutlierMethod);
                noiseExcludeStatus.clear();
                return false;
            });

            // Statistical (IQR/Tukey) sensitivity param row (float text input, shown only for method 0)
            ftxui::InputOption iqrOpts;
            iqrOpts.multiline = false;
            iqrOpts.on_change = [this]() {
                try {
                    double const v = std::stod(iqrMultiplierStr);
                    if(v > 0.0) { iqrMultiplier = v; }
                } catch(std::exception const&) {}
            };
            iqrInput
              = trackInput(ftxui::Input(&iqrMultiplierStr, "1.5", iqrOpts) | numericFilter(true));
            auto iqrParamRow = ftxui::Maybe(
              ftxui::Container::Horizontal({iqrInput}) | ftxui::Renderer([](ftxui::Element inner) {
                  return ftxui::hbox(
                    {ftxui::text(" sensitivity: ") | ftxui::color(Theme::Status::info()),
                     std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8)});
              }),
              [this] { return selectedOutlierMethod == 0; });

            // Top Percent param row (float text input, shown only for method 1)
            ftxui::InputOption topNOpts;
            topNOpts.multiline = false;
            topNOpts.on_change = [this]() {
                try {
                    double const v = std::stod(topNPercentStr);
                    if(v > 0.0 && v < 100.0) { topNPercent = v; }
                } catch(std::exception const&) {}
            };
            topNInput
              = trackInput(ftxui::Input(&topNPercentStr, "10", topNOpts) | numericFilter(true));
            auto topNParamRow = ftxui::Maybe(
              ftxui::Container::Horizontal({topNInput}) | ftxui::Renderer([](ftxui::Element inner) {
                  return ftxui::hbox(
                    {ftxui::text(" top %: ") | ftxui::color(Theme::Status::info()),
                     std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8)});
              }),
              [this] { return selectedOutlierMethod == 1; });

            // Count Limit param row (integer text input, shown only for method 2)
            ftxui::InputOption absOpts;
            absOpts.multiline = false;
            absOpts.on_change = [this]() {
                try {
                    auto const v      = std::stoull(absoluteThresholdStr);
                    absoluteThreshold = static_cast<std::size_t>(v);
                } catch(std::exception const&) {}
            };
            absInput         = trackInput(ftxui::Input(&absoluteThresholdStr, "100", absOpts)
                                          | numericFilter(false));
            auto absParamRow = ftxui::Maybe(
              ftxui::Container::Horizontal({absInput}) | ftxui::Renderer([](ftxui::Element inner) {
                  return ftxui::hbox(
                    {ftxui::text(" count > ") | ftxui::color(Theme::Status::info()),
                     std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10)});
              }),
              [this] { return selectedOutlierMethod == 2; });

            // Enhanced preview: delegates to the memoized computeOutliers — no duplication
            auto previewRenderer = ftxui::Renderer([this] {
                auto const& r = currentOutliers();
                if(!r.valid) {
                    return ftxui::text("  (need ≥ 3 known locations to preview)")
                         | ftxui::color(Theme::Status::inactive());
                }
                auto const n = display.locationList.size();
                auto const w = r.wouldExclude.size();
                return ftxui::vbox(
                  {ftxui::text(fmt::format("  → {} of {} location{} would be excluded",
                                           w,
                                           n,
                                           n == 1 ? "" : "s"))
                     | ftxui::color(w > 0 ? Theme::Status::warning() : Theme::Status::inactive()),
                   ftxui::text(fmt::format("    cutoff: {}", r.cutoff))
                     | ftxui::color(Theme::Status::inactive())});
            });

            auto excludeButton = ftxui::Button(
              "🎯 Exclude Outliers",
              [this]() { autoExcludeNoisyLocations(); },
              createButtonStyle(Theme::Button::Background::debug(), Theme::Button::text()));

            auto noiseButtonRow
              = ftxui::Container::Horizontal({excludeButton})
              | ftxui::Renderer([this, excludeButton](ftxui::Element) {
                    ftxui::Element statusEl = ftxui::text("");
                    if(!noiseExcludeStatus.empty()) {
                        bool isError = noiseExcludeStatus.rfind("Need", 0) == 0
                                    || noiseExcludeStatus.rfind("No outliers", 0) == 0;
                        statusEl     = ftxui::text("  " + noiseExcludeStatus)
                                     | ftxui::color(isError ? Theme::Status::inactive()
                                                            : Theme::Status::success());
                    }
                    return ftxui::hbox({ftxui::text(" ") | ftxui::color(ftxui::Color::Default),
                                        excludeButton->Render(),
                                        std::move(statusEl)});
                });

            auto noiseInfoBox = ftxui::Renderer([] {
                return ftxui::vbox(
                  {ftxui::separator(),
                   ftxui::hbox(
                     {ftxui::text(" ℹ ") | ftxui::bold | ftxui::color(Theme::Status::info()),
                      ftxui::text("Noisy Locations") | ftxui::bold
                        | ftxui::color(Theme::Status::info())}),
                   ftxui::text("   Statistical: auto-detects outliers using inter-quartile range")
                     | ftxui::color(Theme::Status::inactive()),
                   ftxui::text("   Top Percent: exclude the N% most frequent locations")
                     | ftxui::color(Theme::Status::inactive()),
                   ftxui::text("   Count Limit: exclude locations seen more than N times")
                     | ftxui::color(Theme::Status::inactive())});
            });

            auto noisyExcludeSection
              = ftxui::Container::Vertical({methodToggle,
                                            iqrParamRow,
                                            topNParamRow,
                                            absParamRow,
                                            previewRenderer,
                                            noiseButtonRow})
              | ftxui::Renderer([noiseInfoBox](ftxui::Element inner) {
                    return ftxui::vbox({ftxui::text("🎯 Auto-Exclude Noisy Locations") | ftxui::bold
                                          | ftxui::color(Theme::Header::accent()) | ftxui::center,
                                        ftxui::separator(),
                                        std::move(inner),
                                        noiseInfoBox->Render()})
                         | ftxui::border;
                });

            // ── Time Filter section ──────────────────────────────────────────────
            {
                ftxui::InputOption o;
                o.multiline = false;
                o.on_change = [this]() {
                    double value = 0.0;
                    if(!minUcTimeStr.empty()) {
                        try {
                            value = std::stod(minUcTimeStr);
                        } catch(...) { return; }
                    }
                    store.updateUcTime([value](GuiEntryStore& s) {
                        s.ucTimeFilterEnabled = true;
                        s.ucTimeLiveMode      = false;
                        s.minUcTimeSec        = value;
                    });
                    store.scheduleRefilterDebounced();
                };
                ucTimeMinInput
                  = trackInput(ftxui::Input(&minUcTimeStr, "0.0", o) | numericFilter(true));
            }
            {
                ftxui::InputOption o;
                o.multiline = false;
                o.on_change = [this]() {
                    double value = std::numeric_limits<double>::infinity();
                    if(!maxUcTimeStr.empty()) {
                        try {
                            value = std::stod(maxUcTimeStr);
                        } catch(...) { return; }
                    }
                    store.updateUcTime([value](GuiEntryStore& s) {
                        s.ucTimeFilterEnabled = true;
                        s.ucTimeLiveMode      = false;
                        s.maxUcTimeSec        = value;
                    });
                    store.scheduleRefilterDebounced();
                };
                ucTimeMaxInput
                  = trackInput(ftxui::Input(&maxUcTimeStr, "∞", o) | numericFilter(true));
            }
            {
                ftxui::InputOption o;
                o.multiline = false;
                o.on_change = [this]() {
                    try {
                        auto const value = std::stod(ucTimeLiveWindowStr);
                        store.updateUcTime(
                          [value](GuiEntryStore& s) { s.ucTimeLiveWindowSecs = value; });
                    } catch(...) {}
                };
                ucTimeLiveWindowInput
                  = trackInput(ftxui::Input(&ucTimeLiveWindowStr, "10", o) | numericFilter(true));
            }

            auto ucTimeCheckbox = FunctionCheckbox(
              "Enable",
              [this]() { return display.ucTimeFilterEnabled; },
              [this]() {
                  store.updateUcTime([](GuiEntryStore& s) {
                      s.ucTimeFilterEnabled = !s.ucTimeFilterEnabled;
                      if(!s.ucTimeFilterEnabled) { s.ucTimeLiveMode = false; }
                      s.requestRefilterLocked();
                  });
              });

            auto makeStaticPreset = [this](char const* label, double from, double to) {
                return ftxui::Button(
                  label,
                  [this, from, to]() {
                      minUcTimeStr = fmt::format("{:.1f}", from);
                      maxUcTimeStr = std::isinf(to) ? "" : fmt::format("{:.1f}", to);
                      store.updateUcTime([from, to](GuiEntryStore& s) {
                          s.ucTimeLiveMode      = false;
                          s.ucTimeFilterEnabled = true;
                          s.minUcTimeSec        = from;
                          s.maxUcTimeSec        = to;
                          s.requestRefilterLocked();
                      });
                  },
                  createButtonStyle(Theme::Button::Background::build(), Theme::Button::text()));
            };
            auto makeLivePreset = [this](char const* label, double secs) {
                return ftxui::Button(
                  label,
                  [this, secs]() {
                      ucTimeLiveWindowStr = fmt::format("{:.0f}", secs);
                      maxUcTimeStr.clear();
                      store.updateUcTime([this, secs](GuiEntryStore& s) {
                          s.ucTimeLiveWindowSecs = secs;
                          s.ucTimeLiveMode       = true;
                          s.ucTimeFilterEnabled  = true;
                          s.minUcTimeSec         = std::max(0.0, s.ucTimeDataMax - secs);
                          s.maxUcTimeSec         = std::numeric_limits<double>::infinity();
                          minUcTimeStr           = fmt::format("{:.1f}", s.minUcTimeSec);
                          s.requestRefilterLocked();
                      });
                  },
                  createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));
            };

            auto resetUcTimeBtn = ftxui::Button(
              " ✕ Reset ",
              [this]() {
                  minUcTimeStr.clear();
                  maxUcTimeStr.clear();
                  store.updateUcTime([](GuiEntryStore& s) {
                      s.ucTimeFilterEnabled = false;
                      s.ucTimeLiveMode      = false;
                      s.minUcTimeSec        = 0.0;
                      s.maxUcTimeSec        = std::numeric_limits<double>::infinity();
                      s.requestRefilterLocked();
                  });
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto inputWidth     = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8);
            auto liveInputWidth = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6);

            auto ucTimeSection
              = ftxui::Container::Vertical(
                  {ftxui::Container::Horizontal({ucTimeCheckbox, resetUcTimeBtn}),
                   ftxui::Container::Horizontal(
                     {ftxui::Renderer([]() { return ftxui::text(" From: "); }),
                      ucTimeMinInput | inputWidth,
                      ftxui::Renderer([]() { return ftxui::text(" s   To: "); }),
                      ucTimeMaxInput | inputWidth,
                      ftxui::Renderer([]() { return ftxui::text(" s"); })}),
                   ftxui::Container::Horizontal({makeStaticPreset(" First 10 s ", 0.0, 10.0),
                                                 makeStaticPreset(" First 30 s ", 0.0, 30.0),
                                                 makeStaticPreset(" First 1 m ", 0.0, 60.0)}),
                   ftxui::Container::Horizontal(
                     {makeLivePreset(" ⟳ 10 s ", 10.0),
                      makeLivePreset(" ⟳ 30 s ", 30.0),
                      makeLivePreset(" ⟳ 1 m ", 60.0),
                      ftxui::Renderer([]() { return ftxui::text("  Custom: "); }),
                      ucTimeLiveWindowInput | liveInputWidth,
                      ftxui::Renderer([]() { return ftxui::text(" s"); })})})
              | ftxui::Renderer([this](ftxui::Element inner) {
                    auto dataStr = std::isinf(display.ucTimeDataMin)
                                   ? std::string{"--"}
                                   : fmt::format("{:.1f} – {:.1f} s",
                                                 display.ucTimeDataMin,
                                                 display.ucTimeDataMax);
                    auto liveStr
                      = display.ucTimeLiveMode
                        ? fmt::format(" ⟳ live: last {:.0f} s", display.ucTimeLiveWindowSecs)
                        : std::string{};
                    return ftxui::vbox(
                             {ftxui::text("⏱ UC Time Filter") | ftxui::bold
                                | ftxui::color(Theme::Header::primary()) | ftxui::center,
                              ftxui::separator(),
                              std::move(inner),
                              ftxui::hbox(
                                {ftxui::text(" Data: ") | ftxui::color(Theme::Text::normal()),
                                 ftxui::text(dataStr) | ftxui::color(Theme::Header::accent()),
                                 ftxui::text(liveStr) | ftxui::color(Theme::Text::normal())})})
                         | ftxui::border;
                });

            auto timeFilterSection = ucTimeSection;

            auto clearLogButton = ftxui::Button(
              "❌ Clear All Log Entries",
              [this]() { store.clearAll(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto clearBootButton = ftxui::Button(
              "🔄 Clear Log Entries Before Last Boot",
              [this]() { store.clearBeforeLastBoot(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({clearButton | ftxui::flex,
                                             clearLogButton | ftxui::flex,
                                             clearBootButton | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               timeFilterSection,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Vertical(mainComponents)
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🔍 Filter Settings") | ftxui::bold
                                             | ftxui::color(Theme::Header::primary())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           std::move(inner)});
                   }),
               ftxui::Container::Horizontal({saveLoadSection, noisyExcludeSection})
                 | ftxui::Renderer([saveLoadSection, noisyExcludeSection](ftxui::Element) {
                       return ftxui::hbox({saveLoadSection->Render() | ftxui::flex,
                                           noisyExcludeSection->Render() | ftxui::flex});
                   }),
               exportSection});
        }

        ftxui::Component getSettingsComponent() {
            // ── Section A: Display ────────────────────────────────────────────────
            auto resetButton = ftxui::Button(
              "🔄 Reset to Defaults",
              [this]() {
                  showSysTime        = true;
                  showFunctionName   = false;
                  showUcTime         = true;
                  showLocation       = true;
                  showChannel        = true;
                  showLogLevel       = true;
                  showMetricString   = false;
                  showTypenameString = false;
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            // toggles aligned horizontally in two rows to keep the section flat
            auto const checkboxWidth  = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 24);
            auto       displaySection = ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({ftxui::Renderer([]() {
                                                 return ftxui::text("🎨 Display  ") | ftxui::bold
                                                      | ftxui::color(Theme::Header::accent());
                                             }),
                                             resetButton}),
               ftxui::Container::Horizontal(
                 {ftxui::Renderer([]() { return ftxui::text("  "); }),
                  ftxui::Checkbox("⏰ System Time", &showSysTime) | checkboxWidth,
                  ftxui::Checkbox("🕐 Target Time", &showUcTime) | checkboxWidth,
                  ftxui::Checkbox("📡 Log Channel", &showChannel) | checkboxWidth,
                  ftxui::Checkbox("📊 Log Level", &showLogLevel) | checkboxWidth}),
               ftxui::Container::Horizontal(
                 {ftxui::Renderer([]() { return ftxui::text("  "); }),
                  ftxui::Checkbox("📍 Source Location", &showLocation) | checkboxWidth,
                  ftxui::Checkbox("🔍 Function Names", &showFunctionName) | checkboxWidth,
                  ftxui::Checkbox("📊 Metric Strings", &showMetricString) | checkboxWidth,
                  ftxui::Checkbox("🔤 Typenames", &showTypenameString) | checkboxWidth})});

            // ── Section B: File Logging ───────────────────────────────────────────
            ftxui::InputOption logDirOpts;
            logDirOpts.multiline = false;
            logDirInputComponent
              = trackInput(ftxui::Input(&logDirInput, "path/to/log/dir", logDirOpts));

            auto logToggleBtn = ftxui::Button(
              " Toggle ",
              [this]() {
                  logFileEnabled = !logFileEnabled;
                  if(onLogFileEnable) { onLogFileEnable(logFileEnabled); }
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto logApplyBtn = ftxui::Button(
              " Apply ",
              [this]() {
                  if(!onLogDirChange || logDirInput.empty()) { return; }
                  pendingActions.push_back([this, dir = logDirInput]() { onLogDirChange(dir); });
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto logSection = ftxui::Container::Vertical(
              {ftxui::Renderer([this]() {
                   auto const enabled    = logFileEnabled;
                   auto const statusText = [this, enabled]() -> std::string {
                       if(!enabled) { return "Disabled ○"; }
                       switch(logFileStatus) {
                       case LogFileStatus::Active:     return "Active ●";
                       case LogFileStatus::Error:      return "Error ✘";
                       case LogFileStatus::NotStarted: return "Not Started ○";
                       }
                       return "Not Started ○";
                   }();
                   auto const statusColor = [this, enabled]() {
                       if(!enabled) { return Theme::Text::normal(); }
                       switch(logFileStatus) {
                       case LogFileStatus::Active:     return Theme::Status::success();
                       case LogFileStatus::Error:      return Theme::Status::error();
                       case LogFileStatus::NotStarted: return Theme::Text::normal();
                       }
                       return Theme::Text::normal();
                   }();
                   return ftxui::vbox(
                     {ftxui::hbox(
                        {ftxui::text("📄 File Logging  ") | ftxui::bold
                           | ftxui::color(Theme::Header::accent()),
                         ftxui::text(statusText) | ftxui::color(statusColor) | ftxui::bold}),
                      ftxui::hbox(
                        {ftxui::text("  File: ") | ftxui::bold,
                         ftxui::text(!logFileCurrentPath.empty() ? logFileCurrentPath : "—")
                           | ftxui::color(logFileStatus == LogFileStatus::Active
                                            ? Theme::Status::info()
                                            : Theme::Text::normal())}),
                      logFileStatus == LogFileStatus::Error
                        ? ftxui::text(fmt::format("  ✘  Cannot write to: {}", logFileCurrentPath))
                            | ftxui::color(Theme::Status::error())
                        : ftxui::text("")});
               }),
               ftxui::Container::Horizontal(
                 {logToggleBtn,
                  ftxui::Renderer([]() { return ftxui::text("  Directory: "); }),
                  logDirInputComponent | ftxui::border | ftxui::flex,
                  ftxui::Renderer([]() { return ftxui::text("  "); }),
                  logApplyBtn})});

            // ── Section C: Network / TCP ──────────────────────────────────────────
            ftxui::InputOption portInputOpts;
            portInputOpts.multiline = false;
            tcpPortInputComponent   = trackInput(
              ftxui::Input(&tcpPortInput, "1024–65535", portInputOpts) | numericFilter(false)
              | ftxui::CatchEvent([this](ftxui::Event const& e) {
                    return e.is_character() && tcpPortInput.size() >= 5;
                }));

            auto tcpToggleBtn = ftxui::Button(
              " Toggle ",
              [this]() {
                  tcpEnabled = !tcpEnabled;
                  if(onTcpEnable) { onTcpEnable(tcpEnabled); }
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto portApplyBtn = ftxui::Button(
              " Apply ",
              [this]() {
                  if(!onTcpPortChange) { return; }
                  try {
                      auto const val = std::stoul(tcpPortInput);
                      if(val < 1024 || val > 65535) { return; }
                      tcpEnabled = true;
                      onTcpPortChange(static_cast<std::uint16_t>(val));
                  } catch(...) {}
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            // ── Global socket bind address (metrics + all duplex channels) ────────
            ftxui::InputOption bindAddrOpts;
            bindAddrOpts.multiline = false;
            bindAddressInputComponent
              = trackInput(ftxui::Input(&bindAddressInput, "127.0.0.1", bindAddrOpts));

            auto bindApplyBtn = ftxui::Button(
              " Apply ",
              [this]() {
                  if(!onNetworkBindAddressChange || bindAddressInput.empty()) { return; }
                  if(onNetworkBindAddressChange(bindAddressInput)) {
                      networkBindAddress = bindAddressInput;
                      bindAddressStatus.clear();
                  } else {
                      bindAddressStatus = fmt::format("invalid address: {}", bindAddressInput);
                  }
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto makeBindPresetBtn = [this](std::string label, std::string address) {
                return ftxui::Button(
                  std::move(label),
                  [this, addr = std::move(address)]() {
                      if(!onNetworkBindAddressChange) { return; }
                      if(onNetworkBindAddressChange(addr)) {
                          networkBindAddress = addr;
                          bindAddressInput   = addr;
                          bindAddressStatus.clear();
                      }
                  },
                  createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));
            };
            auto bindLoopbackBtn = makeBindPresetBtn(" Loopback ", "127.0.0.1");
            auto bindAllBtn      = makeBindPresetBtn(" All Interfaces ", "0.0.0.0");

            auto bindAddrSection = ftxui::Container::Vertical(
              {ftxui::Renderer([this]() {
                   bool const exposed = !isLoopbackAddress(networkBindAddress);
                   return ftxui::vbox(
                     {ftxui::hbox({ftxui::text("🔌 Socket Bind Address  ") | ftxui::bold
                                     | ftxui::color(Theme::Header::accent()),
                                   ftxui::text(networkBindAddress)
                                     | ftxui::color(exposed ? Theme::Status::error()
                                                            : Theme::Status::success())
                                     | ftxui::bold}),
                      exposed ? ftxui::text("  ⚠  Exposed to the network — the duplex ports are an "
                                            "unauthenticated shell into the target.")
                                  | ftxui::color(Theme::Status::error())
                              : ftxui::text("  ● Loopback only — reachable from this host.")
                                  | ftxui::color(Theme::Status::inactive()),
                      bindAddressStatus.empty() ? ftxui::text("")
                                                : ftxui::text("  ✘  " + bindAddressStatus)
                                                    | ftxui::color(Theme::Status::error())});
               }),
               ftxui::Container::Horizontal(
                 {ftxui::Renderer([]() { return ftxui::text("  Address: "); }),
                  bindAddressInputComponent | ftxui::border | ftxui::flex,
                  ftxui::Renderer([]() { return ftxui::text("  "); }),
                  bindApplyBtn,
                  ftxui::Renderer([]() { return ftxui::text("  "); }),
                  bindLoopbackBtn,
                  ftxui::Renderer([]() { return ftxui::text("  "); }),
                  bindAllBtn})});

            auto netSection = ftxui::Container::Vertical(
              {ftxui::Renderer([this]() {
                   auto const statusText = [this]() -> std::string {
                       if(!tcpEnabled) { return "Disabled ○"; }
                       switch(tcpPortStatus) {
                       case TcpPortStatus::Active:       return "Active ●";
                       case TcpPortStatus::PortOccupied: return "Port Occupied ⚠";
                       case TcpPortStatus::NotStarted:   return "Not Started ○";
                       }
                       return "Not Started ○";
                   }();
                   auto const statusColor = [this]() {
                       if(!tcpEnabled) { return Theme::Text::normal(); }
                       switch(tcpPortStatus) {
                       case TcpPortStatus::Active:       return Theme::Status::success();
                       case TcpPortStatus::PortOccupied: return Theme::Status::error();
                       case TcpPortStatus::NotStarted:   return Theme::Text::normal();
                       }
                       return Theme::Text::normal();
                   }();
                   auto const clientCount = tcpClientCountGetter ? tcpClientCountGetter() : 0u;

                   bool inputValid = false;
                   try {
                       auto const val = std::stoul(tcpPortInput);
                       inputValid     = (val >= 1024 && val <= 65535);
                   } catch(...) {}
                   bool const inputChanged
                     = !tcpPortInput.empty()
                    && tcpPortInput != std::to_string(static_cast<unsigned>(tcpCurrentPort));

                   return ftxui::vbox(
                     {ftxui::hbox(
                        {ftxui::text("🌐 TCP Metrics  ") | ftxui::bold
                           | ftxui::color(Theme::Header::accent()),
                         ftxui::text(statusText) | ftxui::color(statusColor) | ftxui::bold,
                         ftxui::text("  "),
                         tcpCurrentPort > 0
                           ? ftxui::text(fmt::format("{}:{}", networkBindAddress, tcpCurrentPort))
                               | ftxui::color(Theme::Status::info())
                           : ftxui::text("—") | ftxui::color(Theme::Text::normal()),
                         ftxui::text("  clients ") | ftxui::bold,
                         ftxui::text(std::to_string(clientCount))
                           | ftxui::color(clientCount > 0 ? Theme::Status::success()
                                                          : Theme::Text::normal())}),
                      tcpPortStatus == TcpPortStatus::PortOccupied
                        ? ftxui::text("  ⚠  Port is in use — enter a different port and Apply.")
                            | ftxui::color(Theme::Status::error())
                        : ftxui::text(""),
                      inputChanged && !inputValid
                        ? ftxui::text("  ✘  Port out of range (must be 1024–65535).")
                            | ftxui::color(Theme::Status::error())
                        : ftxui::text("")});
               }),
               ftxui::Container::Horizontal(
                 {tcpToggleBtn,
                  ftxui::Renderer([]() { return ftxui::text("  New port: "); }),
                  tcpPortInputComponent | ftxui::border | ftxui::flex,
                  ftxui::Renderer([]() { return ftxui::text("  "); }),
                  portApplyBtn})});

            // ── Combine all sections, each in its own frame like the Debugger tab ─
            return ftxui::Container::Vertical({displaySection | ftxui::border,
                                               logSection | ftxui::border,
                                               bindAddrSection | ftxui::border,
                                               netSection | ftxui::border,
                                               getDuplexSection() | ftxui::border});
        }

        static constexpr int SettingsTabIndex = 3;

        ftxui::Component getDuplexSection() {
            ftxui::InputOption portInputOpts;
            portInputOpts.multiline = false;

            duplexBasePortInputComponent = trackInput(
              ftxui::Input(&duplexBasePortInput, "1024–65535", portInputOpts) | numericFilter(false)
              | ftxui::CatchEvent([this](ftxui::Event const& e) {
                    return e.is_character() && duplexBasePortInput.size() >= 5;
                }));

            auto basePortApplyBtn = ftxui::Button(
              " Apply to all ",
              [this]() {
                  if(!onDuplexBasePortChange) { return; }
                  try {
                      auto const val = std::stoul(duplexBasePortInput);
                      if(val < 1024 || val > 65535 - GUI_Constants::MaxDuplexChannels) { return; }
                      onDuplexBasePortChange(static_cast<std::uint16_t>(val));
                  } catch(...) {}
              },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto getInfos = [this]() {
                return duplexInfoGetter ? duplexInfoGetter()
                                        : std::vector<uc_log::detail::DuplexChannelInfo>{};
            };

            auto headerRenderer = ftxui::Renderer([getInfos]() {
                auto const infos = getInfos();
                return ftxui::hbox({ftxui::text("🔁 Duplex Channels  ") | ftxui::bold
                                      | ftxui::color(Theme::Header::accent()),
                                    infos.empty()
                                      ? ftxui::text("none reported by the target")
                                          | ftxui::color(Theme::Text::normal())
                                      : ftxui::text("one TCP client per channel, first wins")
                                          | ftxui::color(Theme::Status::info())});
            });

            std::vector<ftxui::Component> channelRows;
            for(std::size_t i{}; i < GUI_Constants::MaxDuplexChannels; ++i) {
                duplexPortInputComponents[i] = trackInput(
                  ftxui::Input(&duplexPortInputs[i], "port", portInputOpts) | numericFilter(false)
                  | ftxui::CatchEvent([this, i](ftxui::Event const& e) {
                        return e.is_character() && duplexPortInputs[i].size() >= 5;
                    }));

                auto toggleBtn = ftxui::Button(
                  " Toggle ",
                  [this, getInfos, i]() {
                      if(!onDuplexEnable) { return; }
                      auto const infos = getInfos();
                      if(i < infos.size()) { onDuplexEnable(i, !infos[i].enabled); }
                  },
                  createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

                auto applyBtn = ftxui::Button(
                  " Apply ",
                  [this, i]() {
                      if(!onDuplexPortChange) { return; }
                      try {
                          auto const val = std::stoul(duplexPortInputs[i]);
                          if(val < 1024 || val > 65535) { return; }
                          onDuplexPortChange(i, static_cast<std::uint16_t>(val));
                      } catch(...) {}
                  },
                  createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

                auto infoRenderer = ftxui::Renderer([this, getInfos, i]() {
                    auto const infos = getInfos();
                    if(i >= infos.size()) { return ftxui::text(""); }
                    auto const& info = infos[i];
                    // do not reseed while the user is editing, it would fight the clearing
                    if(duplexPortInputs[i].empty()
                       && !(duplexPortInputComponents[i]
                            && duplexPortInputComponents[i]->Focused()))
                    {
                        duplexPortInputs[i] = std::to_string(static_cast<unsigned>(info.port));
                    }
                    auto const statusText = [&]() -> std::string {
                        if(!info.enabled) { return "Disabled ○"; }
                        switch(info.status) {
                        case TcpPortStatus::Active:       return "Active ●";
                        case TcpPortStatus::PortOccupied: return "Port Occupied ⚠";
                        case TcpPortStatus::NotStarted:   return "Not Started ○";
                        }
                        return "Not Started ○";
                    }();
                    auto const statusColor = [&]() {
                        if(!info.enabled) { return Theme::Text::normal(); }
                        switch(info.status) {
                        case TcpPortStatus::Active:       return Theme::Status::success();
                        case TcpPortStatus::PortOccupied: return Theme::Status::error();
                        case TcpPortStatus::NotStarted:   return Theme::Text::normal();
                        }
                        return Theme::Text::normal();
                    }();
                    return ftxui::hbox(
                      {ftxui::text(fmt::format("  {}  ", info.ordinal)) | ftxui::bold,
                       ftxui::text(info.name) | ftxui::bold | ftxui::color(Theme::Header::accent()),
                       info.hostToTargetOnly ? ftxui::text("  (host→target only)")
                                                 | ftxui::color(Theme::Status::warning())
                                             : ftxui::text(""),
                       ftxui::text("  "),
                       ftxui::text(statusText) | ftxui::color(statusColor) | ftxui::bold,
                       ftxui::text("  "),
                       ftxui::text(fmt::format("{}:{}", networkBindAddress, info.port))
                         | ftxui::color(Theme::Status::info()),
                       ftxui::text("  client ") | ftxui::bold,
                       ftxui::text(info.connected ? "●" : "○")
                         | ftxui::color(info.connected ? Theme::Status::success()
                                                       : Theme::Text::normal()),
                       ftxui::text("  "),
                       ftxui::text(fmt::format("→uc {}  uc→ {}",
                                               FTXUIGui::formatBytes(info.bytesToTarget),
                                               FTXUIGui::formatBytes(info.bytesFromTarget)))
                         | ftxui::color(Theme::Status::info()),
                       info.bytesDropped != 0
                         ? ftxui::text(
                             fmt::format("  dropped {}", FTXUIGui::formatBytes(info.bytesDropped)))
                             | ftxui::color(Theme::Status::error())
                         : ftxui::text("")});
                });

                auto row = ftxui::Container::Vertical(
                  {infoRenderer,
                   ftxui::Container::Horizontal(
                     {ftxui::Renderer([]() { return ftxui::text("      "); }),
                      toggleBtn,
                      ftxui::Renderer([]() { return ftxui::text("  New port: "); }),
                      duplexPortInputComponents[i] | ftxui::border
                        | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 9),
                      ftxui::Renderer([]() { return ftxui::text("  "); }),
                      applyBtn})});

                channelRows.push_back(
                  ftxui::Maybe(row, [getInfos, i]() { return i < getInfos().size(); }));
            }

            auto basePortRow = ftxui::Container::Horizontal(
              {ftxui::Renderer([]() { return ftxui::text("  Base port: ") | ftxui::bold; }),
               duplexBasePortInputComponent | ftxui::border
                 | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 13),
               ftxui::Renderer([]() { return ftxui::text("  "); }),
               basePortApplyBtn});

            std::vector<ftxui::Component> components{headerRenderer, basePortRow};
            for(auto& row : channelRows) { components.push_back(std::move(row)); }
            return ftxui::Container::Vertical(std::move(components));
        }

        ftxui::Component getHelpComponent() {
            return ftxui::Renderer([]() {
                return ftxui::vbox(
                  {ftxui::text("❓ Help - Keyboard Shortcuts") | ftxui::bold
                     | ftxui::color(Theme::Header::primary()) | ftxui::center,
                   ftxui::separator(),
                   ftxui::text(""),
                   ftxui::text("📑 Tab Navigation") | ftxui::bold
                     | ftxui::color(Theme::Header::accent()),
                   ftxui::text("  1       - Logs tab"),
                   ftxui::text("  2       - Build tab"),
                   ftxui::text("  3       - Filter tab"),
                   ftxui::text("  4       - Settings tab (Display, File Logging, Network, Duplex)"),
                   ftxui::text("  5       - Debugger tab"),
                   ftxui::text("  6       - Metrics tab"),
                   ftxui::text("  7       - Status tab"),
                   ftxui::text("  8       - Statistics tab"),
                   ftxui::text("  9       - Help tab"),
                   ftxui::text(""),
                   ftxui::text("🔧 Actions") | ftxui::bold | ftxui::color(Theme::Header::accent()),
                   ftxui::text("  q       - Quit application"),
                   ftxui::text("  r       - Reset target"),
                   ftxui::text("  f       - Flash target"),
                   ftxui::text("  b       - Start build"),
                   ftxui::text("  Shift+F - Build and flash"),
                   ftxui::text("  /       - Show and focus the log search field"),
                   ftxui::text("            (Esc hides it again when it is empty)"),
                   ftxui::text(""),
                   ftxui::text("📜 Log View") | ftxui::bold | ftxui::color(Theme::Header::accent()),
                   ftxui::text("  j / k or mouse wheel   - Scroll down / up"),
                   ftxui::text("  h / l or ←/→           - Scroll horizontally"),
                   ftxui::text("  PageUp / PageDown      - Scroll a page"),
                   ftxui::text("  Home / End             - Jump to first / last entry"),
                   ftxui::text("                           (End re-enables follow mode)"),
                   ftxui::text("  y                      - Copy selected line (OSC 52)"),
                   ftxui::text("  Search: plain text is case-insensitive, prefix with re:"),
                   ftxui::text("          for a regular expression; matches filter the view"),
                   ftxui::text(""),
                   ftxui::text("💡 Tips") | ftxui::bold | ftxui::color(Theme::Header::warning()),
                   ftxui::text("  • Use Tab/Shift+Tab to navigate between UI elements"),
                   ftxui::text("  • Use arrow keys to navigate lists and menus"),
                   ftxui::text("  • Number keys work globally except when typing in text fields"),
                   ftxui::text("")});
            });
        }

        ftxui::Component getStatisticsComponent() {
            auto resetButton = ftxui::Button(
              "🔄 Reset Statistics",
              [this]() {
                  statistics = Statistics{};
                  store.resetLogStats();
              },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            return ftxui::Container::Vertical(
              {resetButton,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Renderer([this]() {
                   auto const now    = std::chrono::system_clock::now();
                   auto const uptime = std::chrono::duration_cast<std::chrono::seconds>(
                     now - statistics.sessionStartTime);
                   auto const hours   = uptime.count() / 3600;
                   auto const minutes = (uptime.count() % 3600) / 60;
                   auto const seconds = uptime.count() % 60;

                   return ftxui::vbox(
                     {ftxui::text("📊 Session Statistics") | ftxui::bold
                        | ftxui::color(Theme::Header::primary()) | ftxui::center,
                      ftxui::separator(),
                      ftxui::text(""),

                      ftxui::text("⏱️ Session Information") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Session Uptime: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}h {}m {}s", hours, minutes, seconds))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::text(""),

                      ftxui::text("🔗 JLink Connection") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Reconnections: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}", statistics.jlinkReconnectionCount))
                                     | ftxui::color(statistics.jlinkReconnectionCount > 0
                                                      ? Theme::Status::warning()
                                                      : Theme::Status::success())}),
                      ftxui::hbox(
                        {ftxui::text("  Disconnections: ") | ftxui::bold,
                         ftxui::text(fmt::format("{}", statistics.jlinkDisconnectionCount))
                           | ftxui::color(statistics.jlinkDisconnectionCount > 0
                                            ? Theme::Status::warning()
                                            : Theme::Status::success())}),
                      ftxui::hbox(
                        {ftxui::text("  Current State: ") | ftxui::bold,
                         ftxui::text(statistics.lastJLinkState ? "Connected ✓" : "Disconnected ✗")
                           | ftxui::color(statistics.lastJLinkState ? Theme::Status::success()
                                                                    : Theme::Status::error())}),
                      ftxui::text(""),

                      ftxui::text("🔨 Build Statistics") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Total Builds: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}", statistics.totalBuildsStarted))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::hbox({ftxui::text("  Successful: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}", statistics.successfulBuilds))
                                     | ftxui::color(Theme::Status::success())}),
                      ftxui::hbox(
                        {ftxui::text("  Failed: ") | ftxui::bold,
                         ftxui::text(fmt::format("{}", statistics.failedBuilds))
                           | ftxui::color(statistics.failedBuilds > 0 ? Theme::Status::error()
                                                                      : Theme::Status::success())}),
                      ftxui::hbox(
                        {ftxui::text("  Success Rate: ") | ftxui::bold,
                         ftxui::text(
                           statistics.totalBuildsStarted > 0
                             ? fmt::format("{:.1f}%",
                                           100.0 * static_cast<double>(statistics.successfulBuilds)
                                             / static_cast<double>(statistics.totalBuildsStarted))
                             : "N/A")
                           | ftxui::color(Theme::Status::info())}),
                      ftxui::text(""),

                      ftxui::text("🎯 Target Control") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Flash Count: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}", statistics.flashCount))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::hbox({ftxui::text("  Reset Requests: ") | ftxui::bold,
                                   ftxui::text(fmt::format("{}", statistics.resetRequestCount))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::hbox(
                        {ftxui::text("  Resets Detected: ") | ftxui::bold,
                         ftxui::text(fmt::format("{}", display.logStats.detectedResetCount))
                           | ftxui::color(display.logStats.detectedResetCount > 0
                                            ? Theme::Status::warning()
                                            : Theme::Status::info())}),
                      ftxui::text(""),

                      ftxui::text("📝 Log Statistics") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Retained Logs: ") | ftxui::bold,
                                   ftxui::text(FTXUIGui::formatNumber(
                                     static_cast<std::uint32_t>(display.originalLogCount)))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::hbox(
                        {ftxui::text("  Trimmed Logs: ") | ftxui::bold,
                         ftxui::text(FTXUIGui::formatNumber(
                           static_cast<std::uint32_t>(display.trimmedLogCount)))
                           | ftxui::color(display.trimmedLogCount > 0 ? Theme::Status::warning()
                                                                      : Theme::Status::info())}),
                      ftxui::hbox({ftxui::text("  Unparsed Logs: ") | ftxui::bold,
                                   ftxui::text(FTXUIGui::formatNumber(static_cast<std::uint32_t>(
                                     display.logStats.parseFailureCount)))
                                     | ftxui::color(display.logStats.parseFailureCount > 0
                                                      ? Theme::Status::error()
                                                      : Theme::Status::info())}),
                      ftxui::hbox(
                        {ftxui::text("  Peak Rate: ") | ftxui::bold,
                         ftxui::text(fmt::format("{} logs/sec", display.logStats.peakLogsPerSecond))
                           | ftxui::color(Theme::Status::warning())}),
                      ftxui::text(""),

                      ftxui::text("📡 Data Transfer") | ftxui::bold
                        | ftxui::color(Theme::Header::accent()),
                      ftxui::hbox({ftxui::text("  Max Bytes Read: ") | ftxui::bold,
                                   ftxui::text(FTXUIGui::formatBytes(
                                     static_cast<std::uint32_t>(statistics.maxBytesRead)))
                                     | ftxui::color(Theme::Status::info())}),
                      ftxui::hbox({ftxui::text("  Max Overflows: ") | ftxui::bold,
                                   ftxui::text(FTXUIGui::formatNumber(
                                     static_cast<std::uint32_t>(statistics.maxOverflowCount)))
                                     | ftxui::color(statistics.maxOverflowCount > 0
                                                      ? Theme::Status::error()
                                                      : Theme::Status::success())})});
               })});
        }

        template<typename Reader>
        ftxui::Component getDebuggerComponent(Reader& rttReader) {
            auto resetTargetBtn = ftxui::Button(
              "🔄 Reset Target [r]",
              [this, &rttReader]() { resetTargetWithStats(rttReader); },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto resetDebuggerBtn = ftxui::Button(
              "🔌 Reset Debugger",
              [&rttReader]() { rttReader.resetJLink(); },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            auto flashBtn = ftxui::Button(
              "⚡ Flash Target [f]",
              [this, &rttReader]() { flashWithStats(rttReader); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto goBtn = ftxui::Button(
              "▶️ Go",
              [&rttReader]() { rttReader.continueTarget(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto haltBtn = ftxui::Button(
              "⏸️ Halt",
              [&rttReader]() { rttReader.haltTarget(); },
              createButtonStyle(Theme::Button::Background::danger(), Theme::Button::text()));

            auto clearBreakpointsBtn = ftxui::Button(
              "🚫 Clear Breakpoints",
              [&rttReader]() { rttReader.clearAllBreakpointsTarget(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto resetTypeRadiobox
              = ftxui::Radiobox(std::vector<std::string>{"0 - Normal", "1 - Core", "2 - ResetPin"},
                                &selectedResetType);

            auto resetTypeSelector
              = ftxui::Container::Vertical(
                  {ftxui::Renderer([]() {
                       return ftxui::text("🔧 Reset Type") | ftxui::bold
                            | ftxui::color(Theme::Header::accent());
                   }),
                   resetTypeRadiobox,
                   ftxui::Button(
                     "✓ Apply Reset Type",
                     [this, &rttReader]() {
                         rttReader.setResetType(static_cast<std::uint8_t>(selectedResetType));
                     },
                     createButtonStyle(Theme::Button::Background::positive(),
                                       Theme::Button::text()))})
              | ftxui::border;

            auto statusDisplay = ftxui::Renderer([&rttReader]() {
                auto const rttStatus = rttReader.getStatus();

                return ftxui::vbox(
                  {ftxui::text("📊 Debugger Status") | ftxui::bold
                     | ftxui::color(Theme::Header::primary()) | ftxui::center,
                   ftxui::separator(),

                   ftxui::hbox({ftxui::text("Connection: ") | ftxui::bold,
                                ftxui::text(rttStatus.isRunning != 0 ? "✓ Active" : "✗ Inactive")
                                  | ftxui::color(rttStatus.isRunning != 0 ? Theme::Status::active()
                                                                          : Theme::Status::error())
                                  | ftxui::bold}),

                   ftxui::hbox(
                     {ftxui::text("Overflows: ") | ftxui::bold,
                      ftxui::text(FTXUIGui::formatNumber(
                        static_cast<std::uint32_t>(rttStatus.hostOverflowCount)))
                        | ftxui::color(rttStatus.hostOverflowCount == 0 ? Theme::Status::success()
                                                                        : Theme::Status::error())
                        | ftxui::bold}),

                   ftxui::hbox({ftxui::text("Read: ") | ftxui::bold,
                                ftxui::text(FTXUIGui::formatBytes(rttStatus.numBytesRead))
                                  | ftxui::color(Theme::Status::info())}),

                   ftxui::hbox(
                     {ftxui::text("Buffers: ") | ftxui::bold,
                      ftxui::text(
                        fmt::format("↑{} ↓{}", rttStatus.numUpBuffers, rttStatus.numDownBuffers))
                        | ftxui::color(Theme::Status::warning())})});
            });

            auto connTypeRadio
              = ftxui::Radiobox(std::vector<std::string>{"USB (local)", "IP (remote)"},
                                &connectionTypeSelection);

            ftxui::InputOption ipAddressOpts;
            ipAddressOpts.multiline = false;
            ipAddressInputComponent
              = trackInput(ftxui::Input(&ipAddressInput, "host or IP address...", ipAddressOpts));
            auto ipInputMaybe = ftxui::Maybe(ipAddressInputComponent | ftxui::flex,
                                             [this]() { return connectionTypeSelection == 1; });

            auto applyConnBtn = ftxui::Button(
              "Apply Connection",
              [this, &rttReader]() {
                  rttReader.setHost(connectionTypeSelection == 0 ? "" : ipAddressInput);
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto connPanel
              = ftxui::Container::Vertical({ftxui::Renderer([]() {
                                                return ftxui::text("Connection") | ftxui::bold
                                                     | ftxui::color(Theme::Header::accent());
                                            }),
                                            connTypeRadio,
                                            ipInputMaybe,
                                            applyConnBtn})
              | ftxui::border;

            ftxui::InputOption noLogTimeoutOpts;
            noLogTimeoutOpts.multiline = false;
            noLogTimeoutOpts.on_change = [this, &rttReader]() {
                try {
                    auto const v = std::stoi(noLogTimeoutStr);
                    if(v > 0) { rttReader.setNoLogTimeout(static_cast<std::uint32_t>(v)); }
                } catch(std::exception const&) {}
            };
            noLogTimeoutInput = trackInput(ftxui::Input(&noLogTimeoutStr, "15", noLogTimeoutOpts)
                                           | numericFilter(false));
            auto timeoutRow
              = ftxui::Container::Horizontal({noLogTimeoutInput})
              | ftxui::Renderer([](ftxui::Element inner) {
                    return ftxui::hbox(
                      {ftxui::text("No-log reconnect timeout (s): ")
                         | ftxui::color(Theme::Status::info()),
                       std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6)});
                });

            return ftxui::Container::Vertical(
              {connPanel,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               timeoutRow,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Horizontal({resetTargetBtn | ftxui::flex,
                                             resetDebuggerBtn | ftxui::flex,
                                             flashBtn | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Horizontal(
                 {goBtn | ftxui::flex, haltBtn | ftxui::flex, clearBreakpointsBtn | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               resetTypeSelector,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               statusDisplay});
        }

        ftxui::Component
        generateTabsComponent(std::vector<std::pair<std::string_view,
                                                    ftxui::Component>> const& entries) {
            std::vector<std::string>      tab_values{};
            std::vector<ftxui::Component> tab_components{};

            for(auto const& [name, component] : entries) {
                tab_values.push_back(std::string{name} + " ");
                tab_components.push_back(component);
            }
            tabCount = static_cast<int>(tab_values.size());

            auto toggle = ftxui::Toggle(std::move(tab_values), &selectedTab) | ftxui::bold;

            ftxui::Components const vertical_components{
              toggle,
              ftxui::Renderer([]() { return ftxui::separator(); }),
              ftxui::Container::Tab(std::move(tab_components), &selectedTab) | ftxui::flex};

            return ftxui::Container::Vertical(vertical_components);
        }

        ftxui::Component
        generateMetricTabsComponent(std::vector<std::pair<std::string_view,
                                                          ftxui::Component>> const& entries) {
            std::vector<std::string>      tab_values{};
            std::vector<ftxui::Component> tab_components{};

            for(auto const& [name, component] : entries) {
                tab_values.push_back(std::string{name} + " ");
                tab_components.push_back(component);
            }

            auto toggle = ftxui::Toggle(std::move(tab_values), &selectedMetricTab) | ftxui::bold;

            ftxui::Components const vertical_components{
              toggle,
              ftxui::Renderer([]() { return ftxui::separator(); }),
              ftxui::Container::Tab(std::move(tab_components), &selectedMetricTab) | ftxui::flex};

            return ftxui::Container::Vertical(vertical_components);
        }

        template<typename Reader>
        ftxui::Component getStatusLineComponent(Reader& rttReader) {
            auto quitBtn = ftxui::Button(
              "[q]uit",
              [this]() { exitScreen(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto resetBtn = ftxui::Button(
              "[r]eset",
              [this, &rttReader]() { resetTargetWithStats(rttReader); },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            auto flashBtn = ftxui::Button(
              "[f]lash",
              [this, &rttReader]() { flashWithStats(rttReader); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto buildBtn = ftxui::Button(
              "[b]uild",
              [this]() { executeBuild(); },
              createButtonStyle(Theme::Button::Background::build(), Theme::Button::text()));

            auto statusRenderer = ftxui::Renderer([&rttReader, this]() {
                auto const rttStatus      = rttReader.getStatus();
                auto const logCount       = display.filteredOriginalLogCount;
                auto const totalCount     = display.originalLogCount;
                bool const filterActive   = display.filterActive;
                auto const buildStatusNow = buildRunner.getStatus();
                bool const buildRunning   = (buildStatusNow == BuildStatus::Running);
                bool const buildSuccess   = (buildStatusNow == BuildStatus::Success);
                bool const isFlashing     = rttReader.isFlashing();

                return ftxui::hbox(
                  {ftxui::text("🔗 " + std::string(rttStatus.isRunning != 0 ? "●" : "○"))
                     | ftxui::color(rttStatus.isRunning != 0 ? Theme::Status::success()
                                                             : Theme::Text::normal()),
                   ftxui::separator(),

                   ftxui::text("🔍 " + std::string(filterActive ? "●" : "○"))
                     | ftxui::color(filterActive ? Theme::Status::success()
                                                 : Theme::Text::normal()),
                   ftxui::separator(),

                   ftxui::text("🔨 "
                               + std::string((buildRunning || buildSuccess
                                              || buildStatusNow == BuildStatus::Failed)
                                               ? "●"
                                               : "○"))
                     | ftxui::color([&]() {
                           if(buildRunning) { return Theme::Status::warning(); }
                           if(buildSuccess) { return Theme::Status::success(); }
                           if(buildStatusNow == BuildStatus::Failed) {
                               return Theme::Status::error();
                           }
                           return Theme::Text::normal();
                       }()),
                   ftxui::separator(),

                   ftxui::text("⚡ " + std::string([&]() {
                                   if(!isFlashing) { return "●"; }
                                   return (rttStatus.isRunning == 0) ? "○" : "●";
                               }()))
                     | ftxui::color([&]() {
                           if(!isFlashing) { return Theme::Status::success(); }
                           return (rttStatus.isRunning == 0) ? Theme::Status::error()
                                                             : Theme::Status::warning();
                       }())
                     | ftxui::bold,
                   ftxui::separator(),

                   ftxui::text(
                     fmt::format("LOGS {}/{}",
                                 FTXUIGui::formatNumber(static_cast<std::uint32_t>(logCount)),
                                 FTXUIGui::formatNumber(static_cast<std::uint32_t>(totalCount))))
                     | ftxui::color(Theme::Status::info()),
                   display.searchText.empty() ? ftxui::text("") : ftxui::separator(),
                   display.searchText.empty()
                     ? ftxui::text("")
                     : ftxui::text(
                         fmt::format("🔎 {:?} → {}",
                                     display.searchText,
                                     FTXUIGui::formatNumber(static_cast<std::uint32_t>(logCount))))
                         | ftxui::color(Theme::Status::warning()),
                   store.refilterInProgress.load() ? ftxui::separator() : ftxui::text(""),
                   store.refilterInProgress.load()
                     ? ftxui::text(fmt::format("⏳ filtering… {}%",
                                               store.refilterTotal.load() == 0
                                                 ? 100
                                                 : (store.refilterScanned.load() * 100)
                                                     / store.refilterTotal.load()))
                         | ftxui::color(Theme::Status::warning())
                     : ftxui::text(""),
                   ftxui::separator(),

                   ftxui::text(
                     fmt::format("DATA {}", FTXUIGui::formatBytes(rttStatus.numBytesRead)))
                     | ftxui::color(Theme::Status::warning()),
                   ftxui::separator(),

                   ftxui::text(fmt::format("OVFL {}",
                                           FTXUIGui::formatNumber(static_cast<std::uint32_t>(
                                             rttStatus.hostOverflowCount))))
                     | ftxui::color(rttStatus.hostOverflowCount == 0 ? Theme::Status::success()
                                                                     : Theme::Status::error()),
                   display.trimmedLogCount > 0 ? ftxui::separator() : ftxui::text(""),
                   display.trimmedLogCount > 0
                     ? ftxui::text(fmt::format("♻ TRIM {}",
                                               FTXUIGui::formatNumber(static_cast<std::uint32_t>(
                                                 display.trimmedLogCount))))
                         | ftxui::color(Theme::Status::warning()) | ftxui::bold
                     : ftxui::text(""),
                   ftxui::separator(),
                   ftxui::text([this]() -> std::string {
                       if(!logFileEnabled) { return "📄 ○"; }
                       switch(logFileStatus) {
                       case LogFileStatus::Active:     return "📄 ●";
                       case LogFileStatus::Error:      return "📄 ✘";
                       case LogFileStatus::NotStarted: return "📄 ○";
                       }
                       return "📄 ○";
                   }())
                     | ftxui::color([this]() {
                           if(!logFileEnabled) { return Theme::Text::normal(); }
                           switch(logFileStatus) {
                           case LogFileStatus::Active:     return Theme::Status::success();
                           case LogFileStatus::Error:      return Theme::Status::error();
                           case LogFileStatus::NotStarted: return Theme::Text::normal();
                           }
                           return Theme::Text::normal();
                       }())
                     | ftxui::bold,
                   ftxui::separator(),
                   ftxui::text([this]() -> std::string {
                       if(!tcpEnabled) { return "🌐 ○"; }
                       return tcpPortStatus == TcpPortStatus::PortOccupied ? "🌐 ⚠" : "🌐 ●";
                   }())
                     | ftxui::color([this]() {
                           if(!tcpEnabled) { return Theme::Text::normal(); }
                           switch(tcpPortStatus) {
                           case TcpPortStatus::Active:       return Theme::Status::success();
                           case TcpPortStatus::PortOccupied: return Theme::Status::error();
                           case TcpPortStatus::NotStarted:   return Theme::Text::normal();
                           }
                           return Theme::Text::normal();
                       }())
                     | ftxui::bold,
                   [this]() -> ftxui::Element {
                       if(!duplexInfoGetter) { return ftxui::text(""); }
                       auto const infos = duplexInfoGetter();
                       if(infos.empty()) { return ftxui::text(""); }
                       auto const connectedCount = static_cast<std::size_t>(
                         std::ranges::count_if(infos, [](auto const& i) { return i.connected; }));
                       return ftxui::hbox(
                         {ftxui::separator(),
                          ftxui::text(fmt::format("🔁 {}/{}", connectedCount, infos.size()))
                            | ftxui::color(connectedCount > 0 ? Theme::Status::success()
                                                              : Theme::Text::normal())
                            | ftxui::bold});
                   }(),
                   ftxui::separator(),
                   ftxui::filler()});
            });

            auto hotkeyContainer = ftxui::Container::Horizontal(
              {quitBtn,
               ftxui::Renderer(
                 []() { return ftxui::text(" | ") | ftxui::color(Theme::UI::separator()); }),
               resetBtn,
               ftxui::Renderer(
                 []() { return ftxui::text(" | ") | ftxui::color(Theme::UI::separator()); }),
               flashBtn,
               ftxui::Renderer(
                 []() { return ftxui::text(" | ") | ftxui::color(Theme::UI::separator()); }),
               buildBtn});

            return ftxui::Container::Horizontal({statusRenderer | ftxui::flex, hotkeyContainer});
        }

        template<typename Reader>
        ftxui::Component getTabComponent(Reader& rttReader) {
            auto tabs = generateTabsComponent({
              {    "📄 Logs",               getLogComponent()},
              {   "🔨 Build",             getBuildComponent()},
              {  "🔍 Filter",            getFilterComponent()},
              {"🔧 Settings",          getSettingsComponent()},
              {"🐛 Debugger", getDebuggerComponent(rttReader)},
              { "📈 Metrics",            getMetricComponent()},
              {  "💬 Status",            getStatusComponent()},
              {   "📊 Stats",        getStatisticsComponent()},
              {    "❓ Help",              getHelpComponent()}
            });

            return ftxui::Container::Vertical({getStatusLineComponent(rttReader),
                                               ftxui::Renderer([]() { return ftxui::separator(); }),
                                               tabs | ftxui::flex})
                 | ftxui::border;
        }

        void exitScreen() {
            std::lock_guard<std::mutex> const lock{screenMutex};
            if(screenPointer != nullptr) { screenPointer->Exit(); }
        }

    public:
        void add(std::chrono::system_clock::time_point recv_time,
                 uc_log::detail::LogEntry const&       entry) {
            store.addEntry(recv_time, entry);
        }

        void fatalError(std::string_view msg) {
            std::lock_guard<std::mutex> const lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Fatal,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            requestRedrawFromAnywhere();
        }

        void statusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> const lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Status,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            requestRedrawFromAnywhere();
        }

        void errorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> const lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Error,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            requestRedrawFromAnywhere();
        }

        void toolStatusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> const lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::ToolStatus,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            requestRedrawFromAnywhere();
        }

        void toolErrorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> const lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::ToolError,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            requestRedrawFromAnywhere();
        }

        void setTcpPortStatus(TcpPortStatus s,
                              std::uint16_t p) {
            std::lock_guard<std::mutex> const lock{mutex};
            tcpPortStatus  = s;
            tcpCurrentPort = p;
            if(tcpPortInput.empty()) { tcpPortInput = std::to_string(static_cast<unsigned>(p)); }
            requestRedrawFromAnywhere();
        }

        void setOnTcpPortChange(std::function<void(std::uint16_t)> cb) {
            onTcpPortChange = std::move(cb);
        }

        void setTcpClientCountGetter(std::function<std::size_t()> getter) {
            tcpClientCountGetter = std::move(getter);
        }

        void setLogFileStatus(LogFileStatus    s,
                              std::string_view path) {
            std::lock_guard<std::mutex> const lock{mutex};
            logFileStatus      = s;
            logFileCurrentPath = std::string{path};
            if(logDirInput.empty()) {
                logDirInput = std::filesystem::path{path}.parent_path().string();
            }
            if(exportDirInput.empty()) { exportDirInput = logDirInput; }
            requestRedrawFromAnywhere();
        }

        void setOnLogDirChange(std::function<void(std::string const&)> cb) {
            onLogDirChange = std::move(cb);
        }

        void setOnLogFileEnable(std::function<void(bool)> cb) { onLogFileEnable = std::move(cb); }

        void setOnTcpEnable(std::function<void(bool)> cb) { onTcpEnable = std::move(cb); }

        void setDuplexInfoGetter(
          std::function<std::vector<uc_log::detail::DuplexChannelInfo>()> getter) {
            duplexInfoGetter = std::move(getter);
        }

        void setOnDuplexPortChange(std::function<void(std::size_t,
                                                      std::uint16_t)> cb) {
            onDuplexPortChange = std::move(cb);
        }

        void setOnDuplexEnable(std::function<void(std::size_t,
                                                  bool)> cb) {
            onDuplexEnable = std::move(cb);
        }

        void setOnDuplexBasePortChange(std::function<void(std::uint16_t)> cb) {
            onDuplexBasePortChange = std::move(cb);
        }

        void setNetworkBindAddress(std::string address) {
            std::lock_guard<std::mutex> const lock{mutex};
            networkBindAddress = std::move(address);
            if(bindAddressInput.empty()) { bindAddressInput = networkBindAddress; }
        }

        void setOnNetworkBindAddressChange(std::function<bool(std::string const&)> cb) {
            onNetworkBindAddressChange = std::move(cb);
        }

        void setDuplexBasePort(std::uint16_t port) {
            std::lock_guard<std::mutex> const lock{mutex};
            if(duplexBasePortInput.empty()) {
                duplexBasePortInput = std::to_string(static_cast<unsigned>(port));
            }
        }

        void triggerRedraw() {
            std::lock_guard<std::mutex> const lock{mutex};
            requestRedrawFromAnywhere();
        }

        template<typename Reader>
        int run(Reader&            rttReader,
                std::string const& buildCommand,
                std::string const& initialHost = "") {
            connectionTypeSelection = initialHost.empty() ? 0 : 1;
            ipAddressInput          = initialHost;
            buildRunner.initialize(buildCommand);

            auto screen = ftxui::ScreenInteractive::Fullscreen();
            screen.ForceHandleCtrlC(true);
            ftxui::Component mainComponent;
            {
                std::lock_guard<std::mutex> const lock{mutex};

                mainComponent
                  = ftxui::CatchEvent(getTabComponent(rttReader), [&](ftxui::Event const& event) {
                        if(!event.is_character()) { return false; }
                        // Only handle hotkeys when not actively typing in a text input field
                        if(anyTextInputFocused()) { return false; }

                        auto const& chars = event.character();
                        if(chars.size() != 1) { return false; }
                        char const c = chars[0];

                        // Number keys for tab switching
                        if(c >= '1' && c <= '9' && c - '1' < tabCount) {
                            selectedTab = c - '1';
                            return true;
                        }
                        // Action hotkeys
                        if(c == '/') {
                            selectedTab    = 0;   // Logs tab
                            searchRowShown = true;
                            if(searchInput) { searchInput->TakeFocus(); }
                            return true;
                        }
                        switch(c) {
                        case 'r': resetTargetWithStats(rttReader); return true;
                        case 'f': flashWithStats(rttReader); return true;
                        case 'b': executeBuild(); return true;
                        case 'F': executeBuildAndFlash(); return true;
                        case 'q': exitScreen(); return true;
                        default:  return false;
                        }
                    });
            }
            ftxui::Loop loop(&screen, mainComponent);
            {
                std::lock_guard<std::mutex> const lock{screenMutex};
                screenPointer = &screen;
            }

            auto lastSettingsRefresh = std::chrono::steady_clock::now();
            while(!loop.HasQuitted()) {
                // one snapshot per frame: renderers never touch the store afterwards
                store.refreshMirror(display);
                buildRunner.snapshotOutput(buildOutputDisplay, buildOutputSeen);
                {
                    std::lock_guard<std::mutex> const lock{mutex};
                    updateJLinkStatistics(rttReader);
                    // the live window moves with the data; the input string follows unless
                    // the user is editing it
                    if(display.ucTimeLiveMode && !ucTimeMinInput->Focused()) {
                        minUcTimeStr = fmt::format("{:.1f}", display.minUcTimeSec);
                        maxUcTimeStr.clear();
                    }
                    // duplex byte counters and client state change without a posted event
                    auto const now = std::chrono::steady_clock::now();
                    if(selectedTab == SettingsTabIndex
                       && now - lastSettingsRefresh >= GUI_Constants::SettingsRefreshInterval)
                    {
                        lastSettingsRefresh = now;
                        redrawPending.store(true, std::memory_order_relaxed);
                    }
                    // producers cannot post ftxui events (not thread-safe); relay their
                    // coalesced request from the UI thread
                    if(redrawPending.exchange(false, std::memory_order_relaxed)) {
                        screen.PostEvent(ftxui::Event::Custom);
                    }
                    loop.RunOnce();
                }
                store.pollRefilterDeadline();
                for(auto& action : pendingActions) { action(); }
                pendingActions.clear();
                std::this_thread::sleep_for(GUI_Constants::UpdateInterval);
                buildRunner.joinIfFinished();
                if(buildRunner.triggerFlashNow.exchange(false)) {
                    std::lock_guard<std::mutex> const lock{mutex};
                    flashWithStats(rttReader);
                }
            }
            {
                std::lock_guard<std::mutex> const lock{screenMutex};
                screenPointer = nullptr;
            }

            return 0;
        }
    };
}}   // namespace uc_log::FTXUIGui
