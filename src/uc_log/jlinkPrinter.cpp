#include "jlink/JLink.hpp"

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/FTXUIGui.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/TimeDelayedQueue.hpp"
#include "uc_log/detail/AnnouncedReset.hpp"
#include "uc_log/detail/ControlEvents.hpp"
#include "uc_log/detail/ControlServer.hpp"
#include "uc_log/detail/DuplexChannelServer.hpp"
#include "uc_log/detail/HexImage.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/LogFormat.hpp"
#include "uc_log/detail/RamImage.hpp"
#include "uc_log/detail/RttChannelMap.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"
#include "uc_log/metric_utils.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>
// clang-format off
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#  pragma clang diagnostic ignored "-Wexit-time-destructors"
#  pragma clang diagnostic ignored "-Wglobal-constructors"
#  pragma clang diagnostic ignored "-Wextra-semi-stmt"
#  pragma clang diagnostic ignored "-Wdeprecated-copy-with-dtor"
#  pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
#include <cxxopts.hpp>
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
// clang-format on
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <stop_token>
#include <thread>

namespace {
std::expected<RttBlockInfo,
              std::string>
parseMapFileForControlBlockInfo(std::filesystem::path const& mapFile) {
    static constexpr std::string_view needle{"::rttControlBlock"};
    static constexpr std::uint32_t    controlBlockHeaderSize{24};   // 16 ID + 4 numUp + 4 numDown
    static constexpr std::uint32_t    bufferControlBlockSize{24};   // per RTT spec

    std::ifstream file{mapFile};
    if(!file) { return std::unexpected(fmt::format("failed to open map file: {:?}", mapFile)); }

    auto const  fileSize = std::filesystem::file_size(mapFile);
    std::string content;
    content.resize(fileSize);
    file.read(content.data(), std::ssize(content));

    // Three map-file shapes: lld puts "<vma> <lma> <size> <align> <name>" on one symbol line;
    // GNU ld puts only the address on the symbol line, with the size on the preceding
    // input-section line; GNU ld with LTO localises the symbol, leaving only the input-section
    // line with the mangled name, followed by address and size.
    auto lines = content | std::views::split('\n')
               | std::views::transform([](auto&& rng) { return std::string_view{rng}; });

    auto const parseHex = [](std::string_view sv, std::uint32_t& out) -> std::string_view {
        auto const begin = std::find_if_not(sv.begin(), sv.end(), [](char c) { return c == ' '; });
        std::string_view rest{begin, sv.end()};
        if(rest.starts_with("0x")) { rest.remove_prefix(2); }
        auto const* end = std::to_address(rest.end());
        auto [ptr, ec]  = std::from_chars(rest.data(), end, out, 16);
        if(ec != std::errc{}) { return {}; }
        return std::string_view{ptr, end};
    };
    auto const plausible = [&](std::uint32_t size) {
        return size >= controlBlockHeaderSize
            && (size - controlBlockHeaderSize) % bufferControlBlockSize == 0;
    };

    static constexpr std::string_view mangledNeedle{"rttControlBlockE"};

    std::vector<std::string_view> all{lines.begin(), lines.end()};
    std::vector<std::string_view> candidates{};
    for(std::size_t i = 0; i < all.size(); ++i) {
        auto const    line = all[i];
        std::uint32_t probe{};
        // lld's input-section line also contains the mangled name but always starts with an
        // address; the GNU ld section line never does. Reading it as the GNU ld shape would
        // mistake the LMA for the size.
        bool const startsWithAddress = !parseHex(line, probe).empty();
        bool const demangled         = line.contains(needle);
        bool const section = !demangled && !startsWithAddress && line.contains(mangledNeedle);
        if(!demangled && !section) { continue; }
        candidates.push_back(line);

        std::uint32_t address{};
        std::uint32_t size{};
        if(section) {
            // "<name> <addr> <size> <file>" on one line, or the numbers on the next
            auto const nameEnd = line.find(mangledNeedle) + mangledNeedle.size();
            auto       rest    = parseHex(line.substr(nameEnd), address);
            if(rest.empty() && i + 1 < all.size()) { rest = parseHex(all[i + 1], address); }
            if(!rest.empty() && !parseHex(rest, size).empty() && plausible(size)) {
                return RttBlockInfo{address,
                                    (size - controlBlockHeaderSize) / bufferControlBlockSize};
            }
            continue;
        }
        auto rest = parseHex(line, address);
        if(rest.empty()) { continue; }
        // lld: two more hex columns, the third is the size
        std::uint32_t lma{};
        auto          r2 = parseHex(rest, lma);
        if(!r2.empty() && !parseHex(r2, size).empty() && plausible(size)) {
            return RttBlockInfo{address, (size - controlBlockHeaderSize) / bufferControlBlockSize};
        }
        // GNU ld without LTO: "<addr> <size> <file>" on the line before, same address
        std::uint32_t prevAddress{};
        if(i > 0) {
            auto p1 = parseHex(all[i - 1], prevAddress);
            if(!p1.empty() && prevAddress == address && !parseHex(p1, size).empty()
               && plausible(size))
            {
                return RttBlockInfo{address,
                                    (size - controlBlockHeaderSize) / bufferControlBlockSize};
            }
        }
    }
    auto addressLines = candidates;

    return std::unexpected(
      fmt::format("failed to parse address from file: {:?} lines: {::?}", mapFile, addressLines));
}

struct LogFilePrinter {
    std::function<void(std::string_view)>                errorMessagef;
    std::function<void(LogFileStatus, std::string_view)> statusChangef;
    std::filesystem::path                                logFilePath;
    std::ofstream                                        logFile;
    bool                                                 errorShown{false};
    bool                                                 logFileEnabled{true};
    bool                                                 logFileDirty{false};
    std::mutex                                           mutex;
    // own mutex: add() reports errors through the gui (-> addStatus) while holding `mutex`
    std::filesystem::path                                      statusFilePath;
    std::ofstream                                              statusFile;
    std::mutex                                                 statusMutex;
    std::deque<uc_log::control::StatusMessage>                 statusLines;
    std::size_t                                                statusErrors{};
    std::function<void(uc_log::control::StatusMessage const&)> statusListener;
    static constexpr std::size_t                               StatusLinesKept = 1000;
    static constexpr auto FlushPeriod = std::chrono::milliseconds{100};

