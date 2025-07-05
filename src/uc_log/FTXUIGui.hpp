#pragma once

#include "uc_log/FTXUI_Utils.hpp"
#include "uc_log/detail/LogEntry.hpp"

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
#endif

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/v1/search_path.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif

#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <chrono>
#include <csignal>
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

    struct FTXUIGui {
        struct GuiLogEntry {
            std::chrono::system_clock::time_point recv_time;
            uc_log::detail::LogEntry              logEntry;
        };

        FTXUIGui()                           = default;
        FTXUIGui(FTXUIGui const&)            = delete;
        FTXUIGui& operator=(FTXUIGui const&) = delete;

        FTXUIGui(FTXUIGui&&)            = delete;
        FTXUIGui& operator=(FTXUIGui&&) = delete;

        ~FTXUIGui() {
            if(buildIoContext) {
                buildIoContext->stop();
            }
            if(buildThread.joinable()) {
                buildThread.join();
            }
        }

        struct FilterState {
            std::set<uc_log::LogLevel> enabledLogLevels;
            std::set<std::size_t>      enabledChannels;
            std::set<SourceLocation>   enabledLocations;

            bool operator==(FilterState const&) const = default;
        };

        std::mutex mutex;

        ftxui::ScreenInteractive* screen_ptr = nullptr;

        std::map<SourceLocation, std::size_t>           allSourceLocations;
        std::vector<std::shared_ptr<GuiLogEntry const>> allLogEntries;
        std::vector<std::shared_ptr<GuiLogEntry const>> filteredLogEntries;

        FilterState activeFilterState;
        FilterState editedFilterState;

        static constexpr auto NoFilter = [](GuiLogEntry const&) { return true; };

        std::function<bool(GuiLogEntry const&)> currentFilter = NoFilter;

        bool showSysTime{true};
        bool showFunctionName{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showChannel{true};
        bool showLogLevel{true};

        struct MessageEntry {
            enum class Level { Fatal, Error, Status };

            Level                                 level;
            std::chrono::system_clock::time_point time;
            std::string                           message;
        };

        std::vector<MessageEntry> messages;

        struct BuildEntry {
            std::chrono::system_clock::time_point time;
            std::string                           line;
            bool                                  fromTool;
            bool                                  isError;
        };

        enum class BuildStatus { Idle, Running, Success, Failed };

        std::vector<BuildEntry> buildOutput;
        BuildStatus             buildStatus = BuildStatus::Idle;

        std::vector<std::string> parsedBuildArgs;
        boost::filesystem::path  buildExecutablePath;

        std::string                                  stdoutBuffer;
        std::string                                  stderrBuffer;
        std::jthread                                 buildThread;
        std::unique_ptr<boost::asio::io_context>     buildIoContext{};
        std::unique_ptr<boost::asio::readable_pipe>  stdoutPipe{};
        std::unique_ptr<boost::asio::readable_pipe>  stderrPipe{};
        std::unique_ptr<boost::process::v2::process> buildProcess{};

        void addBuildOutput(std::string const& line,
                            bool               fromTool,
                            bool               isError) {
            std::lock_guard<std::mutex> lock{mutex};
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, fromTool, isError);
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void addBuildOutputGui(std::string const& line,
                               bool               isError) {
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, false, isError);
        }

        void startAsyncReadStdout() {
            boost::asio::async_read_until(
              *stdoutPipe,
              boost::asio::dynamic_buffer(stdoutBuffer),
              '\n',
              [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                  if(!ec && bytes_transferred > 0) {
                      auto pos = stdoutBuffer.find('\n');
                      if(pos != std::string::npos) {
                          std::string line = stdoutBuffer.substr(0, pos);
                          stdoutBuffer.erase(0, pos + 1);
                          addBuildOutput(line, true, false);
                      }
                      startAsyncReadStdout();
                  }
              });
        }

        void startAsyncReadStderr() {
            boost::asio::async_read_until(
              *stderrPipe,
              boost::asio::dynamic_buffer(stderrBuffer),
              '\n',
              [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                  if(!ec && bytes_transferred > 0) {
                      auto pos = stderrBuffer.find('\n');
                      if(pos != std::string::npos) {
                          std::string line = stderrBuffer.substr(0, pos);
                          stderrBuffer.erase(0, pos + 1);
                          addBuildOutput(line, true, true);
                      }
                      startAsyncReadStderr();
                  }
              });
        }

    private:
        bool hasScript() const {
            try {
                auto scriptPath = boost::process::v1::search_path("script");
                return !scriptPath.empty();
            } catch(...) {
                return false;
            }
        }

        std::pair<boost::filesystem::path,
                  std::vector<std::string>>
        createColoredBuildCommand() const {
            if(hasScript()) {
                auto scriptPath = boost::process::v1::search_path("script");
                if(!scriptPath.empty()) {
                    std::vector<std::string> wrappedArgs;
                    wrappedArgs.push_back("-q");
                    wrappedArgs.push_back("-c");

                    std::string commandStr = buildExecutablePath.string();
                    for(auto const& arg : parsedBuildArgs) {
                        commandStr += " ";
                        if(arg.find(' ') != std::string::npos) {
                            commandStr += "\"" + arg + "\"";
                        } else {
                            commandStr += arg;
                        }
                    }
                    wrappedArgs.push_back(commandStr);
                    wrappedArgs.push_back("/dev/null");

                    return {scriptPath, wrappedArgs};
                }
            }

            return {buildExecutablePath, parsedBuildArgs};
        }

        bool initializeBuildCommand(std::string const& buildCommandStr) {
            std::vector<std::string> args;
            std::string              currentArgument;
            bool                     in_quotes   = false;
            bool                     escape_next = false;

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
                        args.push_back(currentArgument);
                        currentArgument.clear();
                    }
                } else {
                    currentArgument += c;
                }
            }

            if(!currentArgument.empty()) {
                args.push_back(currentArgument);
            }

            if(args.empty()) {
                return false;
            }

            std::string executable = args[0];
            args.erase(args.begin());

            auto const ex = boost::process::v1::search_path(executable);
            if(ex.empty()) {
                return false;
            }

            parsedBuildArgs     = std::move(args);
            buildExecutablePath = ex;

            return true;
        }

    public:
        void executeAsyncBuild() {
            if(buildStatus == BuildStatus::Running) {
                return;
            }

            if(buildIoContext) {
                buildIoContext->stop();
            }

            buildOutput.clear();
            buildStatus = BuildStatus::Running;

            if(buildThread.joinable()) {
                buildThread.join();
            }

            try {
                stdoutPipe.reset();
                stderrPipe.reset();
                buildProcess.reset();

                buildIoContext = std::make_unique<boost::asio::io_context>();

                addBuildOutputGui(fmt::format("🚀 Starting process: {} {}",
                                              buildExecutablePath.string(),
                                              parsedBuildArgs),
                                  false);

                buildThread = std::jthread{[this]() {
                    try {
                        stdoutPipe = std::make_unique<boost::asio::readable_pipe>(*buildIoContext);
                        stderrPipe = std::make_unique<boost::asio::readable_pipe>(*buildIoContext);

                        auto [execPath, execArgs] = createColoredBuildCommand();
                        std::vector<std::string> envVars;

                        auto currentEnv = boost::this_process::environment();
                        for(auto const& envVar : currentEnv) {
                            envVars.push_back(envVar.get_name() + "=" + envVar.to_string());
                        }
                        envVars.push_back("FORCE_COLOR=1");
                        envVars.push_back("CLICOLOR_FORCE=1");
                        envVars.push_back("COLORTERM=truecolor");
                        envVars.push_back("CMAKE_COLOR_DIAGNOSTICS=ON");
                        envVars.push_back("NINJA_STATUS=[%f/%t] ");

                        buildProcess = std::make_unique<boost::process::v2::process>(
                          *buildIoContext,
                          execPath,
                          execArgs,
                          boost::process::v2::process_stdio{nullptr, *stdoutPipe, *stderrPipe},
                          boost::process::v2::process_environment{envVars});

                        startAsyncReadStdout();
                        startAsyncReadStderr();

                        buildProcess->async_wait([this](boost::system::error_code ec, int exitCode) {
                            {
                                std::lock_guard<std::mutex> lock{mutex};
                                buildStatus
                                  = (exitCode == 0) ? BuildStatus::Success : BuildStatus::Failed;
                            }

                            if(ec) {
                                addBuildOutput(fmt::format("❌ Process error: {}", ec.message()),
                                               false,
                                               true);
                            } else {
                                addBuildOutput(fmt::format("🏁 Build {} (exit code: {})",
                                                           exitCode == 0 ? "succeeded" : "failed",
                                                           exitCode),
                                               false,
                                               exitCode != 0);
                            }
                        });
                        buildIoContext->run();

                        addBuildOutputGui(fmt::format("foo"), true);

                        if(buildStatus == BuildStatus::Running){
                            addBuildOutputGui(fmt::format("bar"), true);
                            buildProcess->terminate();
                        }else if(buildStatus == BuildStatus::Success){
                            addBuildOutputGui(fmt::format("foobar"), true);
                        }else if(buildStatus == BuildStatus::Failed){
                            addBuildOutputGui(fmt::format("barfoo"), true);
                        }
                    } catch (boost::process::system_error const& e) {
                        addBuildOutputGui(fmt::format("System error: {}", e.what()), true);
                    }
                }};

            } catch(boost::system::system_error const& e) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui(fmt::format("❌ System error: {}", e.what()), true);
            } catch(std::exception const& e) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui(fmt::format("❌ Build error: {}", e.what()), true);
            } catch(...) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui("❌ Unknown error occurred during build", true);
            }
        }

        void executeBuild() { executeAsyncBuild(); }

        auto defaultRender(GuiLogEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(12);

            if(showSysTime) {
                elements.push_back(
                  ftxui::text(detail::to_time_string_with_milliseconds(e.recv_time))
                  | ftxui::color(ftxui::Color::Cyan));
                elements.push_back(ftxui::text(" "));
            }

            if(showChannel) {
                elements.push_back(toElement(e.logEntry.channel));
                elements.push_back(ftxui::text(" "));
            }

            if(showUcTime) {
                elements.push_back(ftxui::text(fmt::format("{}", e.logEntry.ucTime))
                                   | ftxui::color(ftxui::Color::Magenta));
                elements.push_back(ftxui::text(" "));
            }

            if(showLogLevel) {
                elements.push_back(toElement(e.logEntry.logLevel));
                elements.push_back(ftxui::text("│ ") | ftxui::color(ftxui::Color::GrayDark));
            }

            elements.push_back(ansiColoredTextToFtxui(e.logEntry.logMsg));

            auto scrollableContent = ftxui::hbox(elements) | ftxui::flex;

            ftxui::Elements metadata;
            if(showFunctionName) {
                metadata.push_back(ftxui::text(e.logEntry.functionName)
                                   | ftxui::color(ftxui::Color::Blue));
            }

            if(showLocation) {
                if(!metadata.empty()) {
                    metadata.push_back(ftxui::text(" "));
                }
                metadata.push_back(
                  ftxui::text(fmt::format("{}:{}", e.logEntry.fileName, e.logEntry.line))
                  | ftxui::color(ftxui::Color::GrayDark));
            }

            ftxui::Element metadataElement = nullptr;
            if(!metadata.empty()) {
                metadataElement = ftxui::hbox(metadata);
            }

            return std::make_shared<ScrollableWithMetadata>(std::move(scrollableContent),
                                                            std::move(metadataElement));
        }

        ftxui::Element renderMessage(MessageEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(e.time))
                               | ftxui::color(ftxui::Color::Cyan));

            elements.push_back(ftxui::text(" | ") | ftxui::color(ftxui::Color::Default));

            auto messageColor
              = e.level == MessageEntry::Level::Fatal ? ftxui::color(ftxui::Color::Red)
              : e.level == MessageEntry::Level::Error ? ftxui::color(ftxui::Color::Magenta)
                                                      : ftxui::color(ftxui::Color::Green);

            elements.push_back(ftxui::text(e.message) | messageColor | ftxui::flex);

            return ftxui::hbox(elements);
        }

        void updateFilteredLogEntries() {
            filteredLogEntries.clear();
            for(auto& entry : allLogEntries) {
                if(currentFilter(*entry)) {
                    filteredLogEntries.push_back(entry);
                }
            }
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

        void add(std::chrono::system_clock::time_point recv_time,
                 uc_log::detail::LogEntry const&       e) {
            {
                std::lock_guard<std::mutex> lock{mutex};
                auto entry = std::make_shared<GuiLogEntry const>(recv_time, e);
                allLogEntries.push_back(entry);
                allSourceLocations[SourceLocation{e.fileName, e.line}]++;
                if(currentFilter(*entry)) {
                    filteredLogEntries.push_back(entry);
                }
            }
            {
                std::lock_guard<std::mutex> lock{mutex};
                if(screen_ptr) {
                    screen_ptr->PostEvent(ftxui::Event::Custom);
                }
            }
        }

        void fatalError(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Fatal,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void statusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Status,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void errorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Error,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
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
              [this]() { messages.clear(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            return ftxui::Container::Vertical(
              {clearButton,
               Scroller([&]() -> std::vector<MessageEntry> const& { return messages; },
                        [&](auto const& e) { return renderMessage(e); })});
        }

        ftxui::Element renderBuildEntry(BuildEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(entry.time))
                               | ftxui::color(ftxui::Color::Cyan));

            elements.push_back(ftxui::text(" | ") | ftxui::color(ftxui::Color::Default));

            if(entry.fromTool) {
                elements.push_back(ansiColoredTextToFtxui(entry.line) | ftxui::flex);
            } else {
                auto lineColor = entry.isError ? ftxui::color(ftxui::Color::Red)
                                               : ftxui::color(ftxui::Color::Default);
                elements.push_back(ftxui::text(entry.line) | lineColor | ftxui::flex);
            }
            return ftxui::hbox(elements);
        }

        ftxui::Component getBuildComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Output",
              [this]() { buildOutput.clear(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            auto stopButton = ftxui::Button(
              "⏸ Stop Build",
              [this]() {
                  try {
                      if(buildIoContext) {
                          boost::asio::post(*buildIoContext, [this]() {
                              if(buildProcess->running()) {
                                  buildProcess->terminate();
                                  addBuildOutputGui("Stopped build process...", false);
                              }
                              buildIoContext->stop();
                          });
                      }

                      if(buildThread.joinable()) {
                          buildThread.join();
                      }
                      buildStatus = BuildStatus::Failed;
                  } catch(std::exception const& e) {
                      addBuildOutputGui(fmt::format("❌ Error stopping build: {}", e.what()), true);
                  }
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            auto buildButton = ftxui::Button(
              "🔨 Start Build [b]",
              [this]() { executeAsyncBuild(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Green));

            auto statusDisplay = ftxui::Renderer([this]() {
                std::string  statusText;
                ftxui::Color statusColor;

                switch(buildStatus) {
                case BuildStatus::Idle:
                    statusText  = "⚪ Idle";
                    statusColor = ftxui::Color::GrayDark;
                    break;
                case BuildStatus::Running:
                    statusText  = "🟡 Building...";
                    statusColor = ftxui::Color::Yellow;
                    break;
                case BuildStatus::Success:
                    statusText  = "✅ Success";
                    statusColor = ftxui::Color::Green;
                    break;
                case BuildStatus::Failed:
                    statusText  = "❌ Failed";
                    statusColor = ftxui::Color::Red;
                    break;
                }

                return ftxui::vbox({ftxui::text("🔨 Build Status") | ftxui::bold
                                      | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                                    ftxui::separator(),
                                    ftxui::hbox({ftxui::text("Status: ") | ftxui::bold,
                                                 ftxui::text(statusText) | ftxui::color(statusColor)
                                                   | ftxui::bold}),
                                    ftxui::hbox({ftxui::text("Output Lines: ") | ftxui::bold,
                                                 ftxui::text(fmt::format("{}", buildOutput.size()))
                                                   | ftxui::color(ftxui::Color::Cyan)})})
                     | ftxui::borderRounded;
            });

            auto outputScroller
              = Scroller([&]() -> std::vector<BuildEntry> const& { return buildOutput; },
                         [&](auto const& e) { return renderBuildEntry(e); });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal(
                 {buildButton | ftxui::flex, stopButton | ftxui::flex, clearButton | ftxui::flex}),
               statusDisplay,
               outputScroller | ftxui::flex});
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
              ftxui::ButtonOption::Animated(ftxui::Color::Cyan));

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
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

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

        int            selectedLocationIndex{};
        SourceLocation selectedSourceLocation;
        std::string    locationFilterInput;

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
                     | ftxui::color(ftxui::Color::Magenta);
            }));
            manualInputComponents.push_back(ftxui::Input(&locationFilterInput, "filename:line")
                                            | ftxui::flex);
            manualInputComponents.push_back(
              ftxui::Maybe(ftxui::Button(
                             "+ Add",
                             [this, addEntry, stringToSourceLocation]() {
                                 auto sc = stringToSourceLocation(locationFilterInput);
                                 if(sc) {
                                     addEntry(*sc);
                                     locationFilterInput.clear();
                                 }
                             },
                             ftxui::ButtonOption::Animated(ftxui::Color::Green)),
                           [this, stringToSourceLocation]() {
                               return stringToSourceLocation(locationFilterInput).has_value();
                           }));

            auto manualInputComponent = ftxui::Container::Horizontal(manualInputComponents);

            std::vector<ftxui::Component> dropdownComponents;
            dropdownComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📋 Known:") | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
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
            dropdownComponents.push_back(
              ftxui::Maybe(ftxui::Button(
                             "+ Add",
                             [addEntry, this]() { addEntry(selectedSourceLocation); },
                             ftxui::ButtonOption::Animated(ftxui::Color::Green)),
                           [this]() { return !selectedSourceLocation.first.empty(); }));

            auto dropdownComponent = ftxui::Container::Horizontal(dropdownComponents);

            std::vector<ftxui::Component> inputSectionComponents;
            inputSectionComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📍 Add Location Filter") | ftxui::bold
                     | ftxui::color(ftxui::Color::Magenta) | ftxui::center;
            }));
            inputSectionComponents.push_back(manualInputComponent);
            inputSectionComponents.push_back(dropdownComponent);

            auto inputComponent
              = ftxui::Container::Vertical(inputSectionComponents) | ftxui::border;

            std::vector<ftxui::Component> location_components{};

            location_components.push_back(ftxui::Renderer([this]() {
                return ftxui::text(fmt::format("📂 Active Filters ({})",
                                               editedFilterState.enabledLocations.size()))
                     | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
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
                return ftxui::hbox({ftxui::text("❌ ") | ftxui::color(ftxui::Color::Red), t});
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
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            std::vector<ftxui::Component> mainComponents;

            std::vector<ftxui::Component> levelComponents;
            levelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📊 Log Levels") | ftxui::bold | ftxui::color(ftxui::Color::Cyan)
                     | ftxui::center;
            }));
            levelComponents.push_back(getLogLevelFilterComponent());

            std::vector<ftxui::Component> channelComponents;
            channelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📡 Channels") | ftxui::bold | ftxui::color(ftxui::Color::Yellow)
                     | ftxui::center;
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
               ftxui::Container::Vertical(mainComponents)
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🔍 Filters") | ftxui::bold
                                             | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })
                 | ftxui::borderRounded});
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
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Blue));

            auto clearButton = ftxui::Button(
              "❌ Clear log entries",
              [this]() {
                  allLogEntries.clear();
                  filteredLogEntries.clear();
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetButton | ftxui::flex, clearButton | ftxui::flex}),
               ftxui::Container::Vertical({ftxui::Checkbox("⏰ System Time", &showSysTime),
                                           ftxui::Checkbox("🔍 Function Names", &showFunctionName),
                                           ftxui::Checkbox("🕐 Target Time", &showUcTime),
                                           ftxui::Checkbox("📍 Source Location", &showLocation),
                                           ftxui::Checkbox("📡 Log Channel", &showChannel),
                                           ftxui::Checkbox("📊 Log Level", &showLogLevel)})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🎨 Display Settings") | ftxui::bold
                                             | ftxui::color(ftxui::Color::Magenta) | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })
                 | ftxui::borderRounded});
        }

        template<typename Reader>
        ftxui::Component getDebuggerComponent(Reader& rttReader) {
            auto resetTargetBtn = ftxui::Button(
              "🔄 Reset Target [r]",
              [&rttReader]() { rttReader.resetTarget(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Blue));

            auto resetDebuggerBtn = ftxui::Button(
              "🔌 Reset Debugger [d]",
              [&rttReader]() { rttReader.resetJLink(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Magenta));

            auto flashBtn = ftxui::Button(
              "⚡ Flash Target [f]",
              [&rttReader]() { rttReader.flash(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            auto statusDisplay = ftxui::Renderer([&rttReader]() {
                auto const rttStatus = rttReader.getStatus();

                return ftxui::vbox(
                         {ftxui::text("📊 Debugger Status") | ftxui::bold
                            | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                          ftxui::separator(),

                          ftxui::hbox(
                            {ftxui::text("Connection: ") | ftxui::bold,
                             ftxui::text(rttStatus.isRunning != 0 ? "✓ Active" : "✗ Inactive")
                               | ftxui::color(rttStatus.isRunning != 0 ? ftxui::Color::Green
                                                                       : ftxui::Color::Red)
                               | ftxui::bold}),

                          ftxui::hbox(
                            {ftxui::text("Overflows: ") | ftxui::bold,
                             ftxui::text(fmt::format("{}", rttStatus.hostOverflowCount))
                               | ftxui::color(rttStatus.hostOverflowCount == 0 ? ftxui::Color::Green
                                                                               : ftxui::Color::Red)
                               | ftxui::bold}),

                          ftxui::hbox({ftxui::text("Read: ") | ftxui::bold,
                                       ftxui::text(fmt::format("{} bytes", rttStatus.numBytesRead))
                                         | ftxui::color(ftxui::Color::Cyan)}),

                          ftxui::hbox({ftxui::text("Buffers: ") | ftxui::bold,
                                       ftxui::text(fmt::format("↑{} ↓{}",
                                                               rttStatus.numUpBuffers,
                                                               rttStatus.numDownBuffers))
                                         | ftxui::color(ftxui::Color::Yellow)})})
                     | ftxui::borderRounded;
            });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetTargetBtn | ftxui::flex,
                                             resetDebuggerBtn | ftxui::flex,
                                             flashBtn | ftxui::flex}),
               statusDisplay});
        }

        int tab_selected{};

        ftxui::Component
        generateTabsComponent(std::vector<std::pair<std::string_view,
                                                    ftxui::Component>> const& entries) {
            std::vector<std::string>      tab_values{};
            std::vector<ftxui::Component> tab_components{};

            for(auto const& [name, component] : entries) {
                tab_values.push_back(std::string{name} + " ");
                tab_components.push_back(component | ftxui::border);
            }

            auto toggle
              = ftxui::Toggle(std::move(tab_values), &tab_selected) | ftxui::bold | ftxui::border;

            ftxui::Components vertical_components{
              toggle,
              ftxui::Container::Tab(std::move(tab_components), &tab_selected) | ftxui::flex};

            return ftxui::Container::Vertical(vertical_components);
        }

        template<typename Reader>
        ftxui::Component getTabComponent(Reader& rttReader) {
            auto tabs = generateTabsComponent({
              {   "📄 Logs",                getLogComponent()},
              { "🔍 Filter",             getFilterComponent()},
              {"🎨 Display", getAppearanceSettingsComponent()},
              {  "🔧 Debug",  getDebuggerComponent(rttReader)},
              {  "🔨 Build",              getBuildComponent()},
              { "💬 Status",             getStatusComponent()}
            });

            return ftxui::Container::Vertical(
              {getStatusLineComponent(rttReader), tabs | ftxui::flex});
        }

        template<typename Reader>
        ftxui::Component getStatusLineComponent(Reader& rttReader) {
            return ftxui::Renderer([&rttReader, this]() {
                auto const rttStatus    = rttReader.getStatus();
                auto const logCount     = filteredLogEntries.size();
                auto const totalCount   = allLogEntries.size();
                bool const filterActive = activeFilterState != FilterState{};

                std::string  statusText;
                ftxui::Color statusColor;
                switch(buildStatus) {
                case BuildStatus::Idle:
                    statusText  = "⚪ Idle";
                    statusColor = ftxui::Color::GrayDark;
                    break;
                case BuildStatus::Running:
                    statusText  = "🟡 Building...";
                    statusColor = ftxui::Color::Yellow;
                    break;
                case BuildStatus::Success:
                    statusText  = "✅ Success";
                    statusColor = ftxui::Color::Green;
                    break;
                case BuildStatus::Failed:
                    statusText  = "❌ Failed";
                    statusColor = ftxui::Color::Red;
                    break;
                }

                return ftxui::hbox(
                         {ftxui::text(rttStatus.isRunning != 0 ? "● Connected" : "○ Disconnected")
                            | ftxui::color(rttStatus.isRunning != 0 ? ftxui::Color::Green
                                                                    : ftxui::Color::Red)
                            | ftxui::bold,
                          ftxui::separator(),
                          ftxui::text(fmt::format("Logs: {}/{}", logCount, totalCount))
                            | ftxui::color(ftxui::Color::Cyan),
                          ftxui::separator(),
                          ftxui::text(fmt::format("Overflows: {}", rttStatus.hostOverflowCount))
                            | ftxui::color(rttStatus.hostOverflowCount == 0 ? ftxui::Color::Green
                                                                            : ftxui::Color::Red),
                          ftxui::separator(),
                          ftxui::text(fmt::format("Bytes: {}", rttStatus.numBytesRead))
                            | ftxui::color(ftxui::Color::Yellow),
                          ftxui::separator(),
                          ftxui::text(filterActive ? "✓ Filters Active" : "○ No Filters")
                            | ftxui::color(filterActive ? ftxui::Color::Yellow
                                                        : ftxui::Color::Green),
                          ftxui::separator(),
                          ftxui::text(fmt::format("🔨 {}", statusText)) | ftxui::color(statusColor),
                          ftxui::separator(),
                          ftxui::filler(),
                          ftxui::text("[q]uit [r]eset [f]lash [b]uild [d]ebugger_reset")
                            | ftxui::color(ftxui::Color::GrayDark)})
                     | ftxui::border;
            });
        }

        template<typename Reader>
        int run(Reader&            rttReader,
                std::string const& buildCommand) {
            if(!initializeBuildCommand(buildCommand)) {
                fmt::print(stderr, "❌ Invalid build command: {}\n", buildCommand);
                return 1;
            }

            static std::atomic<bool> gotSignal{};
            {
                struct sigaction sa{};
                sa.sa_handler = [](int) { gotSignal = true; };
                ::sigemptyset(&sa.sa_mask);
                if(::sigaction(SIGINT, &sa, nullptr) == -1) {
                    fmt::print(stderr,
                               "Failed to register signal handler: {}\n",
                               std::strerror(errno));
                    return 1;
                }
            }

            auto             screen = ftxui::ScreenInteractive::Fullscreen();
            ftxui::Component mainComponent;
            {
                std::lock_guard<std::mutex> lock{mutex};
                screen_ptr = &screen;

                mainComponent
                  = ftxui::CatchEvent(getTabComponent(rttReader), [&](ftxui::Event event) {
                        if(tab_selected == 1 && event.is_character()) {
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
                        if(event == ftxui::Event::Character('d')) {
                            rttReader.resetJLink();
                            return true;
                        }
                        if(event == ftxui::Event::Character('b')) {
                            executeBuild();
                            return true;
                        }
                        if(event == ftxui::Event::Character('q')) {
                            screen_ptr->Exit();
                            return true;
                        }

                        return false;
                    });
            }
            ftxui::Loop loop(screen_ptr, mainComponent);

            while(!loop.HasQuitted() && !gotSignal) {
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    loop.RunOnce();
                }
                std::this_thread::sleep_for(GUI_Constants::UpdateInterval);
            }

            return 0;
        }
    };
}}   // namespace uc_log::FTXUIGui
