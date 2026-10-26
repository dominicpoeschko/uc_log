#pragma once

#include "uc_log/LogFilter.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/DuplexChannelInfo.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/FilterFile.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LogFormat.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/Outliers.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/RowText.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"
#include "uc_log/printer/TargetInspector.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/metric_utils.hpp"
#include "uc_log/printer/tui/FTXUI_Utils.hpp"
#include "uc_log/printer/tui/InspectorViews.hpp"
#include "uc_log/printer/tui/MetricPlot.hpp"
#include "uc_log/printer/tui/Widgets.hpp"
#include "uc_log/printer/tui/theme.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fmt/format.h>
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

namespace uc_log::printer { namespace tui {

    struct Window {
        // The Window outlives every thread of the printer that says something (main and the demo construct it before
        // them); the model's build thread it stops itself.
        explicit Window(printer::PrinterModel& printerModel UC_LOG_LIFETIMEBOUND)
          : model{printerModel}
          , messages{printerModel.messages()}
          , buildRunner{printerModel.builds()} {
            // a message: redraw; a captured halt: the inspector's crash view
            messages.addObserver([this](printer::MessageLog::Said const& said) {
                if(said.kind == printer::MessageKind::halt && said.halt != nullptr) {
                    std::scoped_lock<std::mutex> const lock{inspectorMutex};
                    if(inspector) { inspector->noteHalt(*said.halt); }
                }
                requestRedrawFromAnywhere();
            });
            store.setRedrawRequest([this]() { requestRedrawFromAnywhere(); });
            buildRunner.requestRedraw = [this]() { requestRedrawFromAnywhere(); };
        }

        ~Window() {
            buildRunner.stopAndJoin();
            buildRunner.requestRedraw = {};
            messages.clearObservers();
        }

        Window(Window const&)            = delete;
        Window& operator=(Window const&) = delete;

        Window(Window&&)            = delete;
        Window& operator=(Window&&) = delete;

    private:
        using MessageEntry = printer::MessageLog::Entry;

        using OutlierMethod = printer::OutlierMethod;
        using OutlierResult = printer::OutlierResult<printer::SourceLocation>;

        using Statistics = printer::Statistics;

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
        printer::EntryStore         store;
        printer::EntryStore::Mirror display;

        tui::MetricPlotWidget metricPlotWidget;
        // UI-side copy of the plotted series, filled under the store mutex per frame
        std::vector<MetricEntry> plotDataBuffer;

        printer::FilterState editedFilterState;

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
        // the firmware's compile-time log filter (--log_filter), shown in the module tree
        uc_log::detail::FilterTable compiledFilter{};
        std::size_t                 compiledFilterVersion{0};