    // scripts read the .rttlog while the printer runs: a line is on disk FlushPeriod later at most
    std::jthread flusher{[this](std::stop_token const& stop) {
        while(!stop.stop_requested()) {
            std::this_thread::sleep_for(FlushPeriod);
            std::lock_guard<std::mutex> const lock{mutex};
            if(logFileDirty) {
                logFile.flush();
                logFileDirty = false;
            }
        }
    }};

    LogFilePrinter(uc_log::FTXUIGui::Gui& gui UC_LOG_LIFETIMEBOUND,
                   std::string const&         logDir)
      : errorMessagef{[&gui](auto const& m) { gui.errorMessage(m); }}
      , statusChangef{[&gui](LogFileStatus    s,
                             std::string_view p) { gui.setLogFileStatus(s, p); }} {
        gui.setMessageSink(
          [this](std::string_view level, std::string_view msg) { addStatus(level, msg); });
        openFileUnlocked(logDir);
    }

    void addStatus(std::string_view level,
                   std::string_view msg) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        auto                              message
          = uc_log::detail::toStatusMessage(std::chrono::system_clock::now(), level, msg);
        if(level != "status" && level != "tool") { ++statusErrors; }
        if(statusFile) {
            statusFile << uc_log::detail::statusFileLine(message) << '\n';
            statusFile.flush();
        }
        // under the lock: stream order = file order
        if(statusListener) { statusListener(message); }
        statusLines.push_back(std::move(message));
        if(statusLines.size() > StatusLinesKept) { statusLines.pop_front(); }
    }

