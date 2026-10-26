#include "uc_log/printer/tui/Gui.hpp"

#include "uc_log/printer/tui/Window.hpp"

#include <memory>
#include <utility>

namespace uc_log::printer { namespace tui {

    auto Window::refreshModelView(printer::PrinterModel::Snapshot const& m) -> void {
        tcpPortStatus         = m.controlStatus;
        tcpCurrentPort        = m.controlPort;
        controlSocketPath     = m.controlSocketPath;
        tcpEnabled            = m.controlEnabled;
        tcpClientCount        = m.controlClients;
        networkBindAddress    = m.bindAddress;
        logFileStatus         = m.logFileStatus;
        logFileCurrentPath    = m.logFilePath;
        logFileEnabled        = m.logFileEnabled;
        firmwareState         = m.firmware;
        firmwareMismatchCount = m.firmwareMismatches;
        targetLostCount       = m.targetLost;
        // inputs are seeded from a new report only, never while somebody empties them
        if(m.controlStatusSerial != controlStatusSeen) {
            controlStatusSeen = m.controlStatusSerial;
            if(tcpPortInput.empty()) {
                tcpPortInput = std::to_string(static_cast<unsigned>(m.controlPort));
            }
        }
        if(m.logFileSerial != logFileSeen) {
            logFileSeen = m.logFileSerial;
            if(logDirInput.empty()) {
                logDirInput = std::filesystem::path{m.logFilePath}.parent_path().string();
            }
            if(exportDirInput.empty()) { exportDirInput = logDirInput; }
        }
        if(m.controlSocketPath != controlSocketSeen) {
            controlSocketSeen  = m.controlSocketPath;
            transportSelection = m.controlSocketPath.empty() ? 1 : 0;
        }
        if(bindAddressInput.empty()) { bindAddressInput = m.bindAddress; }
        if(duplexBasePortInput.empty()) {
            duplexBasePortInput = std::to_string(static_cast<unsigned>(m.duplexBasePort));
        }
    }

    auto Window::run(printer::TargetPort& rttReader,
                     std::string const&   buildCommand,
                     std::string const&   initialHost,
                     std::string const&   initialProbe) -> int {
        connectionTypeSelection = initialHost.empty() ? 0 : 1;
        ipAddressInput          = initialHost;
        probeInput              = initialProbe;
        buildRunner.initialize(buildCommand);
        {
            using uc_log::printer::TargetInspector;
            auto created = std::make_unique<TargetInspector>(TargetInspector::Hooks{
              .target  = &rttReader,
              .map     = mapSource,
              .changed = [this] { requestRedrawFromAnywhere(); },
              .note    = [this](std::string_view msg) { messages.status(msg); }});
            std::scoped_lock<std::mutex> const lock{inspectorMutex};
            inspector = std::move(created);
        }

        // the reader outlives run(): stop the inspector's reads before returning
        struct StopInspector {
            Window& gui;

            ~StopInspector() {
                std::unique_ptr<uc_log::printer::TargetInspector> gone;
                {
                    std::scoped_lock<std::mutex> const lock{gui.inspectorMutex};
                    gone = std::move(gui.inspector);
                }
            }
        } const stopInspector{*this};

        auto screen = ftxui::ScreenInteractive::Fullscreen();
        screen.ForceHandleCtrlC(true);
        ftxui::Component mainComponent;
        {
            std::scoped_lock<std::mutex> const lock{mutex};

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
                    if((c == '0' || c == '?') && tabCount > static_cast<int>(Tab::help)) {
                        selectedTab = static_cast<int>(Tab::help);
                        return true;
                    }
                    // Action hotkeys
                    if(c == '/') {
                        selectedTab    = static_cast<int>(Tab::logs);
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
            std::scoped_lock<std::mutex> const lock{screenMutex};
            screenPointer = &screen;
        }

        auto lastSettingsRefresh = std::chrono::steady_clock::now();
        while(!loop.HasQuitted()) {
            // one snapshot per frame: renderers never touch the store afterwards
            store.refreshMirror(display);
            buildRunner.snapshotOutput(buildOutputDisplay, buildOutputSeen);
            // the model before the gui's mutex (lock order, PrinterModel.hpp)
            model.sampleLink(rttReader.linkStatus());
            auto const modelView  = model.snapshot();
            auto const modelStats = model.statistics();
            {
                std::scoped_lock<std::mutex> const lock{mutex};
                updateJLinkStatistics(rttReader);
                refreshModelView(modelView);
                statistics = modelStats;
                messages.copyNew(statusMessages, statusClearsSeen);
                auto const counts = messages.counts();
                formatErrorCount  = counts.formatErrors;
                statusErrorCount  = counts.errors;
                refreshInspectorView(rttReader.isHalted());
                // the live window moves with the data; the input string follows unless
                // the user is editing it
                if(display.ucTimeLiveMode && !ucTimeMinInput->Focused()) {
                    minUcTimeStr = fmt::format("{:.1f}", display.minUcTimeSec);
                    maxUcTimeStr.clear();
                }
                // duplex byte counters and client state change without a posted event
                auto const now = std::chrono::steady_clock::now();
                if(selectedTab == settingsTabIndex
                   && now - lastSettingsRefresh >= GUI_Constants::settingsRefreshInterval)
                {
                    lastSettingsRefresh = now;
                    redrawPending.store(true, std::memory_order_relaxed);
                }
                // relay the producers' coalesced redraw request as a single event
                // per iteration
                if(redrawPending.exchange(false, std::memory_order_relaxed)) {
                    screen.PostEvent(ftxui::Event::Custom);
                }
                loop.RunOnce();
            }
            store.pollRefilterDeadline();
            for(auto& action : pendingActions) { action(); }
            pendingActions.clear();
            std::this_thread::sleep_for(GUI_Constants::updateInterval);
            buildRunner.joinIfFinished();
            if(buildRunner.triggerFlashNow.exchange(false)) {
                std::scoped_lock<std::mutex> const lock{mutex};
                flashWithStats(rttReader);
            }
        }
        {
            std::scoped_lock<std::mutex> const lock{screenMutex};
            screenPointer = nullptr;
        }

        return 0;
    }

}}   // namespace uc_log::printer::tui

namespace uc_log::printer::tui {

Gui::Gui(PrinterModel& model UC_LOG_LIFETIMEBOUND) : window_{std::make_unique<Window>(model)} {}

Gui::~Gui() = default;

void Gui::add(std::chrono::system_clock::time_point recvTime,
              printer::LogEntry const&              entry) {
    window_->add(recvTime, entry);
}

void Gui::setCompiledFilter(uc_log::detail::FilterTable const& table) {
    window_->setCompiledFilter(table);
}

void Gui::setMapSource(MapSource source) { window_->setMapSource(std::move(source)); }

void Gui::triggerRedraw() { window_->triggerRedraw(); }

int Gui::run(TargetPort&        target,
             std::string const& buildCommand,
             std::string const& initialHost,
             std::string const& initialProbe) {
    return window_->run(target, buildCommand, initialHost, initialProbe);
}

}   // namespace uc_log::printer::tui
