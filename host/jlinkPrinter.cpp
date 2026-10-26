#include "jlink/JLink.hpp"

#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/wire.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/printer/AnnouncedReset.hpp"
#include "uc_log/printer/BootRows.hpp"
#include "uc_log/printer/Catalog.hpp"
#include "uc_log/printer/ControlEvents.hpp"
#include "uc_log/printer/ControlServer.hpp"
#include "uc_log/printer/DuplexChannelServer.hpp"
#include "uc_log/printer/Environment.hpp"
#include "uc_log/printer/FirmwareCheck.hpp"
#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/HexToken.hpp"
#include "uc_log/printer/JLinkRttReader.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LogFiles.hpp"
#include "uc_log/printer/LogFormat.hpp"
#include "uc_log/printer/LossLedgerWatch.hpp"
#include "uc_log/printer/MapFacts.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/Options.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/RamImage.hpp"
#include "uc_log/printer/RowFile.hpp"
#include "uc_log/printer/RttBlockInfo.hpp"
#include "uc_log/printer/RttChannelMap.hpp"
#include "uc_log/printer/SeggerTerms.hpp"
#include "uc_log/printer/SocketSession.hpp"
#include "uc_log/printer/TargetLoss.hpp"
#include "uc_log/printer/TimeDelayedQueue.hpp"
#include "uc_log/printer/metric_utils.hpp"
#include "uc_log/printer/tui/Gui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

struct OnScopeExit {
    std::function<void()> f;

    ~OnScopeExit() {
        if(f) { f(); }
    }
};

// The compile-time log filter the firmware was built with (LogFilter.hpp), for the Filter tab.
std::optional<uc_log::detail::FilterTable> loadLogFilter(uc_log::printer::MessageLog& messages,
                                                         std::string const&           file) {
    if(file.empty()) { return std::nullopt; }
    std::ifstream     stream{file};
    std::string const text{std::istreambuf_iterator<char>{stream}, {}};
    auto const        parsed = uc_log::detail::parseFilter(text);
    if(!stream.is_open()) {
        messages.error(fmt::format("log filter {}: cannot open it", file));
        return std::nullopt;
    }
    if(!parsed.error.empty()) {
        messages.error(fmt::format("log filter {}:{}: {}", file, parsed.line, parsed.error));
        return std::nullopt;
    }
    messages.status(fmt::format("log filter {}: {} module rule(s), every line >= {}",
                                file,
                                parsed.table.count,
                                uc_log::detail::filterLevelName(parsed.table.global)));
    return parsed.table;
}

// SEGGER's daily EDU terms (printer/SeggerTerms.hpp), accepted for today before every connect so a connect never waits on
// the dialog - just before local midnight for the new day, after waiting for it; an outcome other than "already
// today" is said once.
std::function<void()> termsAcceptor(uc_log::printer::MessageLog& messages UC_LOG_LIFETIMEBOUND) {
    return
      [&messages, lastSaid = std::optional<uc_log::printer::segger_terms::Result>{}]() mutable {
          using uc_log::printer::segger_terms::Result;
          namespace st = uc_log::printer::segger_terms;
          if(auto const wait = st::waitForNewDay(std::chrono::system_clock::now());
             wait > decltype(wait){})
          {
              messages.tool(
                fmt::format("SEGGER's EDU terms of use: connecting after local midnight, in {} s",
                            std::chrono::ceil<std::chrono::seconds>(wait).count()));
              std::this_thread::sleep_for(wait);
          }
          auto const file   = st::registryFile();
          auto const result = st::acceptToday(file);
          if(result == lastSaid) { return; }
          lastSaid = result;
          switch(result) {
          case Result::accepted:
              messages.tool("SEGGER's EDU terms of use accepted for today");
              break;
          case Result::alreadyToday: break;
          case Result::noFile:
              messages.tool(fmt::format(
                "SEGGER's EDU terms of use: no registry file {:?} (a J-Link EDU may wait on "
                "its daily dialog)",
                file.string()));
              break;
          case Result::noKey:
              messages.tool(fmt::format(
                "SEGGER's EDU terms of use: no {} in {:?} (a J-Link EDU may wait on its daily "
                "dialog)",
                uc_log::printer::segger_terms::key,
                file.string()));
              break;
          case Result::unwritable:
              messages.toolError(
                fmt::format("SEGGER's EDU terms of use: {:?} could not be written", file.string()));
              break;
          }
      };
}

