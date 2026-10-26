// printer/PrinterModel.hpp: the network settings' validation (ports, the bind address), the commands reaching the
// services through the hooks, the transport switch, and the state the services report.
#include "support/Check.hpp"
#include "uc_log/printer/PrinterModel.hpp"

#include <cstdint>
#include <string>
#include <vector>

using uc_log::printer::PrinterModel;

namespace {

void ports() {
    using uc_log::printer::parsePort;
    CHECK(!parsePort("1023") && parsePort("1024") == 1024 && parsePort("65535") == 65535
            && !parsePort("65536"),
          "1024..65535");
    CHECK(!parsePort("0") && !parsePort("") && !parsePort("80x") && !parsePort(" 2000")
            && !parsePort("-2000"),
          "garbage refused");
    CHECK(parsePort("65527", 65535 - 8) == 65527 && !parsePort("65528", 65535 - 8),
          "a lower limit for a base port");
}

void loopback() {
    using uc_log::printer::isLoopbackAddress;
    CHECK(isLoopbackAddress("127.0.0.1") && isLoopbackAddress("127.1.2.3")
            && isLoopbackAddress("::1") && isLoopbackAddress("localhost"),
          "loopback");
    CHECK(!isLoopbackAddress("0.0.0.0") && !isLoopbackAddress("192.0.2.4")
            && !isLoopbackAddress("::"),
          "exposed");
}

void commands() {
    PrinterModel               model{"127.0.0.1", 34565, 34600};
    std::vector<std::uint16_t> restarts;
    int                        stops = 0;
    std::uint16_t              current{};
    std::vector<std::string>   binds;
    std::vector<bool>          transports;
    std::vector<std::uint16_t> basePorts;
    std::vector<std::string>   dirs;
    model.setHooks(
      PrinterModel::Hooks{.restartControl = [&](std::uint16_t p) { restarts.push_back(p); },
                          .stopControl    = [&] { ++stops; },
                          .controlPort    = [&] { return current; },
                          .controlClients = [] { return std::size_t{2}; },
                          .setTransport =
                            [&](bool unix) {
                                transports.push_back(unix);
                                return unix ? std::string{"/tmp/log/control.sock"} : std::string{};
                            },
                          .setBindAddress =
                            [&](std::string const& a) {
                                binds.push_back(a);
                                return a != "not an address";
                            },
                          .duplexInfo        = {},
                          .setDuplexPort     = {},
                          .setDuplexEnabled  = {},
                          .setDuplexBasePort = [&](std::uint16_t p) { basePorts.push_back(p); },
                          .setLogDir         = [&](std::string const& d) { dirs.push_back(d); },
                          .setLogFileEnabled = {}});

    CHECK(!model.applyControlPort("80") && restarts.empty(), "a port below 1024: nothing done");
    CHECK(model.applyControlPort("40000") && restarts == std::vector<std::uint16_t>{40000},
          "a port applied");
    model.setControlEnabled(false);
    CHECK(stops == 1 && !model.snapshot().controlEnabled, "disabled");
    model.setControlEnabled(true);
    CHECK(restarts.back() == 34565, "enabled again at the configured port when none runs");
    current = 41000;
    model.setControlEnabled(true);
    CHECK(restarts.back() == 41000, "or at the one it had");

    CHECK(!model.applyBindAddress("not an address") && model.snapshot().bindAddress == "127.0.0.1",
          "an address that does not parse: kept the old one");
    CHECK(model.applyBindAddress("0.0.0.0") && model.snapshot().bindAddress == "0.0.0.0",
          "a new address");

    model.setTransport(true);
    CHECK(transports.back() && model.snapshot().controlSocketPath == "/tmp/log/control.sock",
          "unix sockets");
    model.setTransport(false);
    CHECK(model.snapshot().controlSocketPath.empty(), "TCP");

    CHECK(!model.applyDuplexBasePort("65530") && model.applyDuplexBasePort("34700")
            && basePorts == std::vector<std::uint16_t>{34700},
          "a base port the last channel still fits");
    model.applyLogDir("");
    model.applyLogDir("/tmp/x");
    CHECK(dirs == std::vector<std::string>{"/tmp/x"}, "an empty directory is not applied");
    CHECK(model.snapshot().controlClients == 2, "the client count from the server");
}

void reports() {
    PrinterModel model{"127.0.0.1", 34565, 34600};
    model.controlStatus(uc_log::printer::SocketPortStatus::active, 34565);
    model.logFileStatus(uc_log::printer::LogFileStatus::error, "/nowhere/x.rttlog");
    model.firmwareChecked(PrinterModel::FirmwareState::different);
    model.firmwareChecked(PrinterModel::FirmwareState::different);
    model.targetLossCounted(true);
    model.targetRecordsLost(5);
    auto s = model.snapshot();
    CHECK(s.controlStatus == uc_log::printer::SocketPortStatus::active && s.controlPort == 34565
            && s.controlStatusSerial == 1,
          "control status");
    CHECK(s.logFileStatus == uc_log::printer::LogFileStatus::error
            && s.logFilePath == "/nowhere/x.rttlog",
          "log file status");
    CHECK(s.firmware == PrinterModel::FirmwareState::different && s.firmwareMismatches == 2,
          "two mismatches");
    CHECK(s.targetLost == 5U, "lost records");
    model.messages().error("something");
    model.clearCounts();
    s = model.snapshot();
    CHECK(s.firmwareMismatches == 0 && s.targetLost == 0U && model.messages().counts().errors == 0,
          "Clear resets the counts");
    model.targetLossCounted(false);
    CHECK(!model.snapshot().targetLost, "a firmware that does not count: none");
}

void statistics() {
    PrinterModel model{"127.0.0.1", 34565, 34600};
    using L = uc_log::printer::TargetPort::LinkStatus;
    model.sampleLink(L{.numBytesRead = 10, .hostOverflowCount = 1, .isRunning = 1});
    model.sampleLink(L{.numBytesRead = 5, .isRunning = 1});
    model.sampleLink(L{.isRunning = 0});
    model.sampleLink(L{.numBytesRead = 30, .isRunning = 1});
    model.noteFlashRequest();
    model.noteResetRequest();
    model.noteResetRequest();
    auto const s = model.statistics();
    CHECK(s.jlinkReconnectionCount == 2 && s.jlinkDisconnectionCount == 1 && s.lastJLinkState,
          "connects and disconnects counted on the transitions");
    CHECK(s.maxBytesRead == 30 && s.maxOverflowCount == 1, "the highest the probe reported");
    CHECK(s.flashCount == 1 && s.resetRequestCount == 2 && s.totalBuildsStarted == 0,
          "the requests");
    model.resetStatistics();
    auto const r = model.statistics();
    CHECK(r.resetRequestCount == 0 && r.maxBytesRead == 0 && !r.lastJLinkState,
          "Reset starts over");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {     "ports",      ports},
                               {  "loopback",   loopback},
                               {  "commands",   commands},
                               {   "reports",    reports},
                               {"statistics", statistics},
    });
}
