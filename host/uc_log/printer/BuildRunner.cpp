#include "uc_log/printer/BuildRunner.hpp"

#include "uc_log/detail/Lifetimebound.hpp"

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

#include <exception>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>

namespace uc_log::printer {

struct BuildRunner::Process {
    std::vector<std::string> originalBuildArguments;
    boost::filesystem::path  originalBuildExecutablePath;
    std::vector<std::string> buildArguments;
    boost::filesystem::path  buildExecutablePath;
    std::vector<std::string> buildEnvironment;

    std::unique_ptr<boost::asio::io_context> ioContext;
};

BuildRunner::BuildRunner() : process{std::make_unique<Process>()} {}

BuildRunner::~BuildRunner() { stopAndJoin(); }

void BuildRunner::stopAndJoin() {
    if(thread.joinable()) {
        thread.request_stop();
        if(process->ioContext) { process->ioContext->stop(); }
        thread.join();
    }
}

void BuildRunner::addOutput(std::string const& line,
                            bool               fromTool,
                            bool               isError) {
    {
        std::scoped_lock<std::mutex> const lock{mutex};
        output.emplace_back(std::chrono::system_clock::now(), line, fromTool, isError);
        ++outputVersion;
    }
    if(requestRedraw) { requestRedraw(); }
}

void BuildRunner::clearOutput() {
    std::scoped_lock<std::mutex> const lock{mutex};
    output.clear();
    ++outputVersion;
}

BuildStatus BuildRunner::getStatus() const {
    std::scoped_lock<std::mutex> const lock{mutex};
    return status;
}

bool BuildRunner::busy() const { return getStatus() == BuildStatus::running || thread.joinable(); }

bool BuildRunner::snapshotOutput(std::vector<BuildEntry>& copy,
                                 std::uint64_t&           seenVersion) const {
    std::scoped_lock<std::mutex> const lock{mutex};
    if(seenVersion == outputVersion) { return false; }
    seenVersion = outputVersion;
    copy        = output;
    return true;
}

void BuildRunner::initialize(std::string const& buildCommandStr) {
    std::string currentArgument;
    bool        inQuotes   = false;
    bool        escapeNext = false;

    for(char const character : buildCommandStr) {
        if(escapeNext) {
            currentArgument += character;
            escapeNext = false;
        } else if(character == '\\') {
            escapeNext = true;
        } else if(character == '"' || character == '\'') {
            inQuotes = !inQuotes;
        } else if(character == ' ' && !inQuotes) {
            if(!currentArgument.empty()) {
                process->originalBuildArguments.push_back(currentArgument);
                currentArgument.clear();
            }
        } else {
            currentArgument += character;
        }
    }

    if(!currentArgument.empty()) { process->originalBuildArguments.push_back(currentArgument); }

    if(process->originalBuildArguments.empty()) {
        throw std::invalid_argument("empty build command");
    }

    process->originalBuildExecutablePath
      = boost::process::environment::find_executable(process->originalBuildArguments[0]);

    if(process->originalBuildExecutablePath.empty()) {
        throw std::invalid_argument(
          fmt::format("executable {} not found", process->originalBuildArguments[0]));
    }

    process->originalBuildArguments.erase(process->originalBuildArguments.begin());

    auto const scriptPath = boost::process::environment::find_executable("script");
    if(!scriptPath.empty()) {
        process->buildArguments.emplace_back("-e");
        process->buildArguments.emplace_back("-q");
        process->buildArguments.emplace_back("-c");

        std::string commandStr = process->originalBuildExecutablePath.string();
        for(auto const& arg : process->originalBuildArguments) {
            commandStr += ' ';
            if(arg.contains(' ')) {
                commandStr += "\"" + arg + "\"";
            } else {
                commandStr += arg;
            }
        }
        process->buildArguments.emplace_back(commandStr);
        process->buildArguments.emplace_back("/dev/null");

        process->buildExecutablePath = scriptPath;
    } else {
        process->buildArguments      = process->originalBuildArguments;
        process->buildExecutablePath = process->originalBuildExecutablePath;
    }

    for(auto const var : boost::process::environment::current()) {
        process->buildEnvironment.push_back(var.string());
    }

    process->buildEnvironment.emplace_back("FORCE_COLOR=1");
    process->buildEnvironment.emplace_back("CLICOLOR_FORCE=1");
    process->buildEnvironment.emplace_back("COLORTERM=truecolor");
    process->buildEnvironment.emplace_back("CMAKE_COLOR_DIAGNOSTICS=ON");
    process->buildEnvironment.emplace_back("NINJA_STATUS=[%f/%t] ");
}

void BuildRunner::cancel() {
    try {
        if(getStatus() != BuildStatus::running || !process->ioContext) { return; }

        if(thread.joinable()) {
            thread.request_stop();
            process->ioContext->stop();
        }
    } catch(std::exception const& e) {
        addOutput(fmt::format("❌ Error stopping build: {}", e.what()), false, true);
    }
}

void BuildRunner::execute() {
    if(busy()) { return; }

    {
        std::scoped_lock<std::mutex> const lock{mutex};
        output.clear();
        status = BuildStatus::running;
        ++outputVersion;
    }

    try {
        process->ioContext = std::make_unique<boost::asio::io_context>();

        addOutput(fmt::format("🚀 Starting process: {} {}",
                              process->originalBuildExecutablePath.string(),
                              process->originalBuildArguments),
                  false,
                  false);

        thread = std::jthread{[this](std::stop_token const& stoken) {
            try {
                std::string                stdoutBuffer;
                std::string                stderrBuffer;
                boost::asio::readable_pipe stdoutPipe{*process->ioContext};
                boost::asio::readable_pipe stderrPipe{*process->ioContext};

                boost::process::v2::process buildProcess{
                  *process->ioContext,
                  process->buildExecutablePath,
                  process->buildArguments,
                  boost::process::v2::process_stdio{.in  = nullptr,
                                                    .out = stdoutPipe,
                                                    .err = stderrPipe},
                  boost::process::process_environment{process->buildEnvironment}
                };

                auto createRead = [this](auto& pipe   UC_LOG_LIFETIMEBOUND,
                                         auto& buffer UC_LOG_LIFETIMEBOUND,
                                         auto& self   UC_LOG_LIFETIMEBOUND,
                                         bool         isError) {
                    return [this, &pipe, &buffer, &self, isError]() {
                        boost::asio::async_read_until(
                          pipe,
                          boost::asio::dynamic_buffer(buffer),
                          '\n',
                          [this, &buffer, &self, isError](boost::system::error_code errorCode,
                                                          std::size_t bytesTransferred) {
                              if(!errorCode && bytesTransferred > 0) {
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

                std::function<void()> readOut;
                readOut = createRead(stdoutPipe, stdoutBuffer, readOut, false);
                std::function<void()> readErr;
                readErr = createRead(stderrPipe, stderrBuffer, readErr, true);

                readOut();
                readErr();

                int  processExitCode = 1;
                bool completed       = false;

                buildProcess.async_wait(
                  [this, &processExitCode, &completed](boost::system::error_code errorCode,
                                                       int                       exitCode) {
                      processExitCode = exitCode;
                      if(errorCode) {
                          addOutput(fmt::format("❌ Process error: {}", errorCode.message()),
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
                    process->ioContext->run_one_for(std::chrono::milliseconds{100});
                }
                if(!completed && buildProcess.running()) {
                    addOutput("❌ Build ended by user", false, true);
                    buildProcess.terminate();
                }

                {
                    std::scoped_lock<std::mutex> const lock{mutex};
                    status = (processExitCode == 0) ? BuildStatus::success : BuildStatus::failed;
                }
                if(onBuildFinished) { onBuildFinished(processExitCode == 0); }

                if(processExitCode == 0 && flashAfterBuild.exchange(false)) {
                    addOutput("⚡ Build succeeded, triggering flash...", false, false);
                    triggerFlashNow = true;
                }
            } catch(std::exception const& e) {
                {
                    std::scoped_lock<std::mutex> const lock{mutex};
                    status = BuildStatus::failed;
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
            std::scoped_lock<std::mutex> const lock{mutex};
            status = BuildStatus::failed;
        }
        if(onBuildFinished) { onBuildFinished(false); }
        addOutput(fmt::format("❌ Build error: {}", e.what()), false, true);
    }
}

void BuildRunner::executeAndFlash() {
    if(busy()) { return; }
    flashAfterBuild = true;
    execute();
}

void BuildRunner::joinIfFinished() {
    if(callJoin) {
        if(thread.joinable()) { thread.join(); }
        callJoin = false;
    }
}

}   // namespace uc_log::printer
