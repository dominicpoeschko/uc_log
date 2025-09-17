#pragma once

#include "uc_log/FTXUI_Utils.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/metric_utils.hpp"
#include "uc_log/theme.hpp"

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wredundant-decls"
    #pragma GCC diagnostic ignored "-Woverloaded-virtual"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
    #pragma GCC diagnostic ignored "-Wshadow"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
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
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace uc_log { namespace FTXUIGui {

    struct Gui {
        Gui() = default;

        ~Gui() {
            if(buildThread.joinable()) {
                buildThread.request_stop();
                if(buildIoContext) {
                    buildIoContext->stop();
                }
                buildThread.join();
            }
        }

        Gui(Gui const&)            = delete;
        Gui& operator=(Gui const&) = delete;

        Gui(Gui&&)            = delete;
        Gui& operator=(Gui&&) = delete;

    private:
        struct GuiLogEntry {
            std::chrono::system_clock::time_point recv_time;
            uc_log::detail::LogEntry              logEntry;
        };

        struct FilterState {
            std::set<uc_log::LogLevel> enabledLogLevels;
            std::set<std::size_t>      enabledChannels;
            std::set<SourceLocation>   enabledLocations;

            bool operator==(FilterState const&) const = default;
        };

        struct MessageEntry {
            enum class Level { Fatal, Error, Status, ToolError, ToolStatus };

            Level                                 level;
            std::chrono::system_clock::time_point time;
            std::string                           message;
        };

        struct BuildEntry {
            std::chrono::system_clock::time_point time;
            std::string                           line;
            bool                                  fromTool;
            bool                                  isError;
        };

        enum class BuildStatus { Idle, Running, Success, Failed };

        static constexpr auto NoFilter = [](GuiLogEntry const&) { return true; };

        std::mutex mutex;

        std::atomic<bool> callJoin{false};

        ftxui::ScreenInteractive* screenPointer = nullptr;

        std::map<SourceLocation, std::size_t>           allSourceLocations;
        std::vector<std::shared_ptr<GuiLogEntry const>> allLogEntries;
        std::vector<std::shared_ptr<GuiLogEntry const>> filteredLogEntries;
        std::map<MetricInfo, std::vector<MetricEntry>>  metricEntries;

        FTXUIGui::MetricPlotWidget metricPlotWidget;

        FilterState activeFilterState;
        FilterState editedFilterState;

        std::function<bool(GuiLogEntry const&)> currentFilter = NoFilter;

        bool showSysTime{true};
        bool showFunctionName{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showChannel{true};
        bool showLogLevel{true};
        bool showMetricString{false};

        std::size_t lastMetricCount{0};
        bool        hasLastSelectedInfo{false};
        MetricInfo  lastSelectedInfo;

        std::vector<MessageEntry> statusMessages;

        std::vector<BuildEntry> buildOutput;
        BuildStatus             buildStatus = BuildStatus::Idle;

        std::vector<std::string> originalBuildArguments;
        boost::filesystem::path  originalBuildExecutablePath;

        std::vector<std::string> buildArguments;
        boost::filesystem::path  buildExecutablePath;
        std::vector<std::string> buildEnvironment;

        int            selectedLocationIndex{};
        SourceLocation selectedSourceLocation;
        std::string    locationFilterInput;

        int selectedTab{};

        int selectedMetricTab{};

        std::unique_ptr<boost::asio::io_context> buildIoContext;
        std::jthread                             buildThread;

        void addBuildOutput(std::string const& line,
                            bool               fromTool,
                            bool               isError) {
            std::lock_guard<std::mutex> lock{mutex};
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, fromTool, isError);
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        void addBuildOutputGui(std::string const& line,
                               bool               isError) {
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, false, isError);
        }

        void initializeBuildCommand(std::string const& buildCommandStr) {
            std::string currentArgument;
            bool        in_quotes   = false;
            bool        escape_next = false;

            for(char c : buildCommandStr) {
                if(escape_next) {
                    currentArgument += c;
                    escape_next = false;
                } else if(c == '\\') {
                    escape_next = true;
                } else if(c == '"' || c == '\'') {
                    in_quotes = !in_quotes;
                } else if(c == ' ' && !in_quotes) {
                    if(!currentArgument.empty()) {
                        originalBuildArguments.push_back(currentArgument);
                        currentArgument.clear();
                    }
                } else {
                    currentArgument += c;
                }
            }

            if(!currentArgument.empty()) {
                originalBuildArguments.push_back(currentArgument);
            }

            if(originalBuildArguments.empty()) {
                throw std::invalid_argument("empty build command");
            }

            originalBuildExecutablePath
              = boost::process::environment::find_executable(originalBuildArguments[0]);

            if(originalBuildExecutablePath.empty()) {
                throw std::invalid_argument(
                  fmt::format("executable {} not found", originalBuildArguments[0]));
            }

            originalBuildArguments.erase(originalBuildArguments.begin());

            auto const scriptPath = boost::process::environment::find_executable("script");
            if(!scriptPath.empty()) {
                buildArguments.push_back("-q");
                buildArguments.push_back("-c");

                std::string commandStr = originalBuildExecutablePath.string();
                for(auto const& arg : originalBuildArguments) {
                    commandStr += " ";
                    if(arg.find(' ') != std::string::npos) {
                        commandStr += "\"" + arg + "\"";
                    } else {
                        commandStr += arg;
                    }
                }
                buildArguments.push_back(commandStr);
                buildArguments.push_back("/dev/null");

                buildExecutablePath = scriptPath;
            } else {
                buildArguments      = originalBuildArguments;
                buildExecutablePath = originalBuildExecutablePath;
            }

            for(auto const var : boost::process::environment::current()) {
                buildEnvironment.push_back(var.string());
            }

            buildEnvironment.push_back("FORCE_COLOR=1");
            buildEnvironment.push_back("CLICOLOR_FORCE=1");
            buildEnvironment.push_back("COLORTERM=truecolor");
            buildEnvironment.push_back("CMAKE_COLOR_DIAGNOSTICS=ON");
            buildEnvironment.push_back("NINJA_STATUS=[%f/%t] ");
        }

        void cancelBuild() {
            try {
                if(buildStatus != BuildStatus::Running || !buildIoContext) {
                    return;
                }

                if(buildThread.joinable()) {
                    buildThread.request_stop();
                    buildIoContext->stop();
                }

            } catch(std::exception const& e) {
                addBuildOutputGui(fmt::format("❌ Error stopping build: {}", e.what()), true);
            }
        }

        void executeBuild() {
            if(buildStatus == BuildStatus::Running || buildThread.joinable()) {
                return;
            }

            buildOutput.clear();

            buildStatus = BuildStatus::Running;

            try {
                buildIoContext = std::make_unique<boost::asio::io_context>();

                addBuildOutputGui(fmt::format("🚀 Starting process: {} {}",
                                              originalBuildExecutablePath.string(),
                                              originalBuildArguments),
                                  false);

                buildThread = std::jthread{[this](std::stop_token stoken) {
                    try {
                        std::string                stdoutBuffer;
                        std::string                stderrBuffer;
                        boost::asio::readable_pipe stdoutPipe{*buildIoContext};
                        boost::asio::readable_pipe stderrPipe{*buildIoContext};

                        boost::process::v2::process buildProcess{
                          *buildIoContext,
                          buildExecutablePath,
                          buildArguments,
                          boost::process::v2::process_stdio{nullptr, stdoutPipe, stderrPipe},
                          boost::process::process_environment{buildEnvironment}
                        };

                        auto createRead =
                          [this](auto& pipe, auto& buffer, auto& self, bool isError) {
                              return [this, &pipe, &buffer, &self, isError]() {
                                  boost::asio::async_read_until(
                                    pipe,
                                    boost::asio::dynamic_buffer(buffer),
                                    '\n',
                                    [this, &buffer, &self, isError](boost::system::error_code ec,
                                                                    std::size_t bytes_transferred) {
                                        if(!ec && bytes_transferred > 0) {
                                            auto pos = buffer.find('\n');
                                            if(pos != std::string::npos) {
                                                std::string line = buffer.substr(0, pos);
                                                buffer.erase(0, pos + 1);
                                                addBuildOutput(line, true, isError);
                                            }
                                            self();
                                        }
                                    });
                              };
                          };

                        std::function<void(void)> readOut;
                        readOut = createRead(stdoutPipe, stdoutBuffer, readOut, false);
                        std::function<void(void)> readErr;
                        readErr = createRead(stderrPipe, stderrBuffer, readErr, true);

                        readOut();
                        readErr();

                        int  processExitCode = 1;
                        bool completed       = false;

                        buildProcess.async_wait(
                          [this, &processExitCode, &completed](boost::system::error_code ec,
                                                               int                       exitCode) {
                              processExitCode = exitCode;
                              if(ec) {
                                  addBuildOutput(fmt::format("❌ Process error: {}", ec.message()),
                                                 false,
                                                 true);
                              } else {
                                  completed = true;
                                  addBuildOutput(fmt::format("🏁 Build {} (exit code: {})",
                                                             exitCode == 0 ? "succeeded" : "failed",
                                                             exitCode),
                                                 false,
                                                 exitCode != 0);
                              }
                              buildThread.request_stop();
                          });
                        while(!stoken.stop_requested()) {
                            buildIoContext->run_one_for(std::chrono::milliseconds{100});
                        }
                        if(!completed && buildProcess.running()) {
                            addBuildOutput("❌ Build ended by user", false, true);
                            buildProcess.terminate();
                        }

                        {
                            std::lock_guard<std::mutex> lock{mutex};
                            buildStatus
                              = (processExitCode == 0) ? BuildStatus::Success : BuildStatus::Failed;
                        }

                    } catch(std::exception const& e) {
                        {
                            std::lock_guard<std::mutex> lock{mutex};
                            buildStatus = BuildStatus::Failed;
                        }

                        addBuildOutput(fmt::format("❌ Build error: {}", e.what()), false, true);
                    }

                    callJoin = true;
                }};
            } catch(std::exception const& e) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui(fmt::format("❌ Build error: {}", e.what()), true);
            }
        }

        std::string processLogMessage(std::string const& originalMsg) const {
            if(showMetricString) {
                return originalMsg;
            }

            std::string processedMsg = originalMsg;
            std::size_t pos          = 0;

            while((pos = processedMsg.find("@METRIC(", pos)) != std::string::npos) {
                std::size_t start_pos = pos;
                pos += 8;

                std::size_t end_pos = processedMsg.find(')', pos);
                if(end_pos == std::string::npos) {
                    break;
                }

                std::string_view metric_content
                  = std::string_view{processedMsg}.substr(pos, end_pos - pos);

                std::size_t equals_pos = metric_content.find('=');
                if(equals_pos != std::string_view::npos) {
                    std::string value{metric_content.substr(equals_pos + 1)};
                    processedMsg.replace(start_pos, end_pos - start_pos + 1, value);
                    pos = start_pos + value.length();
                } else {
                    pos = end_pos + 1;
                }
            }

            return processedMsg;
        }

        auto defaultRender(GuiLogEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(12);

            if(showSysTime) {
                elements.push_back(
                  ftxui::text(detail::to_time_string_with_milliseconds(e.recv_time))
                  | ftxui::color(Theme::Text::timestamp()));
                elements.push_back(ftxui::text(" "));
            }

            if(showChannel) {
                elements.push_back(toElement(e.logEntry.channel));
                elements.push_back(ftxui::text(" "));
            }

            if(showUcTime) {
                elements.push_back(ftxui::text(fmt::format("{}", e.logEntry.ucTime))
                                   | ftxui::color(Theme::Text::ucTime()));
                elements.push_back(ftxui::text(" "));
            }

            if(showLogLevel) {
                elements.push_back(toElement(e.logEntry.logLevel));
                elements.push_back(ftxui::text("│ ") | ftxui::color(Theme::Text::separator()));
            }

            elements.push_back(ansiColoredTextToFtxui(processLogMessage(e.logEntry.logMsg)));

            auto scrollableContent = ftxui::hbox(elements) | ftxui::flex;

            ftxui::Elements metadata;
            if(showFunctionName) {
                metadata.push_back(ftxui::text(e.logEntry.functionName)
                                   | ftxui::color(Theme::Text::functionName()));
            }

            if(showLocation) {
                if(!metadata.empty()) {
                    metadata.push_back(ftxui::text(" "));
                }
                metadata.push_back(
                  ftxui::text(fmt::format("{}:{}", e.logEntry.fileName, e.logEntry.line))
                  | ftxui::color(Theme::Text::metadata()));
            }

            ftxui::Element metadataElement
              = metadata.empty() ? ftxui::text("") : ftxui::hbox(metadata);

            return std::make_shared<ScrollableWithMetadata>(std::move(scrollableContent),
                                                            std::move(metadataElement));
        }

        ftxui::Element renderMessage(MessageEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(e.time))
                               | ftxui::color(Theme::Text::timestamp()));

            elements.push_back(ftxui::text(" | ") | ftxui::color(Theme::Text::normal()));

            auto messageColor
              = e.level == MessageEntry::Level::Fatal     ? ftxui::color(Theme::Message::fatal())
              : e.level == MessageEntry::Level::Error     ? ftxui::color(Theme::Message::error())
              : e.level == MessageEntry::Level::Status    ? ftxui::color(ftxui::Color::White)
              : e.level == MessageEntry::Level::ToolError ? ftxui::color(ftxui::Color::RedLight)
                                                          : ftxui::color(ftxui::Color::Cyan);

            elements.push_back(ftxui::text(e.message) | messageColor | ftxui::flex);

            return ftxui::hbox(elements);
        }

        void updateFilteredLogEntries() {
            filteredLogEntries.clear();
            std::ranges::copy_if(allLogEntries,
                                 std::back_inserter(filteredLogEntries),
                                 [&](auto const& v) { return currentFilter(*v); });
        }

        auto createFilter(FilterState const& filterState) {
            return [filterState](GuiLogEntry const& entry) {
                if(!filterState.enabledLogLevels.empty()) {
                    if(!filterState.enabledLogLevels.contains(entry.logEntry.logLevel)) {
                        return false;
                    }
                }
                if(!filterState.enabledChannels.empty()) {
                    if(!filterState.enabledChannels.contains(entry.logEntry.channel.channel)) {
                        return false;
                    }
                }
                if(!filterState.enabledLocations.empty()) {
                    if(!filterState.enabledLocations.contains(
                         SourceLocation{entry.logEntry.fileName, entry.logEntry.line}))
                    {
                        if(!filterState.enabledLocations.contains(
                             SourceLocation{entry.logEntry.fileName, 0}))
                        {
                            return false;
                        }
                    }
                }

                return true;
            };
        }

        void updateCurrentFilter() {
            if(activeFilterState == editedFilterState) {
                return;
            }

            activeFilterState = editedFilterState;

            if(activeFilterState == FilterState{}) {
                currentFilter = NoFilter;
            } else {
                currentFilter = createFilter(activeFilterState);
            }
            updateFilteredLogEntries();
        }

        ftxui::Component getLogComponent() {
            return Scroller(
              [&]() -> std::vector<std::shared_ptr<GuiLogEntry const>> const& {
                  return filteredLogEntries;
              },
              [&](auto const& e) { return defaultRender(*e); });
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
                           [&](auto const& e) { return renderMessage(e); })})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("💬 Status Messages") | ftxui::bold
                                             | ftxui::color(Theme::Header::secondary())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })});
        }

        ftxui::Element renderBuildEntry(BuildEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(entry.time))
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

            switch(buildStatus) {
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
            auto dataProvider
              = [this](MetricInfo const& metric) -> std::optional<std::vector<MetricEntry> const*> {
                auto it = metricEntries.find(metric);
                if(it != metricEntries.end() && !it->second.empty()) {
                    return &it->second;
                }
                return std::nullopt;
            };

            auto clearCallback = [this]() {
                if(auto selectedMetric = metricPlotWidget.getSelectedMetric()) {
                    auto it = metricEntries.find(*selectedMetric);
                    if(it != metricEntries.end()) {
                        it->second.clear();
                    }
                }
            };

            return metricPlotWidget.createComponent(dataProvider, clearCallback);
        }

        ftxui::Component getBuildComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Output",
              [this]() { buildOutput.clear(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto stopButton = ftxui::Button(
              "⏸ Stop Build",
              [this]() { cancelBuild(); },
              createButtonStyle(Theme::Button::Background::danger(), Theme::Button::text()));

            auto buildButton = ftxui::Button(
              "🔨 Start Build [b]",
              [this]() { executeBuild(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto outputScroller
              = Scroller([&]() -> std::vector<BuildEntry> const& { return buildOutput; },
                         [&](auto const& e) { return renderBuildEntry(e); });

            auto statusDisplay
              = ftxui::Container::Vertical({outputScroller | ftxui::flex})
              | ftxui::Renderer([this](ftxui::Element inner) {
                    return ftxui::vbox(
                      {ftxui::text("🔨 Build Status") | ftxui::bold
                         | ftxui::color(Theme::Header::primary()) | ftxui::center,
                       ftxui::separator(),
                       ftxui::hbox({ftxui::text("Status: ") | ftxui::bold, buildStatusToElement()}),
                       ftxui::hbox({ftxui::text("Output Lines: ") | ftxui::bold,
                                    ftxui::text(fmt::format("{}", buildOutput.size()))
                                      | ftxui::color(Theme::Status::info())}),
                       ftxui::separator(),
                       inner});
                });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal(
                 {buildButton | ftxui::flex, stopButton | ftxui::flex, clearButton | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               statusDisplay | ftxui::flex});
        }

        ftxui::Component getMetricOverviewComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Metrics",
              [this]() {
                  metricEntries.clear();
                  metricPlotWidget.setSelectedMetric(std::nullopt);
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            std::vector<ftxui::Component> components;
            components.push_back(clearButton);
            components.push_back(ftxui::Renderer([]() { return ftxui::separator(); }));

            components.push_back(ftxui::Renderer([this]() {
                return ftxui::text(fmt::format("📈 Metrics ({} entries)", metricEntries.size()))
                     | ftxui::bold | ftxui::color(Theme::Header::primary()) | ftxui::center;
            }));
            components.push_back(ftxui::Renderer([]() { return ftxui::separator(); }));

            auto metricsContainer = ftxui::Container::Vertical({});

            auto dynamicMetricsList
              = metricsContainer
              | ftxui::Renderer([this, metricsContainer](ftxui::Element) mutable {
                    auto currentSelected = metricPlotWidget.getSelectedMetric();
                    bool needsRebuild    = (metricEntries.size() != lastMetricCount)
                                     || (!hasLastSelectedInfo && currentSelected.has_value())
                                     || (hasLastSelectedInfo && !currentSelected.has_value())
                                     || (hasLastSelectedInfo && currentSelected.has_value()
                                         && *currentSelected != lastSelectedInfo);

                    if(needsRebuild) {
                        metricsContainer->DetachAllChildren();

                        if(metricEntries.empty()) {
                            metricsContainer->Add(ftxui::Renderer([]() {
                                return ftxui::text("No metrics available")
                                     | ftxui::color(Theme::Status::inactive()) | ftxui::center;
                            }));
                        } else {
                            for(auto const& [metricInfo, metricValues] : metricEntries) {
                                bool isSelected
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
                                       auto it = metricEntries.find(metricInfo);
                                       if(it == metricEntries.end()) {
                                           return ftxui::text("Metric not found")
                                                | ftxui::color(Theme::Status::error());
                                       }

                                       auto const& currentValues = it->second;
                                       double      latestValue
                                         = currentValues.empty() ? 0.0 : currentValues.back().value;

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
                                                 ftxui::text(fmt::format(" ({} values)",
                                                                         currentValues.size()))
                                                   | ftxui::color(Theme::Data::count())})
                                            | ftxui::flex;
                                   }) | ftxui::flex,
                                   selectButton});

                                metricsContainer->Add(metricRow);
                            }
                        }

                        lastMetricCount = metricEntries.size();
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
                  std::string{magic_enum::enum_name(level)},
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
            auto addEntry = [this](SourceLocation const& sc) {
                if(!editedFilterState.enabledLocations.contains(sc)) {
                    editedFilterState.enabledLocations.insert(sc);
                    updateCurrentFilter();
                }
            };

            auto stringToSourceLocation
              = [](std::string const& x) -> std::optional<SourceLocation> {
                auto colonPosition = std::find(x.begin(), x.end(), ':');
                if(colonPosition == x.end()) {
                    if(x.empty()) {
                        return std::nullopt;
                    }
                    return SourceLocation{x, 0};
                }
                std::size_t line;
                auto parseResult = std::from_chars(&(*(colonPosition + 1)), &(*x.end()), line);
                if(parseResult.ec == std::errc{} && parseResult.ptr == &(*x.end())) {
                    return SourceLocation{
                      std::string_view{x.begin(), colonPosition},
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
            manualInputComponents.push_back(ftxui::Input(&locationFilterInput, "filename:line")
                                            | ftxui::flex);
            manualInputComponents.push_back(ftxui::Maybe(
              ftxui::Button(
                "+ Add",
                [this, addEntry, stringToSourceLocation]() {
                    auto sc = stringToSourceLocation(locationFilterInput);
                    if(sc) {
                        addEntry(*sc);
                        locationFilterInput.clear();
                    }
                },
                createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text())),
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
              = std::make_unique<SourceLocationAdapter>(allSourceLocations);
            dropdownOptions.radiobox.selected  = &selectedLocationIndex;
            dropdownOptions.radiobox.on_change = [this]() {
                auto it = allSourceLocations.begin();
                auto i  = selectedLocationIndex;
                while(i != 0) {
                    --i;
                    ++it;
                }
                selectedSourceLocation = it->first;
            };

            dropdownComponents.push_back(ftxui::Dropdown(dropdownOptions));
            dropdownComponents.push_back(ftxui::Maybe(
              ftxui::Button(
                "+ Add",
                [addEntry, this]() { addEntry(selectedSourceLocation); },
                createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text())),
              [this]() { return !selectedSourceLocation.first.empty(); }));

            auto dropdownComponent = ftxui::Container::Horizontal(dropdownComponents);

            std::vector<ftxui::Component> inputSectionComponents;
            inputSectionComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📍 Add Location Filter") | ftxui::bold
                     | ftxui::color(Theme::Header::accent()) | ftxui::center;
            }));
            inputSectionComponents.push_back(manualInputComponent);
            inputSectionComponents.push_back(dropdownComponent);

            auto inputComponent
              = ftxui::Container::Vertical(inputSectionComponents) | ftxui::border;

            std::vector<ftxui::Component> location_components{};

            location_components.push_back(ftxui::Renderer([this]() {
                return ftxui::text(fmt::format("📂 Active Filters ({})",
                                               editedFilterState.enabledLocations.size()))
                     | ftxui::bold | ftxui::color(Theme::Status::info());
            }));

            RadioboxOption radioboxOption = RadioboxOption::Simple();

            radioboxOption.transform = [](ftxui::EntryState const& s) {
                auto t = ftxui::text(s.label);
                if(s.active) {
                    t |= ftxui::bold;
                }
                if(s.focused) {
                    t |= ftxui::inverted;
                }
                return ftxui::hbox({ftxui::text("❌ ") | ftxui::color(Theme::UI::remove()), t});
            };

            radioboxOption.entries
              = std::make_unique<EnabledLocationAdapter>(editedFilterState.enabledLocations);
            radioboxOption.on_click = [this](int i) {
                auto it = editedFilterState.enabledLocations.begin();
                while(i != 0) {
                    --i;
                    ++it;
                }
                editedFilterState.enabledLocations.erase(it);
            };

            location_components.push_back(Radiobox(radioboxOption) | ftxui::frame | ftxui::border
                                          | ftxui::center);
            auto locationsContainer
              = ftxui::Container::Vertical(location_components) | ftxui::border;

            std::vector<ftxui::Component> finalComponents;
            finalComponents.push_back(inputComponent);
            finalComponents.push_back(locationsContainer);

            return ftxui::Container::Vertical(finalComponents);
        }

        ftxui::Component getFilterComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear All Filters",
              [this]() {
                  editedFilterState = FilterState{};
                  updateCurrentFilter();
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

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

            return ftxui::Container::Vertical(
              {clearButton,
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Vertical(mainComponents)
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🔍 Filter Settings") | ftxui::bold
                                             | ftxui::color(Theme::Header::primary())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })});
        }

        ftxui::Component getAppearanceSettingsComponent() {
            auto resetButton = ftxui::Button(
              "🔄 Reset to Defaults",
              [this]() {
                  showSysTime      = true;
                  showFunctionName = false;
                  showUcTime       = true;
                  showLocation     = true;
                  showChannel      = true;
                  showLogLevel     = true;
                  showMetricString = false;
              },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto clearButton = ftxui::Button(
              "❌ Clear log entries",
              [this]() {
                  allLogEntries.clear();
                  filteredLogEntries.clear();
              },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetButton | ftxui::flex, clearButton | ftxui::flex}),
               ftxui::Renderer([]() { return ftxui::separator(); }),
               ftxui::Container::Vertical(
                 {ftxui::Checkbox("⏰ System Time", &showSysTime),
                  ftxui::Checkbox("🔍 Function Names", &showFunctionName),
                  ftxui::Checkbox("🕐 Target Time", &showUcTime),
                  ftxui::Checkbox("📍 Source Location", &showLocation),
                  ftxui::Checkbox("📡 Log Channel", &showChannel),
                  ftxui::Checkbox("📊 Log Level", &showLogLevel),
                  ftxui::Checkbox("📊 Show Metric Strings", &showMetricString)})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🎨 Display Settings") | ftxui::bold
                                             | ftxui::color(Theme::Header::accent())
                                             | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })});
        }

        template<typename Reader>
        ftxui::Component getDebuggerComponent(Reader& rttReader) {
            auto resetTargetBtn = ftxui::Button(
              "🔄 Reset Target [r]",
              [&rttReader]() { rttReader.resetTarget(); },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto resetDebuggerBtn = ftxui::Button(
              "🔌 Reset Debugger",
              [&rttReader]() { rttReader.resetJLink(); },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            auto flashBtn = ftxui::Button(
              "⚡ Flash Target [f]",
              [&rttReader]() { rttReader.flash(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

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

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetTargetBtn | ftxui::flex,
                                             resetDebuggerBtn | ftxui::flex,
                                             flashBtn | ftxui::flex}),
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

            auto toggle = ftxui::Toggle(std::move(tab_values), &selectedTab) | ftxui::bold;

            ftxui::Components vertical_components{
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

            ftxui::Components vertical_components{
              toggle,
              ftxui::Renderer([]() { return ftxui::separator(); }),
              ftxui::Container::Tab(std::move(tab_components), &selectedMetricTab) | ftxui::flex};

            return ftxui::Container::Vertical(vertical_components);
        }

        template<typename Reader>
        ftxui::Component getStatusLineComponent(Reader& rttReader) {
            auto quitBtn = ftxui::Button(
              "[q]uit",
              [this]() { screenPointer->Exit(); },
              createButtonStyle(Theme::Button::Background::destructive(), Theme::Button::text()));

            auto resetBtn = ftxui::Button(
              "[r]eset",
              [&rttReader]() { rttReader.resetTarget(); },
              createButtonStyle(Theme::Button::Background::reset(), Theme::Button::text()));

            auto flashBtn = ftxui::Button(
              "[f]lash",
              [&rttReader]() { rttReader.flash(); },
              createButtonStyle(Theme::Button::Background::positive(), Theme::Button::text()));

            auto buildBtn = ftxui::Button(
              "[b]uild",
              [this]() { executeBuild(); },
              createButtonStyle(Theme::Button::Background::build(), Theme::Button::text()));

            auto statusRenderer = ftxui::Renderer([&rttReader, this]() {
                auto const rttStatus    = rttReader.getStatus();
                auto const logCount     = filteredLogEntries.size();
                auto const totalCount   = allLogEntries.size();
                bool const filterActive = activeFilterState != FilterState{};
                bool const buildRunning = (buildStatus == BuildStatus::Running);
                bool const buildSuccess = (buildStatus == BuildStatus::Success);
                bool const isFlashing   = rttReader.isFlashing();

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
                                              || buildStatus == BuildStatus::Failed)
                                               ? "●"
                                               : "○"))
                     | ftxui::color(buildRunning
                                      ? Theme::Status::warning()
                                      : (buildSuccess ? Theme::Status::success()
                                                      : (buildStatus == BuildStatus::Failed
                                                           ? Theme::Status::error()
                                                           : Theme::Text::normal()))),
                   ftxui::separator(),

                   ftxui::text(isFlashing ? (rttStatus.isRunning == 0 ? "⚡ ●" : "⚡ ●") : "⚡ ●")
                     | ftxui::color(isFlashing
                                      ? (rttStatus.isRunning == 0 ? Theme::Status::error()
                                                                  : Theme::Status::warning())
                                      : Theme::Status::success())
                     | ftxui::bold,
                   ftxui::separator(),

                   ftxui::text(
                     fmt::format("LOGS {}/{}",
                                 FTXUIGui::formatNumber(static_cast<std::uint32_t>(logCount)),
                                 FTXUIGui::formatNumber(static_cast<std::uint32_t>(totalCount))))
                     | ftxui::color(Theme::Status::info()),
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
                   totalCount >= GUI_Constants::MaxLogEntries ? ftxui::separator()
                                                              : ftxui::text(""),
                   totalCount >= GUI_Constants::MaxLogEntries
                     ? ftxui::text("⚠️ MEM") | ftxui::color(ftxui::Color::Red) | ftxui::bold
                     : ftxui::text(""),
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
              {   "📄 Logs",                getLogComponent()},
              { "🔍 Filter",             getFilterComponent()},
              {"🎨 Display", getAppearanceSettingsComponent()},
              {  "🔧 Debug",  getDebuggerComponent(rttReader)},
              {  "🔨 Build",              getBuildComponent()},
              {"📈 Metrics",             getMetricComponent()},
              { "💬 Status",             getStatusComponent()}
            });

            return ftxui::Container::Vertical({getStatusLineComponent(rttReader),
                                               ftxui::Renderer([]() { return ftxui::separator(); }),
                                               tabs | ftxui::flex})
                 | ftxui::border;
        }

    public:
        void add(std::chrono::system_clock::time_point recv_time,
                 uc_log::detail::LogEntry const&       e) {
            {
                std::lock_guard<std::mutex> lock{mutex};
                auto const                  metrics = uc_log::extractMetrics(recv_time, e);

                for(auto const& metric : metrics) {
                    metricEntries[metric.first].push_back(metric.second);
                }

                auto entry = std::make_shared<GuiLogEntry const>(recv_time, e);
                allLogEntries.push_back(entry);
                allSourceLocations[SourceLocation{e.fileName, e.line}]++;
                if(currentFilter(*entry)) {
                    filteredLogEntries.push_back(entry);
                }
                if(screenPointer) {
                    screenPointer->PostEvent(ftxui::Event::Custom);
                }
            }
        }

        void fatalError(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Fatal,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        void statusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Status,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        void errorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::Error,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        void toolStatusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::ToolStatus,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        void toolErrorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            statusMessages.emplace_back(MessageEntry::Level::ToolError,
                                        std::chrono::system_clock::now(),
                                        std::string{msg});
            if(screenPointer) {
                screenPointer->PostEvent(ftxui::Event::Custom);
            }
        }

        template<typename Reader>
        int run(Reader&            rttReader,
                std::string const& buildCommand) {
            initializeBuildCommand(buildCommand);

            auto screen = ftxui::ScreenInteractive::Fullscreen();
            screen.ForceHandleCtrlC(true);
            ftxui::Component mainComponent;
            {
                std::lock_guard<std::mutex> lock{mutex};

                mainComponent
                  = ftxui::CatchEvent(getTabComponent(rttReader), [&](ftxui::Event event) {
                        if(selectedTab == 1 && event.is_character()) {
                            return false;
                        }

                        if(event == ftxui::Event::Character('r')) {
                            rttReader.resetTarget();
                            return true;
                        }
                        if(event == ftxui::Event::Character('f')) {
                            rttReader.flash();
                            return true;
                        }
                        if(event == ftxui::Event::Character('b')) {
                            executeBuild();
                            return true;
                        }
                        if(event == ftxui::Event::Character('q')) {
                            screenPointer->Exit();
                            return true;
                        }

                        return false;
                    });
            }
            ftxui::Loop loop(&screen, mainComponent);

            while(!loop.HasQuitted()) {
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    loop.RunOnce();
                    if(screenPointer == nullptr) {
                        screenPointer = &screen;
                    }
                }
                std::this_thread::sleep_for(GUI_Constants::UpdateInterval);
                if(callJoin == true) {
                    if(buildThread.joinable()) {
                        buildThread.join();
                    }
                    callJoin = false;
                }
            }
            {
                std::lock_guard<std::mutex> lock{mutex};
                screenPointer = nullptr;
            }

            return 0;
        }
    };
}}   // namespace uc_log::FTXUIGui
