#pragma once

// The printer's state and commands without a window: its messages, what the firmware check found, the records the
// target lost, the log files, the network settings (control socket / port, bind address, duplex channels) with
// their validation, the session statistics and the build runner (printer/BuildRunner.hpp: boost.process only in its
// .cpp). A front end (printer/tui) reads it through snapshot() and sends commands; headless runs use it
// alone. No FTXUI here: a second front end needs only rendering.
//
// Lock order: the model's mutex is taken and dropped inside each call, never held while it calls out (the network
// hooks, the log files). A front end takes its own lock only after snapshot() returned, so the order is
// model -> front end, never the other way; MessageLog calls its observers outside its own lock (MessageLog.hpp).

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/DuplexChannelInfo.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// What the Statistics tab counts since the printer started.
struct Statistics {
    std::chrono::system_clock::time_point sessionStartTime{std::chrono::system_clock::now()};

    // J-Link connection
    std::size_t jlinkReconnectionCount{0};
    std::size_t jlinkDisconnectionCount{0};
    bool        lastJLinkState{false};

    // builds
    std::size_t totalBuildsStarted{0};
    std::size_t successfulBuilds{0};
    std::size_t failedBuilds{0};

    // target control
    std::size_t flashCount{0};
    std::size_t resetRequestCount{0};

    // data (the highest the probe reported)
    std::size_t maxBytesRead{0};
    std::size_t maxOverflowCount{0};
};

