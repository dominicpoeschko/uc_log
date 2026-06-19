#include "jlink/JLink.hpp"

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/FTXUIGui.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/TimeDelayedQueue.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/LogFormat.hpp"
#include "uc_log/detail/TcpSender.hpp"
#include "uc_log/metric_utils.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include <ranges>

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

    auto addressLines = content | std::views::split('\n')
                      | std::views::transform([](auto&& rng) { return std::string_view{rng}; })
                      | std::views::filter([](auto&& line) { return line.contains(needle); });

    for(auto addressLine : addressLines) {
        auto const lineEnd = std::next(std::data(addressLine), std::ssize(addressLine));

        // parse: "<address> <lma> <size> ..."
        std::uint32_t address{};
        auto [afterAddr, ec1] = std::from_chars(std::data(addressLine), lineEnd, address, 16);
        if(ec1 != std::errc{}) { continue; }

        // skip whitespace + LMA field
        afterAddr = std::find_if_not(afterAddr, lineEnd, [](char c) { return c == ' '; });
        std::uint32_t lma{};
        auto [afterLma, ec2] = std::from_chars(afterAddr, lineEnd, lma, 16);
        if(ec2 != std::errc{}) { continue; }

        // skip whitespace + parse size field
        afterLma = std::find_if_not(afterLma, lineEnd, [](char c) { return c == ' '; });
        std::uint32_t size{};
        auto [afterSize, ec3] = std::from_chars(afterLma, lineEnd, size, 16);
        if(ec3 != std::errc{}) { continue; }

        if(size >= controlBlockHeaderSize
           && (size - controlBlockHeaderSize) % bufferControlBlockSize == 0)
        {
            std::uint32_t const totalBuffers
              = (size - controlBlockHeaderSize) / bufferControlBlockSize;
            return RttBlockInfo{address, totalBuffers};
        }
    }

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
    std::mutex                                           mutex;

    LogFilePrinter(uc_log::FTXUIGui::Gui& gui,
                   std::string const&     logDir)
      : errorMessagef{[&gui](auto const& m) { gui.errorMessage(m); }}
      , statusChangef{[&gui](LogFileStatus    s,
                             std::string_view p) { gui.setLogFileStatus(s, p); }} {
        openFileUnlocked(logDir);
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
        if(!logFile.is_open()) {
            errorMessagef(fmt::format("failed to open logfile: {:?}", logFilePath));
            if(statusChangef) { statusChangef(LogFileStatus::Error, logFilePath.string()); }
        } else {
            uc_log::detail::logformat::writeHeader(logFile);
            if(statusChangef) { statusChangef(LogFileStatus::Active, logFilePath.string()); }
        }
    }
};

struct TcpPrinter {
    TCPSender tcpSender;

    TcpPrinter(uc_log::FTXUIGui::Gui& gui,
               std::uint16_t          port)
      : tcpSender{port,
                  [&gui](auto const& msg) { gui.errorMessage(msg); },
                  [&gui](TcpPortStatus s,
                         std::uint16_t p) { gui.setTcpPortStatus(s, p); }} {}

    void restart(std::uint16_t newPort) { tcpSender.restart(newPort); }

    void add(std::chrono::system_clock::time_point recv_time,
             uc_log::detail::LogEntry const&       entry) {
        auto const metrics = uc_log::extractMetrics(recv_time, entry);
        for(auto const& metric : metrics) {
            tcpSender.send(
              fmt::format(R"("/*{{"name":{:?},"scope":{:?},"unit":{:?},"time":{},"value":{}}}*/{})",
                          metric.first.name,
                          metric.first.scope,
                          metric.first.unit,
                          std::chrono::duration<double>(metric.second.uc_time.time).count(),
                          metric.second.value,
                          '\n'));
        }
    }
};
}   // namespace