        bool showFunctionName{false};
        // copies and .txt exports take the whole signature too
        bool showFullSignature{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showModule{false};
        bool showChannel{true};
        bool showLogLevel{true};
        bool showMetricString{false};
        bool showTypenameString{false};

        // Parsed-message Element cache: ANSI parsing + marker processing of a line is a
        // pure function of (message slice, metric/typename toggles), so visible rows are
        // re-parsed only when they first appear or a toggle changes. The stored shared_ptr
        // keeps the entry alive, which makes address-reuse false hits impossible.
        static constexpr std::size_t maxRenderCacheEntries = 16 * 1024;
        std::map<std::tuple<void const*, std::uint32_t, std::uint8_t>,
                 std::pair<std::shared_ptr<printer::EntryCommon const>, ftxui::Element>>
          renderCache;

        std::uint8_t messageToggleBits() const {
            return static_cast<std::uint8_t>((showMetricString ? 1U : 0U)
                                             | (showTypenameString ? 2U : 0U));
        }

        std::size_t lastMetricCount{0};
        bool        hasLastSelectedInfo{false};
        MetricInfo  lastSelectedInfo;

        // the printer's state and commands; what the tabs show of it is mirrored once per frame (refreshModelView)
        printer::PrinterModel& model;
        // the printer's messages (say, counts, history); the Status tab mirrors them once per frame
        printer::MessageLog&      messages;
        std::vector<MessageEntry> statusMessages;
        std::size_t               statusClearsSeen{};

        // lines lost to a format string the host could not apply (reset by every flash), and errors: per frame from
        // `messages`
        std::size_t formatErrorCount{0};
        bool        lastFlashing{false};

        std::size_t statusErrorCount{0};

        // log records the target dropped (LossPolicy::Counted, printer/TargetLoss.hpp); nullopt while
        // the firmware does not count them
        std::optional<std::uint64_t> targetLostCount;

        control::FirmwareState firmwareState{control::FirmwareState::unchecked};
        std::size_t            firmwareMismatchCount{0};

        // build execution lives behind its own mutex in the runner; the UI renders from
        // the versioned snapshot below
        printer::BuildRunner&            buildRunner;   // the model's
        std::vector<printer::BuildEntry> buildOutputDisplay;
        std::uint64_t                    buildOutputSeen{std::numeric_limits<std::uint64_t>::max()};

        int                     selectedLocationIndex{};
        printer::SourceLocation selectedSourceLocation;
        std::string             locationFilterInput;
        ftxui::Component        manualLocationInput;
        ftxui::Component        filterConfigInput;
        ftxui::Component        iqrInput;
        ftxui::Component        topNInput;
        ftxui::Component        absInput;

        std::string filterConfigPath{"filter.json"};
        std::string filterConfigStatus;
        bool        filterConfigFailed{};   // filterConfigStatus says what went wrong

        std::string   noiseExcludeStatus;
        OutlierMethod outlierMethod{OutlierMethod::iqrTukey};
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
        OutlierMethod cachedOutlierMethod{OutlierMethod::iqrTukey};
        double        cachedOutlierIqrK{0.0};
        double        cachedOutlierTopN{0.0};
        std::size_t   cachedOutlierAbs{0};

        // the main tabs, in the bar's order: the hotkeys 1..9 and 0 and every "is this tab shown" use these
        enum class Tab : int {
            logs,
            build,
            filter,
            settings,
            debugger,
            metrics,
            status,
            health,
            inspect,
            help,
            count
        };
        int selectedTab{};
        int tabCount{0};

        int selectedMetricTab{};

        // Health and Inspect tabs (kvasir_bench.py's ub/crash/stack/trace/peek/profile/panic):
        // the inspector works on its own thread; the renderers read the copy below, taken
        // only when its version moved
        std::function<std::shared_ptr<printer::MapSnapshot const>()> mapSource;
        std::mutex                                        inspectorMutex;   // the pointer
        std::unique_ptr<uc_log::printer::TargetInspector> inspector;
        uc_log::printer::TargetInspector::Snapshot        inspectorView;
        std::uint64_t inspectorViewVersion{std::numeric_limits<std::uint64_t>::max()};
        bool          targetHalted{false};   // the reader's isHalted(), per frame
        int           selectedInspectTab{};
        bool          traceDelta{true};
        bool          traceHex{false};
        std::string   traceRowsStr{"64"};
        std::string   watchInput;
        std::string   watchStatus;
        std::string   watchCompletionsFor;
        std::uint64_t watchCompletionsMap{~std::uint64_t{}};
        uc_log::printer::TargetInspector::Completions watchCompletions;
        std::vector<std::string>                      watchCompletionTerms;
        int                                           watchCompletionSelection{0};
        std::shared_ptr<PickListBase>                 watchCompletionList;
        std::vector<std::string>                      watchNames;
        int                                           watchSelection{0};
        std::string watchSelectPending;   // a watch just added: selected once it is in the view
        std::vector<std::string> panicCauseEntries;
        int                      panicCauseSelection{0};
        bool                     panicArmed{false};
        static constexpr int     healthTabIndex  = static_cast<int>(Tab::health);
        static constexpr int     inspectTabIndex = static_cast<int>(Tab::inspect);

        // Every text input component, so hotkey handling can generically check whether
        // the user is typing (see trackInput / anyTextInputFocused).
        std::vector<ftxui::Component> trackedTextInputs;

        SocketPortStatus         tcpPortStatus{SocketPortStatus::notStarted};
        std::uint16_t            tcpCurrentPort{0};
        std::string              controlSocketPath;       // not empty: a unix socket
        int                      transportSelection{0};   // 0 unix, 1 TCP
        std::vector<std::string> transportEntries{" unix sockets ", " TCP "};
        std::string              tcpPortInput;
        std::size_t              tcpClientCount{};
        std::uint64_t            controlStatusSeen{};
        std::uint64_t            logFileSeen{};
        std::string      controlSocketSeen{"\x01"};   // no path: the first frame sets the toggle
        ftxui::Component tcpPortInputComponent;

        std::array<std::string, printer::PrinterModel::maxDuplexChannels> duplexPortInputs;
        std::array<ftxui::Component, printer::PrinterModel::maxDuplexChannels>
                         duplexPortInputComponents;
        std::string      duplexBasePortInput;
        ftxui::Component duplexBasePortInputComponent;

        LogFileStatus    logFileStatus{LogFileStatus::notStarted};
        std::string      logFileCurrentPath;
        std::string      logDirInput;
        bool             logFileEnabled{true};
        bool             tcpEnabled{true};
        std::string      networkBindAddress{"127.0.0.1"};
        std::string      bindAddressInput;
        ftxui::Component bindAddressInputComponent;
        std::string      bindAddressStatus;
        ftxui::Component logDirInputComponent;

        std::string      exportDirInput;
        ftxui::Component exportDirInputComponent;
        std::string      lastExportPath;
        std::size_t      lastExportCount{0};
        bool             lastExportOk{false};

        Statistics statistics;   // the model's, mirrored once per frame

        int              selectedResetType{0};
        int              connectionTypeSelection{0};   // 0 = USB, 1 = IP
        std::string      ipAddressInput{};
        ftxui::Component ipAddressInputComponent;
        // Probe picker: the probes the reader last listed, one radio entry each behind
        // "any", and the name that is actually sent (serial number or nickname; typed or
        // filled in by picking an entry).
        std::string              probeInput{};
        ftxui::Component         probeInputComponent;
        std::vector<std::string> probeEntries{"any (the only J-Link on USB)"};
        std::vector<std::string> probeNames{""};
        int                      probeSelection{0};
        std::uint64_t            probesVersionSeen{0};
        std::string              noLogTimeoutStr{"15"};
        ftxui::Component         noLogTimeoutInput;

        // Cross-thread redraw request. PostEvent is thread-safe since ftxui v7.0.2, but
        // producers still only set this flag: the UI loop posts a single event per
        // iteration, coalescing redraw bursts. Latency is bounded by
        // GUI_Constants::UpdateInterval, same as the loop granularity.
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

        // build machinery lives in printer/BuildRunner.hpp; these thin wrappers keep the
        // statistics (UI mutex domain) in sync
        void executeBuild() { static_cast<void>(model.startBuild(false)); }

        void executeBuildAndFlash() { static_cast<void>(model.startBuild(true)); }

        void resetTargetWithStats(printer::TargetPort& rttReader) {
            model.noteResetRequest();
            rttReader.resetTarget();
        }

        void flashWithStats(printer::TargetPort& rttReader) {
            model.noteFlashRequest();
            rttReader.flash();
        }

        void updateJLinkStatistics(printer::TargetPort& rttReader);

        std::string processLogMessage(std::string_view originalMsg) const;

        // ANSI parsing + marker processing of a line is a pure function of the message
        // slice and the metric/typename toggles: cache the resulting element so visible
        // rows are parsed once instead of every frame.
        ftxui::Element renderMessageElement(printer::StoredLine const& entry) {
            auto const key
              = std::tuple<void const*, std::uint32_t, std::uint8_t>{entry.common.get(),
                                                                     entry.lineOffset,
                                                                     messageToggleBits()};
            if(auto const it = renderCache.find(key); it != renderCache.end()) {
                return it->second.second;
            }
            if(renderCache.size() >= maxRenderCacheEntries) { renderCache.clear(); }
            auto element = ansiColoredTextToFtxui(processLogMessage(entry.lineText()));
            renderCache.emplace(key, std::pair{entry.common, element});
            return element;
        }

        auto defaultRender(printer::StoredLine const& entry) {
            auto const&     common = *entry.common;
            ftxui::Elements elements;
            elements.reserve(12);

            bool const showPrefix = entry.startsGroup();

            if(showPrefix) {
                // Show full prefix for first/single lines
                if(showSysTime) {
                    elements.push_back(ftxui::text(toTimeStringWithMilliseconds(common.recvTime))
                                       | ftxui::color(Theme::Text::timestamp()));
                    elements.push_back(ftxui::text(" "));
                }

                if(showChannel) {
                    elements.push_back(
                      toElement(uc_log::printer::LogEntry::Channel{common.channel}));
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
               && printer::CompiledFilter::containsCaseInsensitive(entry.lineText(),
                                                                   display.searchNeedleLower))
            {
                messageElement = messageElement | ftxui::bold | ftxui::underlined;
            }
            elements.push_back(std::move(messageElement));

            auto scrollableContent = ftxui::hbox(elements) | ftxui::flex;

            // Metadata: only show on single/last lines
            bool const showMetadata = (entry.lineType == printer::LineType::singleLine
                                       || entry.lineType == printer::LineType::last);

            ftxui::Element metadataElement;
            if(showMetadata) {
                ftxui::Elements metadata;
                auto const      module = display.moduleNameOf(common.moduleId);
                if(showModule && !module.empty()) {
                    metadata.push_back(ftxui::text(fmt::format("[{}]", module))
                                       | ftxui::color(Theme::Text::module()));
                    if(showFunctionName || showLocation) { metadata.push_back(ftxui::text(" ")); }
                }
                if(showFunctionName) {
                    metadata.push_back(ftxui::text(display.functionNameOf(common.functionId))
                                       | ftxui::color(Theme::Text::functionName()));
                }

                if(showLocation) {
                    if(showFunctionName) { metadata.push_back(ftxui::text(" ")); }
                    metadata.push_back(ftxui::text(fmt::format(
                                         "{}:{}",
                                         display.fileNameOf(common.locationKey),
                                         printer::EntryStore::Mirror::lineOf(common.locationKey)))
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

            elements.push_back(ftxui::text(toTimeStringWithMilliseconds(entry.time))
                               | ftxui::color(Theme::Text::timestamp()));

            elements.push_back(ftxui::text(" | ") | ftxui::color(Theme::Text::normal()));

            elements.push_back(ftxui::text(entry.text)
                               | ftxui::color(printer::tui::colourOf(entry.level)) | ftxui::flex);

            return ftxui::hbox(elements);
        }

        void exportFilteredLogs(std::string const&                       dir,
                                printer::EntryChunkList::Snapshot const& entries,
                                bool                                     asPlainText);

        void updateCurrentFilter() { store.setFilterState(editedFilterState); }

        void saveFilterConfig(std::string const&          path,
                              printer::FilterState const& fs) {
            auto const saved   = printer::saveFilter(path, fs);
            filterConfigFailed = !saved;
            filterConfigStatus = saved ? std::string{"Saved."} : saved.error();
        }

        void loadFilterConfig(std::string const&    path,
                              printer::FilterState& fs) {
            auto loaded        = printer::loadFilter(path);
            filterConfigFailed = !loaded;
            if(!loaded) {
                filterConfigStatus = loaded.error();
                return;
            }
            fs = std::move(*loaded);
            updateCurrentFilter();
            filterConfigStatus = "Loaded.";
        }

        // recompute only when the location list or the parameters changed: the full
        // copy + sort used to run once per rendered frame
        OutlierResult const& currentOutliers() UC_LOG_LIFETIMEBOUND {
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
                cachedOutlierResult  = printer::computeOutliers(display.locationList,
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
            static constexpr char alphabet[]
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
                out.push_back(alphabet[(block >> 18U) & 0x3FU]);
                out.push_back(alphabet[(block >> 12U) & 0x3FU]);
                out.push_back(n > 1 ? alphabet[(block >> 6U) & 0x3FU] : '=');
                out.push_back(n > 2 ? alphabet[block & 0x3FU] : '=');
            }
            return out;
        }

        std::string functionTextOf(printer::EntryStore::Mirror const& mirror,
                                   std::uint32_t                      functionId) const {
            return showFullSignature ? mirror.functionSignatureOf(functionId)
                                     : mirror.functionNameOf(functionId);
        }

        std::string modulePrefix(std::uint32_t moduleId) const {
            auto const module = display.moduleNameOf(moduleId);
            return module.empty() ? std::string{} : fmt::format("[{}] ", module);
        }

        // the export's and the yank's text of a line (printer/RowText.hpp)
        template<typename Entry>
        std::string rowText(Entry const& e,
                            bool         padLevel) const {
            auto const& c        = *e.common;
            auto const  recv     = toTimeStringWithMilliseconds(c.recvTime);
            auto const  uc       = fmt::format("{}", c.ucTime);
            auto const  module   = modulePrefix(c.moduleId);
            auto const  file     = display.fileNameOf(c.locationKey);
            auto const  function = functionTextOf(display, c.functionId);
            return printer::plainRow(
              printer::RowFields{.recvTime     = recv,
                                 .channel      = c.channel,
                                 .ucTime       = uc,
                                 .level        = enchantum::to_string(c.logLevel),
                                 .text         = e.lineText(),
                                 .modulePrefix = module,
                                 .file         = file,
                                 .line         = printer::EntryStore::Mirror::lineOf(c.locationKey),
                                 .function     = function},
              padLevel);
        }

        void yankEntryToClipboard(std::size_t index) {
            if(index >= display.entries.size()) { return; }
            auto const& e    = display.entries[index];
            auto        text = rowText(e, false);
            // OSC 52 goes to the same terminal ftxui renders on, but only after the frame
            // is done (pendingActions run outside RunOnce), so it cannot interleave with
            // ftxui's own escape output
            pendingActions.emplace_back([this, payload = std::move(text)]() {
                auto const sequence = fmt::format("\033]52;c;{}\a", base64Encode(payload));
                std::fwrite(sequence.data(), 1, sequence.size(), stdout);
                std::fflush(stdout);
                messages.status("line copied to clipboard (OSC 52)");
            });
        }

        void jumpToUcTime() {
            auto const parsed = printer::tui::leadingNumber<double>(jumpToStr);
            if(!parsed) { return; }
            double const target = *parsed;
            // ten digits of seconds reach past what the nanosecond clock holds (9.22e9 s): no entry there, and the
            // cast below would be UB
            constexpr double lastSecond = 9e9;
            if(!(target < lastSecond)) {
                messages.status(fmt::format("no entry at or after uc-time {:.3f} s", target));
                return;
            }
            auto const targetNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>{target});
            for(std::size_t i = 0; i < display.entries.size(); ++i) {
                if(display.entries[i].common->ucTime.time >= targetNs) {
                    if(logScrollerJump) { logScrollerJump(static_cast<int>(i)); }
                    return;
                }
            }
            messages.status(fmt::format("no entry at or after uc-time {:.3f} s", target));
        }

        ftxui::Component getLogComponent();

        ftxui::Component getStatusComponent() {
            auto clearButton = printer::tui::button("🗑️ Clear messages",
                                                    printer::tui::Style::destructive,
                                                    [this]() { model.clearCounts(); });

            return ftxui::Container::Vertical(
              {clearButton,
               printer::tui::divider(),
               ftxui::Container::Vertical(
                 {scroller([&]() -> std::vector<MessageEntry> const& { return statusMessages; },
                           [&](auto const& entry) { return renderMessage(entry); })})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("💬 Status Messages") | ftxui::bold
                                             | ftxui::color(Theme::Header::secondary())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           std::move(inner)});
                   })});
        }