    void setStatusListener(std::function<void(uc_log::control::StatusMessage const&)> f) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        statusListener = std::move(f);
    }

    std::vector<uc_log::control::StatusMessage> newestStatus(std::size_t count) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        auto const                        n = std::min(count, statusLines.size());
        return {statusLines.end() - static_cast<std::ptrdiff_t>(n), statusLines.end()};
    }

    std::size_t statusErrorCount() {
        std::lock_guard<std::mutex> const lock{statusMutex};
        return statusErrors;
    }

    void changeDir(std::string const& newDir) {
        std::lock_guard<std::mutex> const lock{mutex};
        openFileUnlocked(newDir);
    }

    void setEnabled(bool enabled) {
        std::lock_guard<std::mutex> const lock{mutex};
        logFileEnabled = enabled;
    }

    void add(std::chrono::system_clock::time_point recv_time,
             uc_log::detail::LogEntry const&       entry) {
        std::lock_guard<std::mutex> const lock{mutex};
        if(!logFileEnabled) { return; }
        if(logFile) {
            uc_log::detail::logformat::writeEntry(logFile, recv_time, entry);
            logFileDirty = true;   // flushed by `flusher`, not per line: a syscall per line
        } else {
            if(!errorShown) {
                errorMessagef(fmt::format("error writing logFile: {:?}", logFilePath));
                errorShown = true;
            }
        }
    }

private:
    void openFileUnlocked(std::string const& dir) {
        logFile.close();
        errorShown = false;
        logFilePath
          = std::filesystem::path{dir}
          / fmt::format("{}.rttlog",
                        uc_log::detail::logformat::toIso8601Utc(std::chrono::system_clock::now()));
        logFile.open(logFilePath);
        {
            std::lock_guard<std::mutex> const lock{statusMutex};
            statusFile.close();
            statusFile.clear();
            statusFilePath = logFilePath;
            statusFilePath.replace_extension(".status.log");
            statusFile.open(statusFilePath);
        }
        if(!logFile.is_open()) {
            errorMessagef(fmt::format("failed to open logfile: {:?}", logFilePath));
            if(statusChangef) { statusChangef(LogFileStatus::Error, logFilePath.string()); }
        } else {
            uc_log::detail::logformat::writeHeader(logFile);
            if(statusChangef) { statusChangef(LogFileStatus::Active, logFilePath.string()); }
        }
    }
};

// Does the board run the build whose strings decode the log? Checked at each session start.
struct FirmwareCheck {
    using State = uc_log::control::FirmwareState;
    std::mutex                     mutex;
    uc_log::control::FirmwareCheck summary{};

    /// `ramImage`: the image lives in RAM, which changes as it runs - nothing to compare, and
    /// that is no error (emit's third argument says whether it is one).
    template<typename ReadF,
             typename EmitF>
    void run(std::string const& hexFile,
             bool               ramImage,
             ReadF&&            read,
             EmitF&&            emit) {
        using uc_log::detail::ImageCheck;
        std::string                    text;
        bool                           isError = true;
        uc_log::control::FirmwareCheck state{};
        std::ifstream                  file{hexFile};
        auto const image = file ? uc_log::detail::parseIntelHex(file)
                                : std::unexpected{fmt::format("cannot open {:?}", hexFile)};
        if(!image) {
            text = fmt::format("firmware not checked: {}", image.error());
        } else if(ramImage) {
            auto const crc = uc_log::detail::imageCrc(*image);
            state          = {.state = State::unchecked, .build = crc};
            isError        = false;
            text           = fmt::format(
              "firmware not checked: {} is a RAM image (build {:08x}), and RAM "
              "changes as it runs",
              hexFile,
              crc);
        } else {
            auto const      crc   = uc_log::detail::imageCrc(*image);
            auto const      check = uc_log::detail::compareImage(*image, read);
            std::error_code ec;
            auto const      written = std::filesystem::last_write_time(hexFile, ec);
            auto const      built
              = ec ? std::string{"?"}
                   : uc_log::detail::logformat::toIso8601Utc(
                       std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                         std::chrono::file_clock::to_sys(written)));
            if(check.result == ImageCheck::Result::match) {
                state   = {.state = State::match, .build = crc};
                isError = false;
                text
                  = fmt::format("firmware matches {} (build {:08x}, {} bytes compared, built {})",
                                hexFile,
                                crc,
                                check.comparedBytes,
                                built);
            } else if(check.result == ImageCheck::Result::different) {
                state = {.state = State::different, .build = crc};
                text  = fmt::format(
                  "⚠ the target does NOT run {} (build {:08x}, built {}): first "
                  "difference at {:#010x} - this log is decoded with the wrong "
                  "strings; flash the target",
                  hexFile,
                  crc,
                  built,
                  check.firstDifference);
            } else {
                state = {.state = State::unchecked, .build = crc};
                text  = fmt::format("firmware not checked: {}", check.error);
            }
        }
        {
            std::lock_guard<std::mutex> const lock{mutex};
            summary = state;
        }
        emit(checkState(state), text, isError);
    }

    uc_log::control::FirmwareCheck get() {
        std::lock_guard<std::mutex> const lock{mutex};
        return summary;
    }

