// the Debugger tab (probe, connection, core buttons).
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::syncProbeEntries(printer::TargetPort& rttReader) -> void {
        auto const version = rttReader.getProbesVersion();
        if(version == probesVersionSeen) { return; }
        probesVersionSeen = version;

        auto const probes = rttReader.getProbes();
        probeEntries.assign({"any (the only J-Link on USB)"});
        probeNames.assign({""});
        for(auto const& p : probes) {
            std::string entry = p.onUsb ? "USB " : "IP  ";
            if(!p.product.empty()) { entry += p.product + " "; }
            entry += std::to_string(p.serialNumber);
            if(!p.nickName.empty()) { entry += " \"" + p.nickName + "\""; }
            if(!p.onUsb) { entry += " (by serial number)"; }
            probeEntries.push_back(std::move(entry));
            probeNames.push_back(std::to_string(p.serialNumber));
        }

        probeSelection = 0;
        for(std::size_t i = 1; i < probeNames.size(); ++i) {
            auto const& p = probes[i - 1];
            if(probeInput == probeNames[i] || (!p.nickName.empty() && probeInput == p.nickName)) {
                probeSelection = static_cast<int>(i);
                break;
            }
        }
    }

    auto Window::getDebuggerComponent(printer::TargetPort& rttReader) -> ftxui::Component {
        auto resetTargetBtn
          = printer::tui::button("🔄 Reset Target [r]",
                                 printer::tui::Style::settings,
                                 [this, &rttReader]() { resetTargetWithStats(rttReader); });

        auto resetDebuggerBtn = printer::tui::button("🔌 Reset Debugger",
                                                     printer::tui::Style::reset,
                                                     [&rttReader]() { rttReader.resetJLink(); });

        auto flashBtn = printer::tui::button("⚡ Flash Target [f]",
                                             printer::tui::Style::positive,
                                             [this, &rttReader]() { flashWithStats(rttReader); });

        auto goBtn
          = printer::tui::button("▶️ Go", printer::tui::Style::positive, [this, &rttReader]() {
                sendCoreOp(rttReader, uc_log::printer::target_op::Go{});
            });

        auto haltBtn
          = printer::tui::button("⏸️ Halt", printer::tui::Style::danger, [this, &rttReader]() {
                // captured as a halt the core made itself (`core halted:`, the Health tab's crash)
                sendCoreOp(rttReader, uc_log::printer::target_op::Halt{.capture = true});
            });

        auto clearBreakpointsBtn = printer::tui::button(
          "🚫 Clear Breakpoints",
          printer::tui::Style::destructive,
          [this, &rttReader]() {
              sendCoreOp(rttReader, uc_log::printer::target_op::ClearBreakpoints{});
          });

        auto resetTypeRadiobox
          = ftxui::Radiobox(std::vector<std::string>{"0 - Normal", "1 - Core", "2 - ResetPin"},
                            &selectedResetType);

        auto resetTypeSelector
          = printer::tui::section(
              "🔧 Reset Type",
              Theme::Header::accent(),
              {resetTypeRadiobox,
               printer::tui::button("✓ Apply Reset Type",
                                    printer::tui::Style::positive,
                                    [this, &rttReader]() {
                                        sendCoreOp(rttReader,
                                                   uc_log::printer::target_op::SetResetType{
                                                     static_cast<std::uint8_t>(selectedResetType)});
                                    })})
          | ftxui::border;

        auto statusDisplay = ftxui::Renderer([&rttReader]() {
            auto const rttStatus = rttReader.linkStatus();

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
                  ftxui::text(
                    tui::formatCount(static_cast<std::uint64_t>(rttStatus.hostOverflowCount)))
                    | ftxui::color(rttStatus.hostOverflowCount == 0 ? Theme::Status::success()
                                                                    : Theme::Status::error())
                    | ftxui::bold}),

               ftxui::hbox({ftxui::text("Read: ") | ftxui::bold,
                            ftxui::text(tui::formatBytes(rttStatus.numBytesRead))
                              | ftxui::color(Theme::Status::info())}),

               ftxui::hbox(
                 {ftxui::text("Buffers: ") | ftxui::bold,
                  ftxui::text(
                    fmt::format("↑{} ↓{}", rttStatus.numUpBuffers, rttStatus.numDownBuffers))
                    | ftxui::color(Theme::Status::warning())})});
        });

        auto connTypeRadio = ftxui::Radiobox(std::vector<std::string>{"USB (local)", "IP (remote)"},
                                             &connectionTypeSelection);

        ipAddressInputComponent
          = trackInput(printer::tui::textInput(ipAddressInput, "host or IP address..."));
        auto ipInputMaybe = ftxui::Maybe(ipAddressInputComponent | ftxui::flex,
                                         [this]() { return connectionTypeSelection == 1; });

        // Picking a listed probe fills the name field; typing keeps working for a
        // probe that is not listed (yet).
        ftxui::RadioboxOption probeRadioOpts;
        probeRadioOpts.on_change = [this]() {
            if(probeSelection >= 0 && static_cast<std::size_t>(probeSelection) < probeNames.size())
            {
                probeInput = probeNames[static_cast<std::size_t>(probeSelection)];
            }
        };
        auto probeRadio = ftxui::Radiobox(&probeEntries, &probeSelection, probeRadioOpts);

        probeInputComponent
          = trackInput(printer::tui::textInput(probeInput, "serial number or nickname"));

        auto refreshProbesBtn = printer::tui::button("🔍 List Probes",
                                                     printer::tui::Style::settings,
                                                     [&rttReader]() { rttReader.refreshProbes(); });

        auto probePanel = ftxui::Container::Vertical(
                            {ftxui::Renderer([]() {
                                 return ftxui::text("Probe") | ftxui::bold
                                      | ftxui::color(Theme::Header::accent());
                             }),
                             probeRadio,
                             ftxui::Container::Horizontal({probeInputComponent | ftxui::flex})
                               | ftxui::Renderer([](ftxui::Element inner) {
                                     return ftxui::hbox(
                                       {ftxui::text("Name: ") | ftxui::color(Theme::Status::info()),
                                        std::move(inner) | ftxui::flex});
                                 }),
                             refreshProbesBtn})
                        | ftxui::Renderer([this, &rttReader](ftxui::Element inner) {
                              syncProbeEntries(rttReader);
                              return inner;
                          });
        auto probePanelMaybe
          = ftxui::Maybe(probePanel, [this]() { return connectionTypeSelection == 0; });

        auto applyConnBtn = printer::tui::button(
          "Apply Connection",
          printer::tui::Style::settings,
          [this, &rttReader]() {
              rttReader.setConnection({connectionTypeSelection == 0 ? "" : ipAddressInput,
                                       connectionTypeSelection == 0 ? probeInput : ""});
          });

        auto connPanel
          = ftxui::Container::Vertical({ftxui::Renderer([]() {
                                            return ftxui::text("Connection") | ftxui::bold
                                                 | ftxui::color(Theme::Header::accent());
                                        }),
                                        connTypeRadio,
                                        ipInputMaybe,
                                        probePanelMaybe,
                                        applyConnBtn})
          | ftxui::border;

        noLogTimeoutInput
          = trackInput(printer::tui::numberInput<int>(noLogTimeoutStr, "15", [&rttReader](int v) {
                if(v > 0) { rttReader.setNoLogTimeout(static_cast<std::uint32_t>(v)); }
            }));
        auto timeoutRow
          = ftxui::Container::Horizontal({noLogTimeoutInput})
          | ftxui::Renderer([](ftxui::Element inner) {
                return ftxui::hbox({ftxui::text("No-log reconnect timeout (s): ")
                                      | ftxui::color(Theme::Status::info()),
                                    std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6)});
            });

        return ftxui::Container::Vertical(
          {connPanel,
           printer::tui::divider(),
           timeoutRow,
           printer::tui::divider(),
           ftxui::Container::Horizontal({resetTargetBtn | ftxui::flex,
                                         resetDebuggerBtn | ftxui::flex,
                                         flashBtn | ftxui::flex}),
           printer::tui::divider(),
           ftxui::Container::Horizontal(
             {goBtn | ftxui::flex, haltBtn | ftxui::flex, clearBreakpointsBtn | ftxui::flex}),
           printer::tui::divider(),
           resetTypeSelector,
           printer::tui::divider(),
           statusDisplay});
    }

}}   // namespace uc_log::printer::tui
