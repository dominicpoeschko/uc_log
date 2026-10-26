// the Settings tab (display, log file, transport, bind address, control socket, duplex channels).
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getSettingsComponent() -> ftxui::Component {
        // ── Section A: Display ────────────────────────────────────────────────
        auto resetButton
          = printer::tui::button("🔄 Reset to Defaults", printer::tui::Style::settings, [this]() {
                showSysTime        = true;
                showFunctionName   = false;
                showFullSignature  = false;
                showUcTime         = true;
                showLocation       = true;
                showModule         = false;
                showChannel        = true;
                showLogLevel       = true;
                showMetricString   = false;
                showTypenameString = false;
            });

        // toggles aligned horizontally in two rows to keep the section flat
        auto const checkboxWidth  = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 24);
        auto       displaySection = ftxui::Container::Vertical(
          {ftxui::Container::Horizontal({ftxui::Renderer([]() {
                                             return ftxui::text("🎨 Display  ") | ftxui::bold
                                                  | ftxui::color(Theme::Header::accent());
                                         }),
                                         resetButton}),
           ftxui::Container::Horizontal(
             {printer::tui::spacer(),
              ftxui::Checkbox("⏰ System Time", &showSysTime) | checkboxWidth,
              ftxui::Checkbox("🕐 Target Time", &showUcTime) | checkboxWidth,
              ftxui::Checkbox("📡 Log Channel", &showChannel) | checkboxWidth,
              ftxui::Checkbox("📊 Log Level", &showLogLevel) | checkboxWidth}),
           ftxui::Container::Horizontal(
             {printer::tui::spacer(),
              ftxui::Checkbox("📍 Source Location", &showLocation) | checkboxWidth,
              ftxui::Checkbox("🔍 Function Names", &showFunctionName) | checkboxWidth,
              ftxui::Checkbox("📊 Metric Strings", &showMetricString) | checkboxWidth,
              ftxui::Checkbox("🔤 Typenames", &showTypenameString) | checkboxWidth,
              ftxui::Checkbox("🧩 Modules", &showModule) | checkboxWidth}),
           ftxui::Container::Horizontal(
             {printer::tui::spacer(),
              ftxui::Checkbox("📜 Full Signatures", &showFullSignature) | checkboxWidth})});

        // ── Section B: File Logging ───────────────────────────────────────────
        logDirInputComponent = trackInput(printer::tui::textInput(logDirInput, "path/to/log/dir"));

        auto logToggleBtn = printer::tui::button(" Toggle ",
                                                 printer::tui::Style::settings,
                                                 [this]() { model.toggleLogFile(); });

        auto logApplyBtn = printer::tui::button(" Apply ", printer::tui::Style::positive, [this]() {
            if(logDirInput.empty()) { return; }
            pendingActions.emplace_back([this, dir = logDirInput]() { model.applyLogDir(dir); });
        });

        auto logSection = ftxui::Container::Vertical(
          {ftxui::Renderer([this]() {
               auto const badge = printer::tui::badgeOf(logFileStatus, logFileEnabled);
               return ftxui::vbox(
                 {ftxui::hbox({ftxui::text("📄 File Logging  ") | ftxui::bold
                                 | ftxui::color(Theme::Header::accent()),
                               printer::tui::render(badge)}),
                  ftxui::hbox({ftxui::text("  File: ") | ftxui::bold,
                               ftxui::text(!logFileCurrentPath.empty() ? logFileCurrentPath : "—")
                                 | ftxui::color(logFileStatus == LogFileStatus::active
                                                  ? Theme::Status::info()
                                                  : Theme::Text::normal())}),
                  logFileStatus == LogFileStatus::error
                    ? ftxui::text(fmt::format("  ✘  Cannot write to: {}", logFileCurrentPath))
                        | ftxui::color(Theme::Status::error())
                    : ftxui::text("")});
           }),
           ftxui::Container::Horizontal(
             {logToggleBtn,
              ftxui::Renderer([]() { return ftxui::text("  Directory: "); }),
              logDirInputComponent | ftxui::border | ftxui::flex,
              printer::tui::spacer(),
              logApplyBtn})});

        // ── Section C: Network / TCP ──────────────────────────────────────────
        tcpPortInputComponent = trackInput(printer::tui::portInput(tcpPortInput, "1024–65535"));

        auto tcpToggleBtn
          = printer::tui::button(" Toggle ", printer::tui::Style::settings, [this]() {
                model.setControlEnabled(!tcpEnabled);
            });

        auto portApplyBtn
          = printer::tui::button(" Apply ", printer::tui::Style::positive, [this]() {
                static_cast<void>(model.applyControlPort(tcpPortInput));
            });

        // ── Bind address ───────────────────────────────────────────────────────
        bindAddressInputComponent
          = trackInput(printer::tui::textInput(bindAddressInput, "127.0.0.1"));

        auto bindApplyBtn
          = printer::tui::button(" Apply ", printer::tui::Style::positive, [this]() {
                if(bindAddressInput.empty()) { return; }
                if(model.applyBindAddress(bindAddressInput)) {
                    networkBindAddress = bindAddressInput;
                    bindAddressStatus.clear();
                } else {
                    bindAddressStatus = fmt::format("invalid address: {}", bindAddressInput);
                }
            });

        auto makeBindPresetBtn = [this](std::string label, std::string address) {
            return printer::tui::button(std::move(label),
                                        printer::tui::Style::settings,
                                        [this, addr = std::move(address)]() {
                                            if(model.applyBindAddress(addr)) {
                                                networkBindAddress = addr;
                                                bindAddressInput   = addr;
                                                bindAddressStatus.clear();
                                            }
                                        });
        };
        auto bindLoopbackBtn = makeBindPresetBtn(" Loopback ", "127.0.0.1");
        auto bindAllBtn      = makeBindPresetBtn(" All Interfaces ", "0.0.0.0");

        auto bindAddrSection = ftxui::Container::Vertical(
          {ftxui::Renderer([this]() {
               bool const exposed = !printer::isLoopbackAddress(networkBindAddress);
               return ftxui::vbox(
                 {ftxui::hbox(
                    {ftxui::text("🔌 Socket Bind Address  ") | ftxui::bold
                       | ftxui::color(Theme::Header::accent()),
                     ftxui::text(networkBindAddress)
                       | ftxui::color(exposed ? Theme::Status::error() : Theme::Status::success())
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
              printer::tui::spacer(),
              bindApplyBtn,
              printer::tui::spacer(),
              bindLoopbackBtn,
              printer::tui::spacer(),
              bindAllBtn})});

        auto netSection = ftxui::Container::Vertical(
          {ftxui::Renderer([this]() {
               auto const badge       = printer::tui::badgeOf(tcpPortStatus, tcpEnabled);
               auto const clientCount = tcpClientCount;
               bool const inputValid  = printer::parsePort(tcpPortInput).has_value();
               bool const inputChanged
                 = !tcpPortInput.empty()
                && tcpPortInput != std::to_string(static_cast<unsigned>(tcpCurrentPort));

               return ftxui::vbox(
                 {ftxui::hbox(
                    {ftxui::text("🌐 Control  ") | ftxui::bold
                       | ftxui::color(Theme::Header::accent()),
                     printer::tui::render(badge),
                     ftxui::text("  "),
                     !controlSocketPath.empty()
                       ? ftxui::text(controlSocketPath) | ftxui::color(Theme::Status::info())
                     : tcpCurrentPort > 0
                       ? ftxui::text(fmt::format("{}:{}", networkBindAddress, tcpCurrentPort))
                           | ftxui::color(Theme::Status::info())
                       : ftxui::text("—") | ftxui::color(Theme::Text::normal()),
                     ftxui::text("  clients ") | ftxui::bold,
                     ftxui::text(std::to_string(clientCount))
                       | ftxui::color(clientCount > 0 ? Theme::Status::success()
                                                      : Theme::Text::normal())}),
                  tcpPortStatus == SocketPortStatus::portOccupied
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
              printer::tui::spacer(),
              portApplyBtn})});

        // ── Transport ───────────────────────────────────────────────────────────
        ftxui::MenuOption transportOpts = ftxui::MenuOption::Toggle();
        transportOpts.on_change         = [this]() {
            // deferred: outside the event handling (it rebinds sockets)
            pendingActions.emplace_back(
              [this, unix = transportSelection == 0]() { model.setTransport(unix); });
        };
        auto transportToggle  = ftxui::Menu(&transportEntries, &transportSelection, transportOpts);
        auto transportSection = ftxui::Container::Vertical(
          {ftxui::Renderer([this]() {
               return ftxui::vbox(
                 {ftxui::text("🔌 Transport") | ftxui::bold | ftxui::color(Theme::Header::accent()),
                  ftxui::text(transportSelection == 0
                                ? "  control.sock and duplex.<n>.sock in the log directory"
                                : "  the control port and the duplex ports below, on the "
                                  "bind address")
                    | ftxui::color(Theme::Status::inactive())});
           }),
           ftxui::Container::Horizontal(
             {ftxui::Renderer([]() { return ftxui::text("  Serve on: "); }), transportToggle})});

        // ── Combine all sections, each in its own frame like the Debugger tab ─
        return ftxui::Container::Vertical({displaySection | ftxui::border,
                                           logSection | ftxui::border,
                                           transportSection | ftxui::border,
                                           bindAddrSection | ftxui::border,
                                           netSection | ftxui::border,
                                           getDuplexSection() | ftxui::border});
    }

    auto Window::getDuplexSection() -> ftxui::Component {
        duplexBasePortInputComponent
          = trackInput(printer::tui::portInput(duplexBasePortInput, "1024–65535"));

        auto basePortApplyBtn
          = printer::tui::button(" Apply to all ", printer::tui::Style::positive, [this]() {
                static_cast<void>(model.applyDuplexBasePort(duplexBasePortInput));
            });

        auto getInfos = [this]() { return model.duplexInfo(); };

        auto headerRenderer = ftxui::Renderer([getInfos]() {
            auto const infos = getInfos();
            return ftxui::hbox({ftxui::text("🔁 Duplex Channels  ") | ftxui::bold
                                  | ftxui::color(Theme::Header::accent()),
                                infos.empty() ? ftxui::text("none reported by the target")
                                                  | ftxui::color(Theme::Text::normal())
                                              : ftxui::text("one client per channel, first wins")
                                                  | ftxui::color(Theme::Status::info())});
        });

        std::vector<ftxui::Component> channelRows;
        for(std::size_t i{}; i < printer::PrinterModel::maxDuplexChannels; ++i) {
            duplexPortInputComponents[i]
              = trackInput(printer::tui::portInput(duplexPortInputs[i], "port"));

            auto toggleBtn = printer::tui::button(" Toggle ",
                                                  printer::tui::Style::settings,
                                                  [this, i]() { model.toggleDuplex(i); });

            auto applyBtn
              = printer::tui::button(" Apply ", printer::tui::Style::positive, [this, i]() {
                    static_cast<void>(model.applyDuplexPort(i, duplexPortInputs[i]));
                });

            auto infoRenderer = ftxui::Renderer([this, getInfos, i]() {
                auto const infos = getInfos();
                if(i >= infos.size()) { return ftxui::text(""); }
                auto const& info = infos[i];
                // do not reseed while the user is editing, it would fight the clearing
                if(duplexPortInputs[i].empty()
                   && !(duplexPortInputComponents[i] && duplexPortInputComponents[i]->Focused()))
                {
                    duplexPortInputs[i] = std::to_string(static_cast<unsigned>(info.port));
                }
                auto const badge = printer::tui::badgeOf(info.status, info.enabled);
                return ftxui::hbox(
                  {ftxui::text(fmt::format("  {}  ", info.ordinal)) | ftxui::bold,
                   ftxui::text(info.name) | ftxui::bold | ftxui::color(Theme::Header::accent()),
                   info.hostToTargetOnly
                     ? ftxui::text("  (host→target only)") | ftxui::color(Theme::Status::warning())
                     : ftxui::text(""),
                   ftxui::text("  "),
                   printer::tui::render(badge),
                   ftxui::text("  "),
                   ftxui::text(info.socketPath.empty()
                                 ? fmt::format("{}:{}", networkBindAddress, info.port)
                                 : info.socketPath)
                     | ftxui::color(Theme::Status::info()),
                   ftxui::text("  client ") | ftxui::bold,
                   ftxui::text(info.connected ? "●" : "○")
                     | ftxui::color(info.connected ? Theme::Status::success()
                                                   : Theme::Text::normal()),
                   ftxui::text("  "),
                   ftxui::text(fmt::format("→uc {}  uc→ {}",
                                           tui::formatBytes(info.bytesToTarget),
                                           tui::formatBytes(info.bytesFromTarget)))
                     | ftxui::color(Theme::Status::info()),
                   info.bytesDropped != 0
                     ? ftxui::text(fmt::format("  dropped {}", tui::formatBytes(info.bytesDropped)))
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
                  printer::tui::spacer(),
                  applyBtn})});

            channelRows.push_back(
              ftxui::Maybe(row, [getInfos, i]() { return i < getInfos().size(); }));
        }

        auto basePortRow = ftxui::Container::Horizontal(
          {ftxui::Renderer([]() { return ftxui::text("  Base port: ") | ftxui::bold; }),
           duplexBasePortInputComponent | ftxui::border
             | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 13),
           printer::tui::spacer(),
           basePortApplyBtn});

        std::vector<ftxui::Component> components{headerRenderer, basePortRow};
        for(auto& row : channelRows) { components.push_back(std::move(row)); }
        return ftxui::Container::Vertical(std::move(components));
    }

}}   // namespace uc_log::printer::tui
