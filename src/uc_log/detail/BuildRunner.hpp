#pragma once

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
    #pragma clang diagnostic ignored "-Winconsistent-missing-destructor-override"
    #pragma clang diagnostic ignored "-Wextra-semi"
    #pragma clang diagnostic ignored "-Wextra-semi-stmt"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wnewline-eof"
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace uc_log { namespace FTXUIGui {

    enum class BuildStatus : std::uint8_t { Idle, Running, Success, Failed };

    struct BuildEntry {
        std::chrono::system_clock::time_point time;
        std::string                           line;
        bool                                  fromTool;
        bool                                  isError;
    };

    // Runs the user's build command on its own thread behind its own mutex, so build
    // output cannot contend with the UI mutex or the log producer. The UI reads through
    // the versioned snapshot accessors.
    struct BuildRunner {
        std::mutex mutable mutex;   // guards output, status, outputVersion
        std::vector<BuildEntry> output;
        BuildStatus             status{BuildStatus::Idle};
        std::uint64_t           outputVersion{0};

        std::atomic<bool> flashAfterBuild{false};
        std::atomic<bool> triggerFlashNow{false};
        std::atomic<bool> callJoin{false};

        // called from the build thread; must not take UI locks
        std::function<void()> requestRedraw;
        // called from the build thread when a build ends (true = success); also fired for
        // startup failures on the calling thread
        std::function<void(bool)> onBuildFinished;

        std::vector<std::string> originalBuildArguments;
        boost::filesystem::path  originalBuildExecutablePath;
        std::vector<std::string> buildArguments;
        boost::filesystem::path  buildExecutablePath;
        std::vector<std::string> buildEnvironment;

        std::unique_ptr<boost::asio::io_context> ioContext;
        std::jthread                             thread;

        ~BuildRunner() {
            if(thread.joinable()) {
                thread.request_stop();
                if(ioContext) { ioContext->stop(); }
                thread.join();
            }
        }

        void addOutput(std::string const& line,
                       bool               fromTool,
                       bool               isError) {
            {
                std::lock_guard<std::mutex> const lock{mutex};
                output.emplace_back(std::chrono::system_clock::now(), line, fromTool, isError);
                ++outputVersion;
            }
            if(requestRedraw) { requestRedraw(); }
        }

        void clearOutput() {
            std::lock_guard<std::mutex> const lock{mutex};
            output.clear();
            ++outputVersion;
        }

        BuildStatus getStatus() const {
            std::lock_guard<std::mutex> const lock{mutex};
            return status;
        }

        // refreshes the UI-side copy only when the output actually changed
        bool snapshotOutput(std::vector<BuildEntry>& copy,
                            std::uint64_t&           seenVersion) const {
            std::lock_guard<std::mutex> const lock{mutex};
            if(seenVersion == outputVersion) { return false; }
            seenVersion = outputVersion;
            copy        = output;
            return true;
        }

        void initialize(std::string const& buildCommandStr) {
            std::string currentArgument;
            bool        in_quotes   = false;
            bool        escape_next = false;

            for(char const character : buildCommandStr) {
                if(escape_next) {
                    currentArgument += character;
                    escape_next = false;
                } else if(character == '\\') {
                    escape_next = true;
                } else if(character == '"' || character == '\'') {
                    in_quotes = !in_quotes;
                } else if(character == ' ' && !in_quotes) {
                    if(!currentArgument.empty()) {
                        originalBuildArguments.push_back(currentArgument);
                        currentArgument.clear();
                    }
                } else {
                    currentArgument += character;
                }
            }

            if(!currentArgument.empty()) { originalBuildArguments.push_back(currentArgument); }

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
                buildArguments.emplace_back("-e");
                buildArguments.emplace_back("-q");
                buildArguments.emplace_back("-c");

                std::string commandStr = originalBuildExecutablePath.string();
                for(auto const& arg : originalBuildArguments) {
                    commandStr += " ";
                    if(arg.contains(' ')) {
                        commandStr += "\"" + arg + "\"";
                    } else {
                        commandStr += arg;
                    }
                }
                buildArguments.emplace_back(commandStr);
                buildArguments.emplace_back("/dev/null");

                buildExecutablePath = scriptPath;
            } else {
                buildArguments      = originalBuildArguments;
                buildExecutablePath = originalBuildExecutablePath;
            }

            for(auto const var : boost::process::environment::current()) {
                buildEnvironment.push_back(var.string());
            }

            buildEnvironment.emplace_back("FORCE_COLOR=1");
            buildEnvironment.emplace_back("CLICOLOR_FORCE=1");
            buildEnvironment.emplace_back("COLORTERM=truecolor");
            buildEnvironment.emplace_back("CMAKE_COLOR_DIAGNOSTICS=ON");
            buildEnvironment.emplace_back("NINJA_STATUS=[%f/%t] ");
        }

        void cancel() {
            try {
                if(getStatus() != BuildStatus::Running || !ioContext) { return; }

                if(thread.joinable()) {
                    thread.request_stop();
                    ioContext->stop();
                }
            } catch(std::exception const& e) {
                addOutput(fmt::format("❌ Error stopping build: {}", e.what()), false, true);
            }
        }

        // UI thread only
        void execute() {
            if(getStatus() == BuildStatus::Running || thread.joinable()) { return; }

            {
                std::lock_guard<std::mutex> const lock{mutex};
                output.clear();
                status = BuildStatus::Running;
                ++outputVersion;
            }

            try {
                ioContext = std::make_unique<boost::asio::io_context>();

                addOutput(fmt::format("🚀 Starting process: {} {}",
                                      originalBuildExecutablePath.string(),
                                      originalBuildArguments),
                          false,
                          false);

                thread = std::jthread{[this](std::stop_token const& stoken) {
                    try {
                        std::string                stdoutBuffer;
                        std::string                stderrBuffer;
                        boost::asio::readable_pipe stdoutPipe{*ioContext};
                        boost::asio::readable_pipe stderrPipe{*ioContext};

                        boost::process::v2::process buildProcess{
                          *ioContext,
                          buildExecutablePath,
                          buildArguments,
                          boost::process::v2::process_stdio{.in  = nullptr,
                                                            .out = stdoutPipe,
                                                            .err = stderrPipe},
                          boost::process::process_environment{buildEnvironment}
                        };

                        auto createRead
                          = [this](auto& pipe, auto& buffer, auto& self, bool isError) {
                                return [this, &pipe, &buffer, &self, isError]() {
                                    boost::asio::async_read_until(
                                      pipe,
                                      boost::asio::dynamic_buffer(buffer),
                                      '\n',
                                      [this, &buffer, &self, isError](
                                        boost::system::error_code error_code,
                                        std::size_t               bytes_transferred) {
                                          if(!error_code && bytes_transferred > 0) {
                                              auto pos = buffer.find('\n');
                                              if(pos != std::string::npos) {
                                                  std::string const line = buffer.substr(0, pos);
                                                  buffer.erase(0, pos + 1);
                                                  addOutput(line, true, isError);
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

                        buildProcess.async_wait([this, &processExitCode, &completed](
                                                  boost::system::error_code error_code,
                                                  int                       exitCode) {
                            processExitCode = exitCode;
                            if(error_code) {
                                addOutput(fmt::format("❌ Process error: {}", error_code.message()),
                                          false,
                                          true);
                            } else {
                                completed = true;
                                addOutput(fmt::format("🏁 Build {} (exit code: {})",
                                                      exitCode == 0 ? "succeeded" : "failed",
                                                      exitCode),
                                          false,
                                          exitCode != 0);
                            }
                            thread.request_stop();
                        });
                        while(!stoken.stop_requested()) {
                            ioContext->run_one_for(std::chrono::milliseconds{100});
                        }
                        if(!completed && buildProcess.running()) {
                            addOutput("❌ Build ended by user", false, true);
                            buildProcess.terminate();
                        }

                        {
                            std::lock_guard<std::mutex> const lock{mutex};
                            status
                              = (processExitCode == 0) ? BuildStatus::Success : BuildStatus::Failed;
                        }
                        if(onBuildFinished) { onBuildFinished(processExitCode == 0); }

                        if(processExitCode == 0 && flashAfterBuild.exchange(false)) {
                            addOutput("⚡ Build succeeded, triggering flash...", false, false);
                            triggerFlashNow = true;
                        }
                    } catch(std::exception const& e) {
                        {
                            std::lock_guard<std::mutex> const lock{mutex};
                            status = BuildStatus::Failed;
                        }
                        if(onBuildFinished) { onBuildFinished(false); }

                        if(flashAfterBuild.exchange(false)) {
                            addOutput("❌ Build failed, flash cancelled", false, true);
                        }
                        addOutput(fmt::format("❌ Build error: {}", e.what()), false, true);
                    }

                    callJoin = true;
                }};
            } catch(std::exception const& e) {
                {
                    std::lock_guard<std::mutex> const lock{mutex};
                    status = BuildStatus::Failed;
                }
                if(onBuildFinished) { onBuildFinished(false); }
                addOutput(fmt::format("❌ Build error: {}", e.what()), false, true);
            }
        }

        void executeAndFlash() {
            if(getStatus() == BuildStatus::Running || thread.joinable()) { return; }
            flashAfterBuild = true;
            execute();
        }

        // called from the UI loop: joins a finished build thread
        void joinIfFinished() {
            if(callJoin) {
                if(thread.joinable()) { thread.join(); }
                callJoin = false;
            }
        }
    };

}}   // namespace uc_log::FTXUIGui