        ftxui::Element renderBuildEntry(printer::BuildEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(toTimeStringWithMilliseconds(entry.time))
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
            case printer::BuildStatus::idle:
                statusText  = "⚪ Idle";
                statusColor = Theme::Status::inactive();
                break;
            case printer::BuildStatus::running:
                statusText  = "🟡 Building...";
                statusColor = Theme::Status::running();
                break;
            case printer::BuildStatus::success:
                statusText  = "✅ Success";
                statusColor = Theme::Status::success();
                break;
            case printer::BuildStatus::failed:
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
                bool copied = false;
                store.withMetric(metric, [&](std::vector<MetricEntry> const& values) {
                    if(values.empty()) { return; }
                    plotDataBuffer = values;
                    copied         = true;
                });
                if(copied) { return &plotDataBuffer; }
                return std::nullopt;
            };

            auto clearCallback = [this]() {
                if(auto selectedMetric = metricPlotWidget.getSelectedMetric()) {
                    store.clearMetric(*selectedMetric);
                }
            };

            return metricPlotWidget.createComponent(dataProvider, clearCallback);
        }

        ftxui::Component getBuildComponent();

        ftxui::Component getMetricOverviewComponent();

        ftxui::Component getMetricComponent() {
            auto metricTabs = tabBar(
              {
                { "📋 Overview", getMetricOverviewComponent()},
                {"📈 Live Plot",     getMetricPlotComponent()}
            },
              &selectedMetricTab);

            return ftxui::Container::Vertical({metricTabs | ftxui::flex});
        }

// clang 23 asks for lifetimebound on these two (the filter's components hold the Window's filter state) and then
// cannot verify it: they return what the widget kit builds
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wlifetime-safety-cross-tu-suggestions"
#endif
        ftxui::Component getLogLevelFilterComponent();

