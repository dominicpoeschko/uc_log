// the Statistics tab.
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::updateJLinkStatistics(printer::TargetPort& rttReader) -> void {
        // the counts are the model's (PrinterModel::sampleLink, before the gui's mutex); a flash that ended resets the
        // format errors: they belong to the image before the flash
        bool const flashing = rttReader.isFlashing();
        if(lastFlashing && !flashing) { messages.resetFormatErrors(); }
        lastFlashing = flashing;
    }

    auto Window::getStatisticsComponent() -> ftxui::Component {
        auto resetButton
          = printer::tui::button("🔄 Reset Statistics", printer::tui::Style::reset, [this]() {
                model.resetStatistics();
                statistics = model.statistics();
                store.resetLogStats();
            });

        return ftxui::Container::Vertical(
          {resetButton,
           printer::tui::divider(),
           scrollView([this]() {
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
                  ftxui::hbox({ftxui::text("  Disconnections: ") | ftxui::bold,
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
                  ftxui::hbox({ftxui::text("  Resets Detected: ") | ftxui::bold,
                               ftxui::text(fmt::format("{}", display.logStats.detectedResetCount))
                                 | ftxui::color(display.logStats.detectedResetCount > 0
                                                  ? Theme::Status::warning()
                                                  : Theme::Status::info())}),
                  ftxui::text(""),

                  ftxui::text("📝 Log Statistics") | ftxui::bold
                    | ftxui::color(Theme::Header::accent()),
                  ftxui::hbox({ftxui::text("  Retained Logs: ") | ftxui::bold,
                               ftxui::text(tui::formatCount(
                                 static_cast<std::uint64_t>(display.originalLogCount)))
                                 | ftxui::color(Theme::Status::info())}),
                  ftxui::hbox(
                    {ftxui::text("  Trimmed Logs: ") | ftxui::bold,
                     ftxui::text(
                       tui::formatCount(static_cast<std::uint64_t>(display.trimmedLogCount)))
                       | ftxui::color(display.trimmedLogCount > 0 ? Theme::Status::warning()
                                                                  : Theme::Status::info())}),
                  ftxui::hbox({ftxui::text("  Unparsed Logs: ") | ftxui::bold,
                               ftxui::text(tui::formatCount(
                                 static_cast<std::uint64_t>(display.logStats.parseFailureCount)))
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
                               ftxui::text(tui::formatBytes(
                                 static_cast<std::uint64_t>(statistics.maxBytesRead)))
                                 | ftxui::color(Theme::Status::info())}),
                  ftxui::hbox({ftxui::text("  Max Overflows: ") | ftxui::bold,
                               ftxui::text(tui::formatCount(
                                 static_cast<std::uint64_t>(statistics.maxOverflowCount)))
                                 | ftxui::color(statistics.maxOverflowCount > 0
                                                  ? Theme::Status::error()
                                                  : Theme::Status::success())})});
           }) | ftxui::flex});
    }

}}   // namespace uc_log::printer::tui