uc_log::printer::ControlTarget
controlTarget(JLinkRttReader&                                         rttReader,
              std::function<uc_log::printer::control::StatusAnswer()> status,
              uc_log::printer::MessageLog&                            messages,
              uc_log::printer::MapFacts&                              mapFacts,
              std::string const&                                      mapFile) {
    return uc_log::printer::ControlTarget{
      .target = &rttReader,
      .status = std::move(status),
      .reset =
        [&rttReader](std::function<bool()> const& cancelled) {
            return rttReader.resetAndWait(std::chrono::seconds{20}, cancelled);
        },
      .resetLine =
        [&rttReader](std::function<bool()> const& cancelled) {
            return rttReader.resetAndWait(std::chrono::seconds{20}, cancelled, true);
        },
      .flash =
        [&rttReader](std::function<bool()> const& cancelled) {
            return rttReader.flashAndWait(std::chrono::seconds{90}, cancelled);
        },
      .messages = [&messages](std::size_t count) { return messages.newest(count); },
      // records, raise_panic: the parse the inspector and the reader use; no --map_file: none
      .map =
        [&mapFacts,
         path = mapFile.empty() ? std::string{} : std::filesystem::absolute(mapFile).string()] {
            if(path.empty()) { return uc_log::printer::ControlMap{}; }
            auto const facts = mapFacts.get();
            return uc_log::printer::ControlMap{.snapshot   = facts->map,
                                               .path       = path,
                                               .generation = facts->generation};
        },
      .unixMicros   = {},
      .steadyMicros = {},
      .stop         = {}};
}

// The end is bounded: from the stop on, the reader drains and decodes what it holds and the threads join - if that
// is not done within exitDeadline (a probe that hangs, a backlog that grinds), the process ends anyway and says so: a
// printer that does not go away holds the probe and blocks the next one.
void armExitDeadline() {
    static constexpr auto exitDeadline = std::chrono::seconds{5};
    std::thread{[] {
        std::this_thread::sleep_for(exitDeadline);
        std::fputs(
          "uc_log_printer: did not end within 5 s of the stop - exiting anyway; whatever was read "
          "from the "
          "target and not decoded yet is dropped\n",
          stderr);
        std::fflush(stderr);
        std::_Exit(3);
    }}.detach();
}

}   // namespace