        ftxui::Component getChannelFilterComponent();
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

        // Unchecking a node hides its subtree; a hidden ancestor must be re-checked first.
        class ModuleFilterBase : public ftxui::ComponentBase {
        public:
            explicit ModuleFilterBase(Window& gui_ UC_LOG_LIFETIMEBOUND) : gui{gui_} {
                list = ftxui::Container::Vertical({});
                Add(ftxui::Container::Vertical(
                  {printer::tui::button("🧩 Show All Modules",
                                        printer::tui::Style::build,
                                        [this]() {
                                            gui.editedFilterState.excludedModules.clear();
                                            gui.updateCurrentFilter();
                                        }),
                   list}));
            }

            ftxui::Element OnRender() override {
                auto const& names = gui.display.moduleNamesById;
                if(built != names.size() || builtFilter != gui.compiledFilterVersion) {
                    rebuild(names);
                }
                auto content = ChildAt(0)->Render();
                if(gui.compiledFilter.global != 0) {
                    content = ftxui::vbox(
                      {ftxui::text(
                         "compiled: every line ≥ "
                         + std::string{uc_log::detail::filterLevelName(gui.compiledFilter.global)})
                         | ftxui::color(Theme::Text::metadata()),
                       content});
                }
                if(names.empty() && !gui.compiledFilter.hasModuleRules()) {
                    content = ftxui::vbox(
                      {content,
                       ftxui::text("no module yet") | ftxui::color(Theme::Text::metadata())});
                }
                return content | ftxui::border;
            }

