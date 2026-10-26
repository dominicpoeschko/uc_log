#pragma once

// Runs the user's build command on its own thread behind its own mutex, so build output cannot contend with a front
// end's mutex or the log producer. A front end reads through the versioned snapshot accessors. boost.process is in
// BuildRunner.cpp only: a program that includes this header compiles no part of it.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uc_log::printer {

enum class BuildStatus : std::uint8_t { idle, running, success, failed };

struct BuildEntry {
    std::chrono::system_clock::time_point time;
    std::string                           line;
    bool                                  fromTool;
    bool                                  isError;
};

class BuildRunner {
public:
    BuildRunner();
    ~BuildRunner();

    BuildRunner(BuildRunner const&)            = delete;
    BuildRunner& operator=(BuildRunner const&) = delete;

    // called from the build thread; must not take a front end's locks
    std::function<void()> requestRedraw;
    // called from the build thread when a build ends (true = success); also fired for
    // startup failures on the calling thread
    std::function<void(bool)> onBuildFinished;

    /// a successful build of executeAndFlash() sets it; the front end's loop takes it and flashes
    std::atomic<bool> triggerFlashNow{false};

    /// splits the command line (quotes, backslash escapes) and finds the executable; throws std::invalid_argument
    void initialize(std::string const& buildCommandStr);

    // a running build is stopped and its thread joined (its callbacks are not called after this)
    void stopAndJoin();

    void addOutput(std::string const& line,
                   bool               fromTool,
                   bool               isError);

    void clearOutput();

    BuildStatus getStatus() const;

    /// a build runs, or its thread is not joined yet (joinIfFinished)
    bool busy() const;

    // refreshes the front end's copy only when the output actually changed
    bool snapshotOutput(std::vector<BuildEntry>& copy,
                        std::uint64_t&           seenVersion) const;

    void cancel();

    // front end thread only
    void execute();

    void executeAndFlash();

    // called from the front end's loop: joins a finished build thread
    void joinIfFinished();

private:
    struct Process;   // the boost.process / asio side (BuildRunner.cpp)

    std::mutex mutable mutex;   // guards output, status, outputVersion
    std::vector<BuildEntry> output;
    BuildStatus             status{BuildStatus::idle};
    std::uint64_t           outputVersion{0};

    std::atomic<bool> flashAfterBuild{false};
    std::atomic<bool> callJoin{false};

    std::unique_ptr<Process> process;
    std::jthread             thread;
};

}   // namespace uc_log::printer