/// A port a person typed: 1024..`max` (below 1024 needs root), nullopt for anything else.
inline std::optional<std::uint16_t> parsePort(std::string_view text,
                                              unsigned         max = 65535) {
    unsigned          v{};
    auto const* const end = std::to_address(text.end());
    auto const [ptr, ec]  = std::from_chars(std::to_address(text.begin()), end, v);
    if(ec != std::errc{} || ptr != end || text.empty() || v < 1024 || v > max) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

/// Loopback addresses keep the target's ports off the network; anything else exposes the unauthenticated duplex
/// shell to other hosts.
inline bool isLoopbackAddress(std::string_view address) {
    return address == "127.0.0.1" || address == "::1" || address == "localhost"
        || address.starts_with("127.");
}

class PrinterModel {
public:
    using FirmwareState = control::FirmwareState;

    static constexpr std::size_t maxDuplexChannels = 8;

    /// What the network commands act on (main: the control server, the duplex hub, the log files). An empty one is
    /// a feature this printer does not have.
    struct Hooks {
        std::function<void(std::uint16_t)>      restartControl;   // on this TCP port
        std::function<void()>                   stopControl;
        std::function<std::uint16_t()>          controlPort;   // 0: none
        std::function<std::size_t()>            controlClients;
        std::function<std::string(bool)>        setTransport;     // unix: the socket path, or ""
        std::function<bool(std::string const&)> setBindAddress;   // false: no address
        std::function<std::vector<printer::DuplexChannelInfo>()> duplexInfo;
        std::function<void(std::size_t, std::uint16_t)>          setDuplexPort;
        std::function<void(std::size_t, bool)>                   setDuplexEnabled;
        std::function<void(std::uint16_t)>                       setDuplexBasePort;
        std::function<void(std::string const&)>                  setLogDir;
        std::function<void(bool)>                                setLogFileEnabled;
    };

    struct Snapshot {
        SocketPortStatus controlStatus{SocketPortStatus::notStarted};
        std::uint16_t    controlPort{};
        std::uint64_t    controlStatusSerial{};   // moves with every report of the control server
        std::string      controlSocketPath;       // not empty: a unix socket
        bool             controlEnabled{true};
        std::size_t      controlClients{};
        std::string      bindAddress;
        std::uint16_t    duplexBasePort{};

        LogFileStatus logFileStatus{LogFileStatus::notStarted};
        std::string   logFilePath;
        std::uint64_t logFileSerial{};
        bool          logFileEnabled{true};

        FirmwareState firmware{FirmwareState::unchecked};
        std::size_t   firmwareMismatches{};
        std::optional<std::uint64_t>
          targetLost;   // nullopt: the firmware does not count its lost records
    };

    /// The command line's: the bind address, the control port a re-enable falls back to, the duplex base port.
    PrinterModel(std::string   bindAddress,
                 std::uint16_t controlPort,
                 std::uint16_t duplexBasePort);
    ~PrinterModel();

    PrinterModel(PrinterModel const&)            = delete;
    PrinterModel& operator=(PrinterModel const&) = delete;

    MessageLog& messages() UC_LOG_LIFETIMEBOUND { return messages_; }

    /// Before the front end runs.
    void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    Snapshot snapshot() const {
        Snapshot s;
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            s = state_;
        }
        s.controlClients = hooks_.controlClients ? hooks_.controlClients() : 0;
        return s;
    }

    std::vector<printer::DuplexChannelInfo> duplexInfo() const {
        return hooks_.duplexInfo ? hooks_.duplexInfo() : std::vector<printer::DuplexChannelInfo>{};
    }

    // ---- what the services report --------------------------------------------------------------

    void controlStatus(SocketPortStatus status,
                       std::uint16_t    port) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.controlStatus = status;
        state_.controlPort   = port;
        ++state_.controlStatusSerial;
    }

    void controlSocket(std::string path) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.controlSocketPath = std::move(path);
    }

    void logFileStatus(LogFileStatus    status,
                       std::string_view path) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.logFileStatus = status;
        state_.logFilePath   = std::string{path};
        ++state_.logFileSerial;
    }

    void firmwareChecked(FirmwareState state) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.firmware = state;
        if(state == FirmwareState::different) { ++state_.firmwareMismatches; }
    }

    /// The firmware counts dropped log records (its map has ::lossLedger), or does not.
    void targetLossCounted(bool counted) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(!counted) {
            state_.targetLost.reset();
        } else if(!state_.targetLost) {
            state_.targetLost = 0;
        }
    }

    void targetRecordsLost(std::uint64_t n) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.targetLost = state_.targetLost.value_or(0) + n;
    }

    /// The Status tab's Clear: the messages and the counts it shows.
    void clearCounts() {
        messages_.clear();
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.firmwareMismatches = 0;
        if(state_.targetLost) { state_.targetLost = 0; }
    }

    // ---- statistics and builds -----------------------------------------------------------------------

    Statistics statistics() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return statistics_;
    }

    /// The Statistics tab's Reset: from now on.
    void resetStatistics() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        statistics_ = Statistics{};
    }

    void noteResetRequest() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        ++statistics_.resetRequestCount;
    }

    void noteFlashRequest() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        ++statistics_.flashCount;
    }

    /// The link as the probe reports it, once per frame: connects / disconnects and the highest counts.
    void sampleLink(TargetPort::LinkStatus const& link) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        bool const                         connected = link.isRunning != 0;
        if(connected && !statistics_.lastJLinkState) {
            ++statistics_.jlinkReconnectionCount;
        } else if(!connected && statistics_.lastJLinkState) {
            ++statistics_.jlinkDisconnectionCount;
        }
        statistics_.lastJLinkState = connected;
        statistics_.maxBytesRead
          = std::max<std::size_t>(statistics_.maxBytesRead, link.numBytesRead);
        statistics_.maxOverflowCount = std::max(statistics_.maxOverflowCount,
                                                static_cast<std::size_t>(link.hostOverflowCount));
    }

    /// The build runner of the Build tab.
    BuildRunner& builds() UC_LOG_LIFETIMEBOUND { return builds_; }

    /// A build (and a flash after it when `andFlash`); false: one runs already.
    bool startBuild(bool andFlash);

    // ---- commands --------------------------------------------------------------------------------

    /// A new control port; false (nothing done) when `text` is no port 1024..65535.
    bool applyControlPort(std::string_view text) {
        auto const port = parsePort(text);
        if(!port) { return false; }
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.controlEnabled = true;
        }
        if(hooks_.restartControl) { hooks_.restartControl(*port); }
        return true;
    }

    /// On again at the port it had (or the configured one).
    void setControlEnabled(bool enabled) {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.controlEnabled = enabled;
        }
        if(enabled) {
            auto const current = hooks_.controlPort ? hooks_.controlPort() : std::uint16_t{};
            if(hooks_.restartControl) {
                hooks_.restartControl(current != 0 ? current : configuredControlPort_);
            }
        } else if(hooks_.stopControl) {
            hooks_.stopControl();
        }
    }

    /// `unix`: control.sock and duplex.<n>.sock in the log directory; else TCP on the bind address.
    void setTransport(bool unix) {
        auto path = hooks_.setTransport ? hooks_.setTransport(unix) : std::string{};
        controlSocket(std::move(path));
    }

    /// false: no address (nothing changed).
    bool applyBindAddress(std::string const& address) {
        if(address.empty() || !hooks_.setBindAddress || !hooks_.setBindAddress(address)) {
            return false;
        }
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.bindAddress = address;
        return true;
    }

    bool applyDuplexPort(std::size_t      ordinal,
                         std::string_view text) {
        auto const port = parsePort(text);
        if(!port || !hooks_.setDuplexPort) { return false; }
        hooks_.setDuplexPort(ordinal, *port);
        return true;
    }

    /// Every channel's port from `text` on: the last one must still fit.
    bool applyDuplexBasePort(std::string_view text) {
        auto const port = parsePort(text, 65535 - maxDuplexChannels);
        if(!port || !hooks_.setDuplexBasePort) { return false; }
        hooks_.setDuplexBasePort(*port);
        return true;
    }

    void toggleDuplex(std::size_t ordinal) {
        auto const infos = duplexInfo();
        if(ordinal < infos.size() && hooks_.setDuplexEnabled) {
            hooks_.setDuplexEnabled(ordinal, !infos[ordinal].enabled);
        }
    }

    void applyLogDir(std::string const& dir) {
        if(dir.empty() || !hooks_.setLogDir) { return; }
        hooks_.setLogDir(dir);
    }

    void toggleLogFile() {
        bool enabled{};
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            enabled = state_.logFileEnabled = !state_.logFileEnabled;
        }
        if(hooks_.setLogFileEnabled) { hooks_.setLogFileEnabled(enabled); }
    }

private:
    MessageLog         messages_;
    Statistics         statistics_;   // mutex_
    Hooks              hooks_;
    std::uint16_t      configuredControlPort_{};
    mutable std::mutex mutex_;
    Snapshot           state_;
    // last: its build thread is joined while the members its callback uses still stand
    BuildRunner builds_;
};

}   // namespace uc_log::printer