        private:
            void rebuild(std::vector<std::string> const& names) {
                built       = names.size();
                builtFilter = gui.compiledFilterVersion;
                // the filter file's modules too: a compiled-out one has no lines
                std::vector<std::string> all{names};
                for(std::size_t i = 0; i < gui.compiledFilter.count; ++i) {
                    all.emplace_back(gui.compiledFilter.rules[i].name());
                }
                std::set<std::string> nodes;
                for(auto const& name : all) {
                    for(std::size_t dot = name.find('.'); dot != std::string::npos;
                        dot             = name.find('.', dot + 1))
                    {
                        nodes.insert(name.substr(0, dot));
                    }
                    nodes.insert(name);
                }
                list->DetachAllChildren();
                for(auto const& node : nodes) {
                    auto const  depth = static_cast<std::size_t>(std::ranges::count(node, '.'));
                    auto const  last  = node.rfind('.');
                    std::string label = std::string(depth * 2, ' ')
                                      + (last == std::string::npos ? node : node.substr(last + 1));
                    for(std::size_t i = 0; i < gui.compiledFilter.count; ++i) {
                        auto const& rule = gui.compiledFilter.rules[i];
                        if(rule.name() != node) { continue; }
                        label += rule.level == uc_log::detail::levelOff
                                 ? std::string{"  ⛔ compiled out"}
                                 : "  compiled ≥ "
                                     + std::string{uc_log::detail::filterLevelName(rule.level)};
                    }
                    list->Add(functionCheckbox(
                      std::move(label),
                      [this, node]() {
                          return !printer::moduleHiddenBy(node,
                                                          gui.editedFilterState.excludedModules);
                      },
                      [this, node]() {
                          auto& excluded = gui.editedFilterState.excludedModules;
                          if(excluded.erase(node) == 0) {
                              if(printer::moduleHiddenBy(node, excluded)) {
                                  return;
                              }   // an ancestor hides it
                              std::erase_if(excluded, [&](std::string const& e) {
                                  return printer::moduleUnder(e, node);
                              });
                              excluded.insert(node);
                          }
                          gui.updateCurrentFilter();
                      }));
                }
            }