private:
    static uc_log::FTXUIGui::FirmwareState checkState(uc_log::control::FirmwareCheck const& state) {
        using S = uc_log::FTXUIGui::FirmwareState;
        switch(state.state) {
        case State::match:     return S::match;
        case State::different: return S::different;
        case State::unchecked: return S::unchecked;
        }
        return S::unchecked;
    }
};

struct OnScopeExit {
    std::function<void()> f;

    ~OnScopeExit() {
        if(f) { f(); }
    }
};

}   // namespace

int main(int    argc,
         char** argv) {
    std::uint32_t                   speed{};
    std::string                     device{};
    std::string                     mapFile{};
    std::string                     hexFile{};
    std::string                     stringConstantsFile{};
    std::string                     host{};
    std::string                     probe{};
    std::string                     logDir{};
    std::string                     buildCommand{};
    std::string                     bindAddressString{};
    std::uint16_t                   controlPort{};
    std::uint16_t                   duplexBasePort{};
    std::string                     controlSocket{};
    std::string                     transport{};
    bool                            disableUi{false};
    bool                            ramImage{false};
    std::vector<JLink::MemoryWrite> preResetCommands{};
    std::string                     logFilterFile{};
    std::size_t                     controlHistoryMb{};
    auto const startedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

    cxxopts::Options options("uc_log_printer");
    try {
        options.add_options()(
          "transport",
          "control server and duplex channels: 'unix' (control.sock, duplex.<n>.sock in the log "
          "directory) or 'tcp' (control_port, duplex_base_port + n on bind_address)",
          cxxopts::value<std::string>()->default_value("unix"))(
          "duplex_base_port",
          "first tcp port for duplex channels (--transport tcp)",
          cxxopts::value<std::uint16_t>()->default_value("34600"))(
          "control_port",
          "tcp port of the control server (--transport tcp)",
          cxxopts::value<std::uint16_t>()->default_value("34565"))(
          "control_history_mb",
          "log history the control server keeps for subscribers asking for the past (MiB)",
          cxxopts::value<std::size_t>()->default_value("128"))(
          "metrics_port",
          "deprecated: the metrics are a stream of the control server now; taken as "
          "--control_port when that is not given",
          cxxopts::value<std::uint16_t>())("speed", "swd speed", cxxopts::value<std::uint32_t>())(
          "device",
          "mpu device",
          cxxopts::value<std::string>())("build_command",
                                         "build command",
                                         cxxopts::value<std::string>())(
          "map_file",
          "map file",
          cxxopts::value<std::string>())("hex_file", "hex file", cxxopts::value<std::string>())(
          "string_constants_file",
          "string constants map file",
          cxxopts::value<std::string>())("log_dir",
                                         "log file directory",
                                         cxxopts::value<std::string>())(
          "host",
          "jlink host",
          cxxopts::value<std::string>()->default_value(""))(
          "probe",
          "jlink by serial number or nickname, on usb or on the network; required when "
          "more than one is on usb",
          cxxopts::value<std::string>()->default_value(""))(
          "bind_address",
          "address the tcp servers (control + duplex) bind to; the duplex ports give raw "
          "unauthenticated access to the target, so anything but loopback exposes that "
          "to the network",
          cxxopts::value<std::string>()->default_value("127.0.0.1"))(
          "log_filter",
          "the uc_log_filter.txt the firmware was compiled with (LogFilter.hpp): shown in the "
          "Filter tab's module tree, and a module it compiled out is named there",
          cxxopts::value<std::string>()->default_value(""))(
          "pre_reset_command",
          "J-Link Commander line written to the target before every reset and download, "
          "repeatable, in order; only 'w4 <address> <value>'. What a chip needs there (the RP "
          "chips park core 1) comes from its package's TARGET_JLINK_CONNECT_COMMANDS",
          cxxopts::value<std::vector<std::string>>())(
          "ram_image",
          "the image lives in RAM (a Kvasir RAM_ONLY target): a reset would boot flash, so flash "
          "and reset load the hex file and start it at its vector table (VTOR, MSP, xPSR, PC from "
          "the map file's _LINKER_vectors_start_ and the hex), as the target's J-Link script does")(
          "control_socket",
          "unix domain socket of the control server (--transport unix; JSON lines, "
          "detail/ControlProtocol.hpp): 'auto' = control.sock in the log directory, where "
          "tools/uc_log_client.py looks, 'off' = no control server",
          cxxopts::value<std::string>()->default_value(
            "auto"))("disable_ui", "disable ui and just log to file and the control server");
        auto const result = options.parse(argc, argv);
        controlPort       = result["control_port"].as<std::uint16_t>();
        controlHistoryMb  = result["control_history_mb"].as<std::size_t>();
        if(result.count("metrics_port") > 0 && result.count("control_port") == 0) {
            controlPort = result["metrics_port"].as<std::uint16_t>();
        }
        duplexBasePort      = result["duplex_base_port"].as<std::uint16_t>();
        speed               = result["speed"].as<std::uint32_t>();
        device              = result["device"].as<std::string>();
        buildCommand        = result["build_command"].as<std::string>();
        mapFile             = result["map_file"].as<std::string>();
        hexFile             = result["hex_file"].as<std::string>();
        stringConstantsFile = result["string_constants_file"].as<std::string>();
        logDir              = result["log_dir"].as<std::string>();
        host                = result["host"].as<std::string>();
        probe               = result["probe"].as<std::string>();
        bindAddressString   = result["bind_address"].as<std::string>();
        disableUi           = result.count("disable_ui") > 0;
        ramImage            = result.count("ram_image") > 0;
        controlSocket       = result["control_socket"].as<std::string>();
        transport           = result["transport"].as<std::string>();
        logFilterFile       = result["log_filter"].as<std::string>();
        if(result.count("pre_reset_command") > 0) {
            for(auto const& line : result["pre_reset_command"].as<std::vector<std::string>>()) {
                preResetCommands.push_back(JLink::parseCommand(line));
            }
        }
        if(transport != "unix" && transport != "tcp") {
            fmt::print(stderr, "Error: --transport is 'unix' or 'tcp', not {:?}\n", transport);
            return 1;
        }
    } catch(cxxopts::exceptions::exception const& e) {
        fmt::print(stderr, "Error: {}\n{}\n", e.what(), options.help());
        return 1;
    } catch(std::runtime_error const& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }

    boost::asio::ip::address bindAddress;
    try {
        bindAddress = boost::asio::ip::make_address(bindAddressString);
    } catch(std::exception const& e) {
        fmt::print(stderr, "Error: invalid bind_address {:?}: {}\n", bindAddressString, e.what());
        return 1;
    }

    uc_log::FTXUIGui::Gui gui{};
    gui.setEchoToStderr(disableUi);

    if(!logFilterFile.empty()) {
        std::ifstream     stream{logFilterFile};
        std::string const text{std::istreambuf_iterator<char>{stream}, {}};
        auto const        parsed = uc_log::detail::parseFilter(text);
        if(!stream.is_open()) {
            gui.errorMessage(fmt::format("log filter {}: cannot open it", logFilterFile));
        } else if(!parsed.error.empty()) {
            gui.errorMessage(
              fmt::format("log filter {}:{}: {}", logFilterFile, parsed.line, parsed.error));
        } else {
            gui.setCompiledFilter(parsed.table);
            gui.statusMessage(fmt::format("log filter {}: {} module rule(s), every line >= {}",
                                          logFilterFile,
                                          parsed.table.count,
                                          uc_log::detail::filterLevelName(parsed.table.global)));
        }
    }
    gui.setNetworkBindAddress(bindAddressString);
    LogFilePrinter              logFilePrinter{gui, logDir};
    uc_log::detail::AsioContext asioContext;
    bool const                  unixTransport    = transport == "unix";
    auto const                  controlSocketFor = [&controlSocket, &logDir](bool unix) {
        if(!unix) { return std::filesystem::path{}; }
        return controlSocket == "auto" ? uc_log::detail::controlSocketPath(logDir)
                                       : std::filesystem::path{controlSocket};
    };

    // a failed bind does not stop logging
    std::optional<uc_log::detail::ControlServer> controlServer;
    if(controlSocket != "off" && !controlSocket.empty()) {
        auto const path = controlSocketFor(unixTransport);
        controlServer.emplace(
          bindAddress,
          controlPort,
          path,
          [&gui](std::string_view msg) {
              gui.toolErrorMessage(fmt::format("control server: {}", msg));
          },
          [&gui](TcpPortStatus s, std::uint16_t p) { gui.setTcpPortStatus(s, p); });
        controlServer->setHistoryLimit(controlHistoryMb * 1024 * 1024);
        gui.setControlSocketPath(path.native());
        gui.toolStatusMessage(
          path.empty() ? fmt::format("control server on {}:{}", bindAddressString, controlPort)
                       : fmt::format("control socket {}", path.native()));
        logFilePrinter.setStatusListener(
          [&controlServer](uc_log::control::StatusMessage const& m) { controlServer->publish(m); });
    }
    // before the server goes: nothing publishes into it while it is torn down
    OnScopeExit const stopStatusStream{[&logFilePrinter] { logFilePrinter.setStatusListener({}); }};

    gui.setOnTcpPortChange([&controlServer](std::uint16_t newPort) {
        if(controlServer) { controlServer->restart(newPort); }
    });
    gui.setTcpClientCountGetter([&controlServer]() -> std::size_t {
        return controlServer ? controlServer->getClientCount() : 0;
    });
    gui.setOnLogDirChange(
      [&logFilePrinter](std::string const& newDir) { logFilePrinter.changeDir(newDir); });
    gui.setOnLogFileEnable([&logFilePrinter](bool enabled) { logFilePrinter.setEnabled(enabled); });
    gui.setOnTcpEnable([&controlServer, controlPort](bool enabled) {
        if(!controlServer) { return; }
        if(enabled) {
            auto const current = controlServer->getPort();
            controlServer->restart(current != 0 ? current : controlPort);
        } else {
            controlServer->stop();
        }
    });

    uc_log::detail::DuplexChannelHub duplexHub{
      asioContext.ioc,
      bindAddress,
      duplexBasePort,
      [&gui](std::string_view msg) { gui.errorMessage(msg); },
      [&gui](std::string_view msg) { gui.statusMessage(msg); },
      [&gui]() { gui.triggerRedraw(); },
      unixTransport ? std::filesystem::path{logDir} : std::filesystem::path{}};
    gui.setDuplexInfoGetter([&duplexHub]() { return duplexHub.info(); });
    gui.setOnDuplexPortChange([&duplexHub](std::size_t ordinal, std::uint16_t newPort) {
        duplexHub.setPort(ordinal, newPort);
    });
    gui.setOnDuplexEnable(
      [&duplexHub](std::size_t ordinal, bool enabled) { duplexHub.setEnabled(ordinal, enabled); });
    gui.setOnDuplexBasePortChange(
      [&duplexHub](std::uint16_t newBasePort) { duplexHub.setBasePort(newBasePort); });
    gui.setDuplexBasePort(duplexBasePort);

    gui.setOnTransportChange(
      [&gui, &controlServer, &controlSocketFor, &duplexHub, &logDir](bool unix) {
          auto const controlPath = controlSocketFor(unix);
          if(controlServer) { controlServer->setSocketPath(controlPath); }
          duplexHub.setSocketDir(unix ? std::filesystem::path{logDir} : std::filesystem::path{});
          gui.setControlSocketPath(controlServer ? controlPath.native() : std::string{});
      });

    // let the operator rebind every socket at runtime; returns false on an unparsable
    // address so the gui can show it without applying anything
    gui.setOnNetworkBindAddressChange([&controlServer, &duplexHub](std::string const& s) {
        boost::system::error_code ec;
        auto const                address = boost::asio::ip::make_address(s, ec);
        if(ec) { return false; }
        if(controlServer) { controlServer->setBindAddress(address); }
        duplexHub.setBindAddress(address);
        return true;
    });

    // declared after all users of the context: joined first on destruction so no asio
    // handler runs while the servers above are torn down
    uc_log::detail::AsioContextRunner asioRunner{asioContext};

    TimeDelayedQueue queue{
      [](uc_log::detail::LogEntry const& entry) { return entry.ucTime; },
      [](uc_log::detail::LogEntry const& entry) { return entry.channel.channel; },
      [&logFilePrinter, &controlServer, &gui](std::chrono::system_clock::time_point recv_time,
                                              uc_log::detail::LogEntry const&       entry) {
          logFilePrinter.add(recv_time, entry);
          if(controlServer) {
              using uc_log::control::Stream;
              controlServer->publish(uc_log::detail::toLogLine(recv_time, entry));
              if(controlServer->wants(Stream::metrics)) {
                  for(auto const& sample : uc_log::detail::toMetricSamples(recv_time, entry)) {
                      controlServer->publish(sample);
                  }
              }
          }
          gui.add(recv_time, entry);
      }};

    uc_log::detail::SignatureTable signatures;

    JLinkRttReader rttReader{
      host,
      device,
      speed,
      probe,
      [&mapFile, &gui]() {
          auto const result = parseMapFileForControlBlockInfo(mapFile);
          if(!result.has_value()) { gui.fatalError(result.error()); }
          return result.value_or(RttBlockInfo{});
                          },
      [&hexFile]() { return hexFile; },
      [&stringConstantsFile, &gui, &signatures]() {
          auto const result = remote_fmt::parseStringConstantsFromJsonFile(stringConstantsFile);
          if(!result.has_value()) { gui.fatalError(result.error()); }
          auto table = uc_log::detail::SignatureTable::fromJsonFile(stringConstantsFile);
          if(table.has_value()) {
              if(table->empty()) {
                  gui.errorMessage("no call sites in " + stringConstantsFile
                                   + ": every function shows as \"?\" (rebuild the target)");
              }
              signatures = std::move(*table);
          } else {
              gui.errorMessage(table.error());
              signatures = {};
          }
          return result.value_or({});
                          },
      // the same thread as the one above
      [&queue, &signatures](std::size_t                           channel,
                            std::string_view                      msg,
                            std::optional<remote_fmt::catalog_id> id) {
          queue.append(uc_log::detail::LogEntry{channel, msg, signatures.find(id)});
                          },
      [&gui](std::string_view msg) { gui.statusMessage(msg); },
      [&gui](std::string_view msg) { gui.errorMessage(msg); },
      [&gui](std::string_view msg) { gui.toolStatusMessage(msg); },
      [&gui](std::string_view msg) { gui.toolErrorMessage(msg); },
      uc_log::detail::DuplexBridge{
                          [&duplexHub](std::vector<uc_log::detail::DuplexChannelDesc> const& descs) {
            duplexHub.configure(descs);
        }, [&duplexHub](std::size_t ordinal, std::span<std::byte const> data) {
            duplexHub.sendToClient(ordinal, data);
        }, [&duplexHub](std::size_t ordinal, std::span<std::byte> out) {
            return duplexHub.peekFromClient(ordinal, out);
        }, [&duplexHub](std::size_t ordinal, std::size_t n) {
            duplexHub.consumeFromClient(ordinal, n);
        }}
    };

    rttReader.setPreResetCommands(preResetCommands);
    // a firmware that announces its own resets (Kvasir_SDK Util/AnnouncedReset.hpp) has the block
    // in its map; asked at every session start, a rebuild may add or move it
    rttReader.setAnnouncedReset([&mapFile]() -> std::optional<std::uint32_t> {
        auto const address = uc_log::detail::announced_reset::addressFromMap(mapFile);
        if(!address) { return std::nullopt; }
        return *address;
    });
    if(ramImage) {
        // asked before each flash and reset: a rebuild may have moved the table
        rttReader.setRamImage(
          [&mapFile, &hexFile]() { return uc_log::detail::ramImageStart(mapFile, hexFile); });
        gui.toolStatusMessage(
          fmt::format("RAM image: flash and reset load {} and start it at its "
                      "vector table, no reset after the download",
                      hexFile));
    }

    FirmwareCheck firmwareCheck;
    rttReader.setOnSessionStart([&](JLinkRttReader::DirectMemoryRead const& read) {
        firmwareCheck.run(
          hexFile,
          ramImage,
          read,
          [&gui](uc_log::FTXUIGui::FirmwareState state, std::string const& text, bool isError) {
              gui.firmwareChecked(state);
              if(isError) {
                  gui.errorMessage(text);
              } else {
                  gui.statusMessage(text);
              }
          });
    });

    // Cleared again before the reader goes: its requests run on the reader.
    if(controlServer) {
        controlServer->setTarget(uc_log::detail::ControlTarget{
          .read =
            [&rttReader](std::span<uc_log::control::Piece const> pieces) {
                std::vector<JLinkRttReader::MemoryAccess> reads;
                reads.reserve(pieces.size());
                for(auto const& p : pieces) { reads.push_back({p.address, p.size}); }
                return rttReader.accessMemory(reads);
            },
          .write =
            [&rttReader](std::span<uc_log::control::WordWrite const> words) {
                std::vector<JLinkRttReader::MemoryAccess> writes;
                writes.reserve(words.size());
                for(auto const& w : words) { writes.push_back({w.address, 4, w.value}); }
                return rttReader.accessMemory(writes);
            },
          .status =
            [&rttReader, &firmwareCheck, &logFilePrinter, &controlServer, startedUs] {
                return uc_log::control::StatusAnswer{
                  .running    = rttReader.getStatus().isRunning != 0,
                  .halted     = rttReader.isHalted(),
                  .flashing   = rttReader.isFlashing(),
                  .sessions   = static_cast<std::uint32_t>(rttReader.sessionCount()),
                  .firmware   = firmwareCheck.get(),
                  .errors     = logFilePrinter.statusErrorCount(),
                  .log_seq    = controlServer->logSeq(),
                  .started_us = startedUs,
                  .ram_image  = rttReader.isRamImage()};
            },
          .reset =
            [&rttReader](std::function<bool()> const& cancelled) {
                return rttReader.resetAndWait(std::chrono::seconds{20}, cancelled);
            },
          .flash =
            [&rttReader](std::function<bool()> const& cancelled) {
                return rttReader.flashAndWait(std::chrono::seconds{90}, cancelled);
            },
          .messages
          = [&logFilePrinter](std::size_t count) { return logFilePrinter.newestStatus(count); },
          .unixMicros   = {},
          .steadyMicros = {},
          .stop         = {}});
    }
    OnScopeExit const releaseTarget{[&controlServer] {
        if(controlServer) { controlServer->clearTarget(); }
    }};

    if(!disableUi) {
        gui.setMapFile(mapFile);
        return gui.run(rttReader, buildCommand, host, probe);
    } else {
        static std::atomic<bool> shutdown_requested(false);
        std::signal(SIGINT, [](int signal) {
            if(signal == SIGINT) { shutdown_requested = true; }
        });
        while(!shutdown_requested) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        return 0;
    }
}
