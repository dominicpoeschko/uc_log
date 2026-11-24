// printer/BuildRunner.hpp: the build command runs as any program does, its stop signals not blocked. The printer
// blocks SIGINT, SIGTERM and SIGHUP in every one of its threads (printer/StopSignals.hpp) and a signal mask survives
// fork and exec: a build started with the block would let Ctrl-C, a kill and the hang-up its pty sends when the printer
// ends it go by, and run on after the printer. The build writes its own blocked set (SigBlk of /proc/self/status) to a
// file; the test waits for the build's end without a deadline (ctest's TIMEOUT bounds a hang).
#include "support/Check.hpp"
#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/StopSignals.hpp"

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace {

// the SigBlk line a build wrote: the blocked signals, bit n-1 for signal n (proc(5))
std::optional<std::uint64_t> blockedSetIn(std::filesystem::path const& file) {
    std::ifstream in{file};
    std::string   key;
    std::string   hex;
    if(!(in >> key >> hex) || key != "SigBlk:") { return std::nullopt; }
    return std::stoull(hex, nullptr, 16);
}

void aBuildRunsWithoutTheStopSignalsBlocked() {
    uc_log::printer::StopSignals const stop;   // as main has it, before the build's thread starts
    auto const                         file = std::filesystem::temp_directory_path()
                                            / ("uc_log_build_runner_" + std::to_string(::getpid()));
    std::filesystem::remove(file);

    std::promise<bool> finished;
    {
        uc_log::printer::BuildRunner runner;
        runner.onBuildFinished = [&finished](bool ok) { finished.set_value(ok); };
        runner.initialize("sh -c \"grep ^SigBlk: /proc/self/status > " + file.string() + "\"");
        runner.execute();
        CHECK(finished.get_future().get(), "the build ran and succeeded");
        runner.stopAndJoin();
    }

    auto const blocked = blockedSetIn(file);
    CHECK(blocked.has_value(), "the build wrote its blocked set");
    for(int const signal : {SIGINT, SIGTERM, SIGHUP}) {
        CHECK(blocked.has_value() && (*blocked & (std::uint64_t{1} << (signal - 1))) == 0,
              "the build takes the stop signal as any program does");
    }

    sigset_t mine;
    sigemptyset(&mine);
    pthread_sigmask(SIG_BLOCK, nullptr, &mine);
    CHECK(sigismember(&mine, SIGINT) == 1 && sigismember(&mine, SIGTERM) == 1
            && sigismember(&mine, SIGHUP) == 1,
          "the printer's own thread keeps them blocked");
    std::filesystem::remove(file);
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(
      argc,
      argv,
      {
        {"aBuildRunsWithoutTheStopSignalsBlocked", aBuildRunsWithoutTheStopSignalsBlocked},
    });
}