            Window&          gui;
            ftxui::Component list;
            std::size_t      built{0};
            std::size_t      builtFilter{0};
        };

        ftxui::Component getModuleFilterComponent() { return ftxui::Make<ModuleFilterBase>(*this); }

        ftxui::Component getLocationFilterComponent();

        ftxui::Component getFilterComponent();

        ftxui::Component getSettingsComponent();

        static constexpr int settingsTabIndex = static_cast<int>(Tab::settings);

        ftxui::Component getDuplexSection();

        ftxui::Component getHelpComponent();

        ftxui::Component getStatisticsComponent();

        // A debugger button's op: through the reader's one queue, outside the UI's event handling (it waits up to a
        // reader pass); a failure is said in the status log.
        void sendCoreOp(printer::TargetPort&      port,
                        uc_log::printer::TargetOp op) {
            pendingActions.emplace_back([this, &port, op]() {
                std::array<uc_log::printer::TargetOp, 1> const ops{op};
                if(auto const r = port.accessTarget(ops, printer::TargetPort::defaultTimeout); !r) {
                    messages.toolError(
                      fmt::format("{}: {}", uc_log::printer::describe(op), r.error()));
                }
            });
        }

        // Rebuild the probe radio entries when the reader has a newer list. The current
        // name is kept; the radio follows it when it is one of the listed probes.
        void syncProbeEntries(printer::TargetPort& rttReader);

