#pragma once

// A printer::TargetPort for host tests and the TUI demo (host/gui_test.cpp): the link state as fields, every TargetOp
// batch handed to `onAccess`, the phase-level requests counted and, with `onEvent`, each said as a line.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::test {

struct FakeTargetPort final : printer::TargetPort {
    using Access = std::function<Result(std::span<printer::TargetOp const>)>;
    using Event  = std::function<void(std::string const&)>;

    Access                     onAccess{};
    Event                      onEvent{};
    std::atomic<bool>          halted{};
    std::atomic<bool>          flashing{};
    std::atomic<std::uint64_t> sessions{1};
    std::atomic<int>           flashes{};
    std::atomic<int>           resets{};
    std::atomic<int>           probeResets{};
    std::vector<Probe>         probeList{};

    FakeTargetPort() = default;

    explicit FakeTargetPort(Access access UC_LOG_LIFETIMEBOUND) : onAccess{std::move(access)} {}

    Result accessTarget(std::span<printer::TargetOp const> ops,
                        std::chrono::milliseconds) override {
        if(!onAccess) { return std::unexpected{std::string{"no target in this test"}}; }
        return onAccess(ops);
    }

    LinkStatus linkStatus() const override {
        std::scoped_lock<std::mutex> lock{linkMutex};
        return link;
    }

    /// Changes the link state under the fake's lock: a thread of the test (or the demo) beside the one that reads it.
    template<typename Change>
    void updateLink(Change&& change) {
        std::scoped_lock<std::mutex> lock{linkMutex};
        std::forward<Change>(change)(link);
    }

    bool isHalted() const override { return halted; }

    bool isFlashing() const override { return flashing; }

    std::uint64_t sessionCount() const override { return sessions; }

    void flash() override {
        ++flashes;
        said("Flash target");
    }

    void resetTarget() override {
        ++resets;
        said("Reset target");
    }

    void resetJLink() override {
        ++probeResets;
        said("Reset debugger");
    }

    void refreshProbes() override {
        ++probesVersion;
        said("Refresh probes");
    }

    std::vector<Probe> getProbes() const override { return probeList; }

    std::uint64_t getProbesVersion() const override { return probesVersion; }

    void setConnection(ConnectionSettings settings) override {
        said("Set connection: host \"" + settings.host + "\" probe \"" + settings.probe + "\"");
    }

    void setNoLogTimeout(std::uint32_t seconds) override {
        said("Set no-log timeout: " + std::to_string(seconds));
    }

private:
    mutable std::mutex         linkMutex{};
    LinkStatus                 link{.isRunning = 1};
    std::atomic<std::uint64_t> probesVersion{};

    void said(std::string const& text) const {
        if(onEvent) { onEvent(text); }
    }
};

}   // namespace uc_log::test