int main(int    argc,
         char** argv) {
    auto const opts = uc_log::printer::parseOptions(argc, argv);
    if(!opts) { return 1; }
    auto const& o         = *opts;
    auto const  startedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    uc_log::printer::PrinterModel model{o.bindAddressString, o.controlPort, o.duplexBasePort};
    auto&                         messages = model.messages();
    // headless (--disable_ui): no window, the messages on stderr
    messages.setEcho(o.disableUi);
    std::optional<uc_log::printer::tui::Gui> gui;
    if(!o.disableUi) { gui.emplace(model); }
    if(auto const table = loadLogFilter(messages, o.logFilterFile); table && gui) {
        gui->setCompiledFilter(*table);
    }
    uc_log::printer::LogFiles logFiles{
      messages,
      [&model](uc_log::printer::LogFileStatus s, std::string_view p) { model.logFileStatus(s, p); },
      o.logDir};
    uc_log::printer::AsioContext asioContext;
    auto const                   controlSocketFor = [&o](bool unix) {
        if(!unix) { return std::filesystem::path{}; }
        return o.controlSocket == "auto" ? uc_log::printer::controlSocketPath(o.logDir)
                                         : std::filesystem::path{o.controlSocket};
    };

    // a failed bind does not stop logging
    std::optional<uc_log::printer::ControlServer> controlServer;
    if(o.controlSocket != "off" && !o.controlSocket.empty()) {
        auto const path = controlSocketFor(o.unixTransport);
        controlServer.emplace(
          o.bindAddress,
          o.controlPort,
          path,
          [&messages](std::string_view msg) {
              messages.toolError(fmt::format("control server: {}", msg));
          },
          [&model](uc_log::printer::SocketPortStatus s, std::uint16_t p) {
              model.controlStatus(s, p);
          });
        controlServer->setHistoryLimit(o.controlHistoryMb * 1024 * 1024);
        model.controlSocket(path.native());
        messages.tool(path.empty()
                        ? fmt::format("control server on {}:{}", o.bindAddressString, o.controlPort)
                        : fmt::format("control socket {}", path.native()));
        messages.setStream([&controlServer](uc_log::printer::control::StatusMessage const& m) {
            controlServer->publish(m);
        });
    }
    // before the server goes: nothing publishes into it while it is torn down
    OnScopeExit const stopStatusStream{[&messages] { messages.setStream({}); }};

    uc_log::printer::DuplexChannelHub duplexHub{
      asioContext.ioc,
      o.bindAddress,
      o.duplexBasePort,
      [&messages](std::string_view msg) { messages.error(msg); },
      [&messages](std::string_view msg) { messages.status(msg); },
      [&gui]() {
          if(gui) { gui->triggerRedraw(); }
      },
      o.unixTransport ? std::filesystem::path{o.logDir} : std::filesystem::path{}};
    // the Settings tab's commands, on the control server, the duplex channels and the log files
    model.setHooks(uc_log::printer::PrinterModel::Hooks{
      .restartControl =
        [&controlServer](std::uint16_t port) {
            if(controlServer) { controlServer->restart(port); }
        },
      .stopControl =
        [&controlServer] {
            if(controlServer) { controlServer->stop(); }
        },
      .controlPort
      = [&controlServer] { return controlServer ? controlServer->getPort() : std::uint16_t{}; },
      .controlClients = [&controlServer]() -> std::size_t {
          return controlServer ? controlServer->getClientCount() : 0;
      },
      .setTransport =
        [&controlServer, controlSocketFor, &duplexHub, logDir = o.logDir](bool unix) {
            auto const controlPath = controlSocketFor(unix);
            if(controlServer) { controlServer->setSocketPath(controlPath); }
            duplexHub.setSocketDir(unix ? std::filesystem::path{logDir} : std::filesystem::path{});
            return controlServer ? controlPath.native() : std::string{};
        },
      // every socket rebound at runtime; false on an unparsable address, so the TUI can show it without applying it
      .setBindAddress =
        [&controlServer, &duplexHub](std::string const& text) {
            boost::system::error_code ec;
            auto const                address = boost::asio::ip::make_address(text, ec);
            if(ec) { return false; }
            if(controlServer) { controlServer->setBindAddress(address); }
            duplexHub.setBindAddress(address);
            return true;
        },
      .duplexInfo = [&duplexHub] { return duplexHub.info(); },
      .setDuplexPort
      = [&duplexHub](std::size_t ordinal, std::uint16_t port) { duplexHub.setPort(ordinal, port); },
      .setDuplexEnabled
      = [&duplexHub](std::size_t ordinal, bool enabled) { duplexHub.setEnabled(ordinal, enabled); },
      .setDuplexBasePort = [&duplexHub](std::uint16_t port) { duplexHub.setBasePort(port); },
      .setLogDir         = [&logFiles](std::string const& dir) { logFiles.changeDir(dir); },
      .setLogFileEnabled = [&logFiles](bool enabled) { logFiles.setEnabled(enabled); }});

    // declared after all users of the context: joined first on destruction so no asio handler runs while the servers
    // above are torn down
    uc_log::printer::AsioContextRunner asioRunner{asioContext};

    uc_log::printer::TimeDelayedQueue queue{
      [](uc_log::printer::LogEntry const& entry) { return entry.ucTime; },
      [](uc_log::printer::LogEntry const& entry) { return entry.channel.channel; },
      [&logFiles, &controlServer, &gui](std::chrono::system_clock::time_point recvTime,
                                        uc_log::printer::LogEntry const&      entry) {
          logFiles.add(recvTime, entry);
          if(controlServer) {
              using uc_log::printer::control::Stream;
              controlServer->publish(uc_log::printer::toLogLine(recvTime, entry));
              if(controlServer->wants(Stream::metrics)) {
                  for(auto const& sample : uc_log::printer::toMetricSamples(recvTime, entry)) {
                      controlServer->publish(sample);
                  }
              }
          }
          if(gui) { gui->add(recvTime, entry); }
      }};

    // the session's catalog: loaded at its start, before its decoder runs (decode thread) - never changed under it
    std::shared_ptr<uc_log::printer::Catalog const> catalog;
    // LossPolicy::Counted firmware: its lost-records counted here, its ledger read by watchLossLedger
    uc_log::printer::TargetLoss targetLoss;
    std::mutex                  targetLossMutex;
    std::atomic<std::uint64_t>  targetLostTotal{0};
    uc_log::printer::MapFacts   mapFacts{o.mapFile};

    uc_log::printer::FirmwareCheck firmwareCheck;
    firmwareCheck.uncachedOffset = o.flashUncachedOffset;
    uc_log::printer::FirmwareCheck::EmitF const sayFirmwareCheck
      = [&model, &messages](uc_log::printer::control::FirmwareState state,
                            std::string const&                      text,
                            bool                                    isError) {
            model.firmwareChecked(state);
            if(isError) {
                messages.error(text);
            } else {
                messages.status(text);
            }
        };

    uc_log::printer::ReaderOptions readerOptions{.preResetCommands = o.preResetCommands,
                                                 .stall            = o.stallSignature,
                                                 .park             = o.parkSignature,
                                                 .flashReady       = o.flashReady,
                                                 .flashWindow      = o.flashWindow,
                                                 .beforeConnect    = termsAcceptor(messages)};
    // UC_LOG_RTT_VIA_DLL=1: RTT through the J-Link DLL's own RTT - the max-throughput mode, with its known weaknesses
    // (a session end drops the DLL's unread bytes unannounced; an unannounced reset can replay); see --help
    if(auto const* via = uc_log::printer::environment("UC_LOG_RTT_VIA_DLL");
       via != nullptr && std::string_view{via} == "1")
    {
        readerOptions.directRtt = false;
        messages.tool(
          "UC_LOG_RTT_VIA_DLL=1: RTT through the J-Link DLL (max throughput; a stop or reset can "
          "drop "
          "or repeat lines without a message)");
    }
    // UC_LOG_STALL_FIRST_STEP=2|3: a test switch - a stall starts the recovery ladder at that step (2: SYSRESETREQ in the
    // session, 3: the reset line), so the later steps can be checked on a board without a stall that outlasts step 1
    if(auto const* step = uc_log::printer::environment("UC_LOG_STALL_FIRST_STEP");
       step != nullptr && (std::string_view{step} == "2" || std::string_view{step} == "3"))
    {
        readerOptions.stallFirstStep = step[0] - '0';
        messages.tool(
          fmt::format("UC_LOG_STALL_FIRST_STEP={}: a stall starts the recovery ladder at step {} "
                      "(a test switch)",
                      step,
                      step));
    }
    uc_log::printer::ReaderSink readerSink{
      .messages = messages,
      // the decode thread, while the session's catalog stands
      .entry =
        [&queue, &catalog, &model, &messages, &targetLoss, &targetLossMutex, &targetLostTotal](
          std::size_t                           channel,
          std::string_view                      msg,
          std::optional<remote_fmt::catalog_id> id) {
            uc_log::printer::LogEntry entry{channel, msg, catalog->signatures.find(id)};
            if(auto const n = uc_log::printer::lostRecordCount(entry)) {
                std::string text;
                {
                    std::scoped_lock<std::mutex> const lock{targetLossMutex};
                    text = targetLoss.lostRecord(channel, *n, entry.ucTime.time);
                }
                targetLostTotal += *n;
                model.targetRecordsLost(*n);
                messages.error(text);
            }
            queue.append(std::move(entry));
        },
      .unreadBoots =
        [&queue](std::uint32_t unread, std::uint32_t from, std::uint32_t to) {
            queue.append(uc_log::printer::unreadBootsRow(unread, from, to));
        },
      .bootCountRestart = [&queue]() { queue.append(uc_log::printer::bootCountRestartRow()); }};

    uc_log::printer::ImageSource image{
      .block =
        [&mapFacts, &messages]() {
            auto const result = mapFacts.get()->block;
            if(!result.has_value()) { messages.fatal(result.error()); }
            return result.value_or(uc_log::printer::RttBlockInfo{});
        },
      .hexFile = [&o]() { return o.hexFile; },
      .catalog =
        [&o, &messages, &catalog]() {
            auto loaded = uc_log::printer::loadCatalog(o.stringConstantsFile);
            if(!loaded) { messages.fatal(loaded.error()); }
            catalog = loaded ? std::make_shared<uc_log::printer::Catalog const>(std::move(*loaded))
                             : std::make_shared<uc_log::printer::Catalog const>();
            return catalog->strings;
        },
      // the image's own word on its frames (the session's catalog, loaded just before)
      .frameCheck =
        [&catalog, &messages] {
            auto const check = catalog ? catalog->frameCheck : remote_fmt::FrameCheck::either;
            if(check != remote_fmt::FrameCheck::either) {
                messages.tool(
                  check == remote_fmt::FrameCheck::required
                    ? "the image checks its frames (CRC-8): unchecked frames are refused"
                    : "the image has no frame check: checked frames are refused");
            }
            return check;
        },
      // a firmware that announces its own resets (Kvasir_SDK Runtime/AnnouncedReset.hpp) has the block in its map
      .announcedReset = [&mapFacts]() { return mapFacts.get()->announce; },
      // the image's RTT epoch (detail/RttEpoch.hpp): fences the printer's ring accesses against a re-init
      .rttEpoch = [&mapFacts]() { return mapFacts.get()->epoch; },
      // the image's count of boots that logged (BootLog.hpp): the boots nobody read are counted by it, exactly
      .loggedBoots = [&mapFacts]() { return mapFacts.get()->loggedBoots; },
      .ramImage    = {},
      // decode only with the strings of the image the target runs (FirmwareCheck::gate)
      .decodeGate =
        [&](uc_log::printer::DirectMemoryRead const& read) {
            return firmwareCheck.gate(o.hexFile, o.ramImage, read, sayFirmwareCheck);
        },
      .onSessionStart =
        [&](uc_log::printer::DirectMemoryRead const& read) {
            // the last session's lines out before this one's: after a reset the target's times start again at 0 and
            // would sort in front of the old boot's last lines in the merge
            queue.flush();
            firmwareCheck.check(o.hexFile, o.ramImage, read, sayFirmwareCheck);
        }};
    if(o.ramImage) {
        // asked before each flash and reset: a rebuild may have moved the table
        image.ramImage = [&o, &mapFacts]() {
            return uc_log::printer::ramImageStart(*mapFacts.get()->map, o.hexFile);
        };
        messages.tool(fmt::format(
          "RAM image: flash and reset load {} and start it at its vector table, no reset "
          "after the download",
          o.hexFile));
    }

    uc_log::printer::DuplexBridge duplexBridge{
      .configure =
        [&duplexHub](std::vector<uc_log::printer::DuplexChannelDesc> const& descs) {
            duplexHub.configure(descs);
        },
      .sendToClient
      = [&duplexHub](std::size_t                ordinal,
                     std::span<std::byte const> data) { duplexHub.sendToClient(ordinal, data); },
      .peekFromClient
      = [&duplexHub](std::size_t          ordinal,
                     std::span<std::byte> out) { return duplexHub.peekFromClient(ordinal, out); },
      .consumeFromClient
      = [&duplexHub](std::size_t ordinal,
                     std::size_t n) { duplexHub.consumeFromClient(ordinal, n); }};

    JLinkRttReader rttReader{
      uc_log::printer::ReaderConnection{.host   = o.host,
                                        .device = o.device,
                                        .speed  = o.speed,
                                        .probe  = o.probe},
      std::move(readerSink),
      std::move(image),
      std::move(readerOptions),
      std::move(duplexBridge)
    };

    uc_log::printer::LossLedgerWatch const ledgerWatch{
      mapFacts,
      rttReader,
      targetLoss,
      targetLossMutex,
      messages,
      [&model](bool counted) { model.targetLossCounted(counted); }};

    // cleared again before the reader goes: its requests run on the reader
    if(controlServer) {
        controlServer->setTarget(controlTarget(
          rttReader,
          [&rttReader, &firmwareCheck, &messages, &controlServer, &targetLostTotal, startedUs] {
              return uc_log::printer::control::StatusAnswer{
                .running     = rttReader.getStatus().isRunning != 0,
                .halted      = rttReader.isHalted(),
                .flashing    = rttReader.isFlashing(),
                .sessions    = static_cast<std::uint32_t>(rttReader.sessionCount()),
                .firmware    = firmwareCheck.get(),
                .errors      = messages.totalErrors(),
                .logSeq      = controlServer->logSeq(),
                .startedUs   = startedUs,
                .ramImage    = rttReader.isRamImage(),
                .targetLost  = targetLostTotal.load(),
                .rttBytes    = rttReader.rttBytes(),
                .probeSerial = rttReader.probeSerial()};
          },
          messages,
          mapFacts,
          o.mapFile));
    }
    OnScopeExit const releaseTarget{[&controlServer] {
        if(controlServer) { controlServer->clearTarget(); }
    }};

    if(!o.disableUi) {
        gui->setMapSource([&mapFacts] { return mapFacts.get()->map; });
        auto const result = gui->run(rttReader, o.buildCommand, o.host, o.probe);
        armExitDeadline();
        return result;
    }
    static std::atomic<bool> shutdownRequested{false};
    std::signal(SIGINT, [](int signal) {
        if(signal == SIGINT) { shutdownRequested = true; }
    });
    while(!shutdownRequested) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    armExitDeadline();
    return 0;
}