        ftxui::Component getDebuggerComponent(printer::TargetPort& rttReader);

        // the main and the Metrics tab bars: "<name> " each (printer/tui/Widgets.hpp's tabs)
        static ftxui::Component tabBar(std::vector<std::pair<std::string_view,
                                                             ftxui::Component>> const& entries,
                                       int*                                            selected) {
            std::vector<std::string>      names;
            std::vector<ftxui::Component> pages;
            for(auto const& [name, page] : entries) {
                names.push_back(std::string{name} + " ");
                pages.push_back(page);
            }
            return printer::tui::tabs(std::move(names), std::move(pages), selected);
        }

        ftxui::Component getStatusLineComponent(printer::TargetPort& rttReader);

        // Per frame, under the gui mutex: the inspector's snapshot when it changed, and which of
        // its pages is shown (rings and watches are read only while they are).
        void refreshInspectorView(bool halted);

        // the inspector, called from a button: the pointer is only swapped in run()
        template<typename F>
        void withInspector(F&& f) {
            std::scoped_lock<std::mutex> const lock{inspectorMutex};
            if(inspector) { f(*inspector); }
        }

        ftxui::Component getHealthComponent();

        ftxui::Component getTraceComponent() {
            auto rowsInput = trackInput(printer::tui::textInput(traceRowsStr, "64")
                                        | printer::tui::numericFilter(false));
            auto controls  = ftxui::Container::Horizontal(
              {ftxui::Checkbox(" Δ first field  ", &traceDelta),
               ftxui::Checkbox(" hex  ", &traceHex),
               ftxui::Renderer([]() { return ftxui::text(" rows per ring "); }),
               rowsInput | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6)});
            auto view = scrollView([this]() {
                // digits only (the filter); empty or too large: 64
                auto const rows = printer::tui::leadingNumber<std::size_t>(traceRowsStr)
                                    .value_or(std::size_t{64});
                return inspector::ringsElement(inspectorView, traceDelta, traceHex, rows);
            });
            return ftxui::Container::Vertical(
              {controls, printer::tui::divider(), view | ftxui::flex});
        }

        static constexpr std::size_t maxWatchCompletions = 200;
        static constexpr int         watchCompletionRows = 10;

        // The data symbols the typed text matches, looked up again when the text or the map
        // changed; called by everything that reads them (a key can come before the frame).
        void refreshWatchCompletions() {
            if(watchInput == watchCompletionsFor
               && inspectorView.mapGeneration == watchCompletionsMap)
            {
                return;
            }
            watchCompletionsFor = watchInput;
            watchCompletionsMap = inspectorView.mapGeneration;
            watchCompletions    = {};
            withInspector([this](auto& i) {
                watchCompletions = i.completions(watchInput, maxWatchCompletions);
            });
            watchCompletionTerms     = uc_log::printer::SymbolQuery{watchInput}.literals();
            watchCompletionSelection = 0;
            if(watchCompletionList) { watchCompletionList->showSelected(); }
        }

        bool isWatched(std::string const& name) const {
            return std::ranges::find(watchNames, name) != watchNames.end();
        }

