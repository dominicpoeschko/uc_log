// the Build tab.
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getBuildComponent() -> ftxui::Component {
        auto clearButton = printer::tui::button("🗑️ Clear Output",
                                                printer::tui::Style::destructive,
                                                [this]() { buildRunner.clearOutput(); });

        auto stopButton = printer::tui::button("⏸ Stop Build",
                                               printer::tui::Style::danger,
                                               [this]() { buildRunner.cancel(); });

        auto buildButton = printer::tui::button("🔨 Start Build [b]",
                                                printer::tui::Style::positive,
                                                [this]() { executeBuild(); });

        auto buildAndFlashButton = printer::tui::button("🔨⚡ Build & Flash [shift+F]",
                                                        printer::tui::Style::positive,
                                                        [this]() { executeBuildAndFlash(); });

        auto outputScroller = scroller(
          [this]() -> std::vector<printer::BuildEntry> const& { return buildOutputDisplay; },
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
           printer::tui::divider(),
           statusDisplay | ftxui::flex});
    }

}}   // namespace uc_log::printer::tui
