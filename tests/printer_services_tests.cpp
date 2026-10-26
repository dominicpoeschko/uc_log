// The printer's services out of main (host/uc_log/printer/): the printer's own log rows, the loss-ledger watch on the
// virtual clock against a fake target, and the .rttlog / .status.log pair of LogFiles.
#include "support/Check.hpp"
#include "support/FakeTargetPort.hpp"
#include "uc_log/printer/BootRows.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/LogFiles.hpp"
#include "uc_log/printer/LossLedgerWatch.hpp"
#include "uc_log/printer/MapFacts.hpp"
#include "uc_log/printer/MessageLog.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;
namespace fs = std::filesystem;

fs::path scratch(std::string const& name) {
    auto const dir
      = fs::temp_directory_path() / ("uc_log_printer_services_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir / name;
}

std::vector<std::string> lines(fs::path const& path) {
    std::ifstream            in{path};
    std::vector<std::string> out;
    for(std::string line; std::getline(in, line);) { out.push_back(line); }
    return out;
}

void bootRows() {
    auto const r = uc_log::printer::unreadBootsRow(2, 5, 7);
    CHECK(r.channel.channel == uc_log::printer::unreadBootsChannel && r.module == "uc_log"
            && r.logLevel == uc_log::LogLevel::warn && r.fileName == "uc_log_printer"
            && r.functionName == "printer" && r.parsedOk,
          "the printer's own row: its channel, module, level and site");
    CHECK(r.logMsg == "⚠ 2 boots passed unread (boots that logged 5 -> 7)", "unread boots, plural");
    CHECK(uc_log::printer::unreadBootsRow(1, 5, 6).logMsg
            == "⚠ 1 boot passed unread (boots that logged 5 -> 6)",
          "and singular");
    CHECK(uc_log::printer::bootCountRestartRow().logMsg
            == "⚠ the boot count starts again: the printer's download wrote over the target's RAM",
          "the boot count restart");
}

void lossLedgerWatch() {
    auto const map = scratch("ledger.map");
    std::ofstream{map}
      << "20079780 20079780       20     1                 uc_log::detail::LossMembers<B, L, "
         "2u>::lossLedger\n";
    uc_log::printer::MapFacts    facts{map};
    uc_log::printer::MessageLog  messages;
    std::atomic<int>             countedTrue{0};
    std::atomic<int>             countedFalse{0};
    std::atomic<int>             reads{0};
    uc_log::printer::TargetLoss  loss;
    std::mutex                   lossMutex;
    std::vector<std::byte> const ledger = [] {
        // ring 0: 10 lost, 0 reported; the no-ring slot: nothing
        std::vector<std::byte> raw(32);
        raw[0] = std::byte{10};
        return raw;
    }();
    uc_log::test::FakeTargetPort port{
      [&](std::span<uc_log::printer::TargetOp const> ops) -> uc_log::printer::TargetPort::Result {
          uc_log::printer::Clock::sleep_for(1ms);
          auto const* r = std::get_if<uc_log::printer::target_op::Read>(&ops.front());
          if(r == nullptr || r->address != 0x2007'9780U || r->size != 0x20) {
              return std::unexpected{std::string{"not the ledger"}};
          }
          ++reads;
          return std::vector<std::vector<std::byte>>{ledger};
      }};
    {
        uc_log::printer::LossLedgerWatch const watch{
          facts,
          port,
          loss,
          lossMutex,
          messages,
          [&](bool counted) { ++(counted ? countedTrue : countedFalse); }};
        uc_log::printer::Clock::sleep_for(9s);
        CHECK(countedTrue == 1 && countedFalse == 0, "the map has a ledger: counted, once");
        CHECK(reads >= 3, "read every 2 s");
        auto const said = messages.newest(10);
        CHECK(said.size() == 1 && said[0].level == "error"
                && said[0].text.contains("10 log records"),
              "a ring quiet after its loss is said once");
        std::ofstream{map}
          << "20079440 20079440       48     4                 uc_log::X::rttControlBlock\n";
        fs::last_write_time(map, fs::last_write_time(map) + 1s);
        uc_log::printer::Clock::sleep_for(3s);
        CHECK(countedFalse == 1, "a rebuilt map without a ledger: not counted");
    }
    fs::remove(map);
}

void logFiles() {
    auto const dir = scratch("logs");
    fs::create_directories(dir);
    uc_log::printer::MessageLog                 messages;
    std::vector<uc_log::printer::LogFileStatus> states;
    {
        uc_log::printer::LogFiles files{
          messages,
          [&](uc_log::printer::LogFileStatus s, std::string_view) { states.push_back(s); },
          dir.string()};
        messages.status("hello");
        uc_log::printer::LogEntry e{0, std::string_view{}};
        e.logMsg   = "a line";
        e.parsedOk = true;
        files.add(std::chrono::system_clock::now(), e);
        uc_log::printer::Clock::sleep_for(uc_log::printer::LogFiles::flushPeriod * 3);
        auto const rtt = files.path();
        auto       log = lines(rtt);
        CHECK(states.size() == 1 && states[0] == uc_log::printer::LogFileStatus::active,
              "opened: Active");
        CHECK(log.size() == 2 && log[1].contains("a line"),
              "header and the row, flushed on the clock");
        auto status = rtt;
        status.replace_extension(".status.log");
        auto const st = lines(status);
        CHECK(st.size() == 1 && st[0].ends_with("[status] hello"), "the status row next to it");

        files.setEnabled(false);
        files.add(std::chrono::system_clock::now(), e);
        uc_log::printer::Clock::sleep_for(uc_log::printer::LogFiles::flushPeriod * 3);
        CHECK(lines(rtt).size() == 2, "disabled: nothing written");
        files.setEnabled(true);

        files.changeDir((dir / "missing" / "deeper").string());
        CHECK(states.size() == 2 && states[1] == uc_log::printer::LogFileStatus::error,
              "a directory that does not exist: Error");
        CHECK(messages.counts().errors == 1, "and said");
    }
    fs::remove_all(dir);
}
}   // namespace

int main(int    argc,
         char** argv) {
    uc_log::printer::VirtualTime const virtualTime;
    auto const result = uc_log::test::run(argc,
                                          argv,
                                          {
                                            {       "bootRows",        bootRows},
                                            {"lossLedgerWatch", lossLedgerWatch},
                                            {       "logFiles",        logFiles},
    });
    std::filesystem::remove_all(scratch("x").parent_path());
    return result;
}