        // Watch one of the listed symbols; `toggle`: one that is watched is not any more.
        void pickWatchCompletion(int  index,
                                 bool toggle) {
            if(index < 0 || index >= static_cast<int>(watchCompletions.best.size())) { return; }
            auto const name  = watchCompletions.best[static_cast<std::size_t>(index)].name;
            auto const shown = inspector::shortName(name);
            if(isWatched(name)) {
                if(toggle) {
                    withInspector([&](auto& i) { i.removeWatch(name); });
                    watchStatus = "no longer watching " + shown;
                } else {
                    watchStatus = "already watching " + shown;
                    watchSelection
                      = static_cast<int>(std::ranges::find(watchNames, name) - watchNames.begin());
                }
                return;
            }
            std::string result;
            withInspector([&](auto& i) { result = i.addWatch(name); });
            watchStatus = result.empty() ? "watching " + shown : result;
            if(result.empty()) { watchSelectPending = name; }
        }

        void addWatchFromInput() {
            if(watchInput.empty()) { return; }
            refreshWatchCompletions();
            if(!watchCompletions.best.empty()) {
                pickWatchCompletion(watchCompletionSelection, false);
                watchInput.clear();
                return;
            }
            // not in the list (no size in a GNU ld map, a function): its exact name still works
            std::string result;
            withInspector([&](auto& i) { result = i.addWatch(watchInput); });
            watchStatus = result.empty() ? "watching " + watchInput : result;
            if(result.empty()) {
                watchSelectPending = watchInput;
                watchInput.clear();
            }
        }

        void removeWatchAt(int index) {
            if(index < 0 || index >= static_cast<int>(inspectorView.watches.size())) { return; }
            auto const name = inspectorView.watches[static_cast<std::size_t>(index)].name;
            withInspector([&](auto& i) { i.removeWatch(name); });
            watchStatus = "no longer watching " + inspector::shortName(name);
        }

        ftxui::Component getWatchComponent();

        ftxui::Component getProfileComponent();

        ftxui::Component getInspectComponent() {
            return printer::tui::tabs(
              {"🧵 Trace ", "👁 Watch ", "⏱ Profile "},
              {getTraceComponent(), getWatchComponent(), getProfileComponent()},
              &selectedInspectTab);
        }

        ftxui::Component getTabComponent(printer::TargetPort& rttReader) {
            // in Tab's order
            std::vector<std::pair<std::string_view, ftxui::Component>> const entries{
              {    "📄 Logs",               getLogComponent()},
              {   "🔨 Build",             getBuildComponent()},
              {  "🔍 Filter",            getFilterComponent()},
              {"🔧 Settings",          getSettingsComponent()},
              {"🐛 Debugger", getDebuggerComponent(rttReader)},
              { "📈 Metrics",            getMetricComponent()},
              {  "💬 Status",            getStatusComponent()},
              {  "🩺 Health",            getHealthComponent()},
              { "🔬 Inspect",           getInspectComponent()},
              {    "❓ Help",              getHelpComponent()}
            };
            static_assert(static_cast<int>(Tab::count) == 10);
            tabCount  = static_cast<int>(entries.size());
            auto tabs = tabBar(entries, &selectedTab);

            return ftxui::Container::Vertical({getStatusLineComponent(rttReader),
                                               printer::tui::divider(),
                                               tabs | ftxui::flex})
                 | ftxui::border;
        }

        void exitScreen() {
            std::scoped_lock<std::mutex> const lock{screenMutex};
            if(screenPointer != nullptr) { screenPointer->Exit(); }
        }

    public:
        void add(std::chrono::system_clock::time_point recvTime,
                 uc_log::printer::LogEntry const&      entry) {
            store.addEntry(recvTime, entry);
        }

        /// The linker map the inspectors take their symbols from (--map_file). Before run().
        void setMapSource(std::function<std::shared_ptr<printer::MapSnapshot const>()> source) {
            mapSource = std::move(source);
        }

        // Before the ui runs.
        void setCompiledFilter(uc_log::detail::FilterTable const& table) {
            compiledFilter = table;
            ++compiledFilterVersion;
        }

        // the model's state, once per frame under the gui's mutex (model first, then the gui: PrinterModel.hpp)
        void refreshModelView(printer::PrinterModel::Snapshot const& m);

        void triggerRedraw() {
            std::scoped_lock<std::mutex> const lock{mutex};
            requestRedrawFromAnywhere();
        }

        int run(printer::TargetPort& rttReader,
                std::string const&   buildCommand,
                std::string const&   initialHost  = "",
                std::string const&   initialProbe = "");
    };
}}   // namespace uc_log::printer::tui
