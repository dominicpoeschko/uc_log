// the status line under the tabs.
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getStatusLineComponent(printer::TargetPort& rttReader) -> ftxui::Component {
        auto quitBtn = printer::tui::button("[q]uit", printer::tui::Style::destructive, [this]() {
            exitScreen();
        });

        auto resetBtn
          = printer::tui::button("[r]eset", printer::tui::Style::reset, [this, &rttReader]() {
                resetTargetWithStats(rttReader);
            });

        auto flashBtn = printer::tui::button("[f]lash",
                                             printer::tui::Style::positive,
                                             [this, &rttReader]() { flashWithStats(rttReader); });

        auto buildBtn = printer::tui::button("[b]uild", printer::tui::Style::build, [this]() {
            executeBuild();
        });

        auto statusRenderer = ftxui::Renderer([&rttReader, this]() {
            auto const rttStatus      = rttReader.linkStatus();
            auto const logCount       = display.filteredOriginalLogCount;
            auto const totalCount     = display.originalLogCount;
            bool const filterActive   = display.filterActive;
            auto const refilter       = store.refilterProgress();
            auto const buildStatusNow = buildRunner.getStatus();
            bool const buildRunning   = (buildStatusNow == printer::BuildStatus::running);
            bool const buildSuccess   = (buildStatusNow == printer::BuildStatus::success);
            bool const isFlashing     = rttReader.isFlashing();

            return ftxui::hbox(
              {ftxui::text("🔗 " + std::string(rttStatus.isRunning != 0 ? "●" : "○"))
                 | ftxui::color(rttStatus.isRunning != 0 ? Theme::Status::success()
                                                         : Theme::Text::normal()),
               ftxui::separator(),

               ftxui::text("🔍 " + std::string(filterActive ? "●" : "○"))
                 | ftxui::color(filterActive ? Theme::Status::success() : Theme::Text::normal()),
               ftxui::separator(),

               ftxui::text("🔨 "
                           + std::string((buildRunning || buildSuccess
                                          || buildStatusNow == printer::BuildStatus::failed)
                                           ? "●"
                                           : "○"))
                 | ftxui::color([&]() {
                       if(buildRunning) { return Theme::Status::warning(); }
                       if(buildSuccess) { return Theme::Status::success(); }
                       if(buildStatusNow == printer::BuildStatus::failed) {
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

               ftxui::text(fmt::format("LOGS {}/{}",
                                       tui::formatCount(static_cast<std::uint64_t>(logCount)),
                                       tui::formatCount(static_cast<std::uint64_t>(totalCount))))
                 | ftxui::color(Theme::Status::info()),
               display.searchText.empty() ? ftxui::text("") : ftxui::separator(),
               display.searchText.empty()
                 ? ftxui::text("")
                 : ftxui::text(fmt::format("🔎 {:?} → {}",
                                           display.searchText,
                                           tui::formatCount(static_cast<std::uint64_t>(logCount))))
                     | ftxui::color(Theme::Status::warning()),
               refilter.inProgress ? ftxui::separator() : ftxui::text(""),
               refilter.inProgress
                 ? ftxui::text(fmt::format(
                     "⏳ filtering… {}%",
                     refilter.total == 0 ? 100 : (refilter.scanned * 100) / refilter.total))
                     | ftxui::color(Theme::Status::warning())
                 : ftxui::text(""),
               ftxui::separator(),

               ftxui::text(fmt::format("DATA {}", tui::formatBytes(rttStatus.numBytesRead)))
                 | ftxui::color(Theme::Status::warning()),
               ftxui::separator(),

               ftxui::text(fmt::format(
                 "OVFL {}",
                 tui::formatCount(static_cast<std::uint64_t>(rttStatus.hostOverflowCount))))
                 | ftxui::color(rttStatus.hostOverflowCount == 0 ? Theme::Status::success()
                                                                 : Theme::Status::error()),
               ftxui::separator(),

               ftxui::text(
                 formatErrorCount == 0
                   ? std::string{"📝 ○"}
                   : fmt::format("📝 ✘ {}",
                                 tui::formatCount(static_cast<std::uint64_t>(formatErrorCount))))
                 | ftxui::color(formatErrorCount == 0 ? Theme::Text::normal()
                                                      : Theme::Status::error())
                 | (formatErrorCount == 0 ? ftxui::nothing : ftxui::bold),
               ftxui::separator(),
               ftxui::text(
                 statusErrorCount == 0
                   ? std::string{"⚠ 0"}
                   : fmt::format("⚠ {}",
                                 tui::formatCount(static_cast<std::uint64_t>(statusErrorCount))))
                 | ftxui::color(statusErrorCount == 0 ? Theme::Text::normal()
                                                      : Theme::Status::error())
                 | (statusErrorCount == 0 ? ftxui::nothing : ftxui::bold),
               ftxui::separator(),
               // records the target dropped: "-" when the firmware does not count them
               ftxui::text(
                 !targetLostCount
                   ? std::string{"📉 -"}
                   : fmt::format("📉 {}",
                                 tui::formatCount(static_cast<std::uint64_t>(*targetLostCount))))
                 | ftxui::color(targetLostCount.value_or(0) == 0 ? Theme::Text::normal()
                                                                 : Theme::Status::error())
                 | (targetLostCount.value_or(0) == 0 ? ftxui::nothing : ftxui::bold),
               ftxui::separator(),
               ftxui::text([this]() -> std::string {
                   auto const n = firmwareMismatchCount == 0
                                  ? std::string{}
                                  : fmt::format(" ({}x)", firmwareMismatchCount);
                   switch(firmwareState) {
                   case control::FirmwareState::match:     return "FW ✔" + n;
                   case control::FirmwareState::different: return "FW ✘ MISMATCH" + n;
                   case control::FirmwareState::unchecked: return "FW ?" + n;
                   }
                   return "FW ?";
               }())
                 | ftxui::color(
                   firmwareState == control::FirmwareState::different ? Theme::Status::error()
                   : firmwareMismatchCount != 0                       ? Theme::Status::warning()
                   : firmwareState == control::FirmwareState::match   ? Theme::Status::success()
                                                                      : Theme::Text::normal())
                 | (firmwareState == control::FirmwareState::different ? ftxui::bold
                                                                       : ftxui::nothing),
               [this]() -> ftxui::Element {
                   auto const&                 v = inspectorView;
                   std::vector<ftxui::Element> badges;
                   if(v.haveUbsan && v.ubsan) {
                       bool const bad = v.ubsan->count != 0;
                       badges.push_back(ftxui::separator());
                       badges.push_back(
                         ftxui::text(bad ? fmt::format("UB {}", v.ubsan->count)
                                         : std::string{"UB 0"})
                         | ftxui::color(bad ? Theme::Status::error() : Theme::Text::normal())
                         | (bad ? ftxui::bold : ftxui::nothing));
                   }
                   if(v.fault || v.panic) {
                       badges.push_back(ftxui::separator());
                       badges.push_back(ftxui::text(v.panic ? "🚨 PANIC" : "📕 FAULT")
                                        | ftxui::color(Theme::Status::error()) | ftxui::bold);
                   }
                   if(targetHalted) {
                       badges.push_back(ftxui::separator());
                       badges.push_back(ftxui::text("⏸ HALTED")
                                        | ftxui::color(Theme::Status::error()) | ftxui::bold);
                   }
                   return ftxui::hbox(std::move(badges));
               }(),
               display.trimmedLogCount > 0 ? ftxui::separator() : ftxui::text(""),
               display.trimmedLogCount > 0
                 ? ftxui::text(fmt::format(
                     "♻ TRIM {}",
                     tui::formatCount(static_cast<std::uint64_t>(display.trimmedLogCount))))
                     | ftxui::color(Theme::Status::warning()) | ftxui::bold
                 : ftxui::text(""),
               ftxui::separator(),
               printer::tui::render(printer::tui::iconOf(logFileStatus, logFileEnabled)),
               ftxui::separator(),
               printer::tui::render(printer::tui::iconOf(tcpPortStatus, tcpEnabled)),
               [this]() -> ftxui::Element {
                   auto const infos = model.duplexInfo();
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

        auto hotkeyContainer = ftxui::Container::Horizontal({quitBtn,
                                                             printer::tui::barDivider(),
                                                             resetBtn,
                                                             printer::tui::barDivider(),
                                                             flashBtn,
                                                             printer::tui::barDivider(),
                                                             buildBtn});

        return ftxui::Container::Horizontal({statusRenderer | ftxui::flex, hotkeyContainer});
    }

}}   // namespace uc_log::printer::tui