int main(int    argc,
         char** argv) {
    std::uint32_t speed{};
    std::string   device{};
    std::string   mapFile{};
    std::string   hexFile{};
    std::string   stringConstantsFile{};
    std::string   host{};
    std::string   logDir{};
    std::string   buildCommand{};
    std::uint16_t port{};
    bool          disableUi{false};

    cxxopts::Options options("uc_log_printer");
    try {
        options.add_options()("metrics_port", "tcp for metrics", cxxopts::value<std::uint16_t>())(
          "speed",
          "swd speed",
          cxxopts::value<std::uint32_t>())("device", "mpu device", cxxopts::value<std::string>())(
          "build_command",
          "build command",
          cxxopts::value<std::string>())("map_file", "map file", cxxopts::value<std::string>())(
          "hex_file",
          "hex file",
          cxxopts::value<std::string>())("string_constants_file",
                                         "string constants map file",
                                         cxxopts::value<std::string>())(
          "log_dir",
          "log file directory",
          cxxopts::value<std::string>())("host",
                                         "jlink host",
                                         cxxopts::value<std::string>()->default_value(""))(
          "disable_ui",
          "disable ui and just log to file and tcp");
        auto const result   = options.parse(argc, argv);
        port                = result["metrics_port"].as<std::uint16_t>();
        speed               = result["speed"].as<std::uint32_t>();
        device              = result["device"].as<std::string>();
        buildCommand        = result["build_command"].as<std::string>();
        mapFile             = result["map_file"].as<std::string>();
        hexFile             = result["hex_file"].as<std::string>();
        stringConstantsFile = result["string_constants_file"].as<std::string>();
        logDir              = result["log_dir"].as<std::string>();
        host                = result["host"].as<std::string>();
        disableUi           = result.count("disable_ui") > 0;
    } catch(cxxopts::exceptions::exception const& e) {
        fmt::print(stderr, "Error: {}\n{}\n", e.what(), options.help());
        return 1;
    }

    uc_log::FTXUIGui::Gui gui{};
    LogFilePrinter        logFilePrinter{gui, logDir};
    TcpPrinter            tcpPrinter{gui, port};
    gui.setOnTcpPortChange([&tcpPrinter](std::uint16_t newPort) { tcpPrinter.restart(newPort); });
    gui.setTcpClientCountGetter([&tcpPrinter]() { return tcpPrinter.tcpSender.getClientCount(); });
    gui.setOnLogDirChange(
      [&logFilePrinter](std::string const& newDir) { logFilePrinter.changeDir(newDir); });
    gui.setOnLogFileEnable([&logFilePrinter](bool enabled) { logFilePrinter.setEnabled(enabled); });
    gui.setOnTcpEnable([&tcpPrinter, port](bool enabled) {
        if(enabled) {
            auto const current = tcpPrinter.tcpSender.getPort();
            tcpPrinter.restart(current != 0 ? current : port);
        } else {
            tcpPrinter.tcpSender.stop();
        }
    });

    TimeDelayedQueue queue{
      [](auto const& entry) { return entry.entry.ucTime; },
      [&logFilePrinter, &tcpPrinter, &gui](std::chrono::system_clock::time_point recv_time,
                                           uc_log::detail::LogEntry const&       entry) {
          logFilePrinter.add(recv_time, entry);
          tcpPrinter.add(recv_time, entry);
          gui.add(recv_time, entry);
      }};

    JLinkRttReader rttReader{host,
                             device,
                             speed,
                             [&mapFile, &gui]() {
                                 auto const result = parseMapFileForControlBlockInfo(mapFile);
                                 if(!result.has_value()) { gui.fatalError(result.error()); }
                                 return result.value_or(RttBlockInfo{});
                             },
                             [&hexFile]() { return hexFile; },
                             [&stringConstantsFile, &gui]() {
                                 auto const result = remote_fmt::parseStringConstantsFromJsonFile(
                                   stringConstantsFile);
                                 if(!result.has_value()) { gui.fatalError(result.error()); }
                                 return result.value_or({});
                             },
                             [&queue](std::size_t channel, std::string_view msg) {
                                 queue.append(uc_log::detail::LogEntry{channel, msg});
                             },
                             [&gui](std::string_view msg) { gui.statusMessage(msg); },
                             [&gui](std::string_view msg) { gui.errorMessage(msg); },
                             [&gui](std::string_view msg) { gui.toolStatusMessage(msg); },
                             [&gui](std::string_view msg) { gui.toolErrorMessage(msg); }};

    if(!disableUi) {
        return gui.run(rttReader, buildCommand, host);
    } else {
        static std::atomic<bool> shutdown_requested(false);
        std::signal(SIGINT, [](int signal) {
            if(signal == SIGINT) { shutdown_requested = true; }
        });
        while(!shutdown_requested) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        return 0;
    }
}
