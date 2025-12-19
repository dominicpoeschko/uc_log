#include "jlink/JLink.hpp"

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/FTXUIGui.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/TimeDelayedQueue.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/TcpSender.hpp"
#include "uc_log/metric_utils.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ranges>

namespace {
std::expected<std::uint32_t,
              std::string>
parseMapFileForControlBlockAddress(std::filesystem::path const& mapFile) {
    static constexpr std::string_view needle{"::rttControlBlock"};

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
        std::uint32_t address{};
        auto const [ptr, ec]
          = std::from_chars(std::data(addressLine),
                            std::next(std::data(addressLine), std::ssize(addressLine)),
                            address,
                            16);
        if(ec == std::errc{}) { return address; }
    }

    return std::unexpected(
      fmt::format("failed to parse address from file: {:?} lines: {::?}", mapFile, addressLines));
}

std::string to_iso8601_UTC_string(std::chrono::system_clock::time_point const& value) {
    //1970-01-01T00:00:00.000Z
    auto const time{std::chrono::system_clock::to_time_t(value)};
    auto const utc     = fmt::gmtime(time);
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
      value - std::chrono::time_point_cast<std::chrono::minutes>(value));
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      value - std::chrono::time_point_cast<std::chrono::seconds>(value));
    return fmt::format("{:%FT%H:%M}:{:02}.{:03}Z", utc, seconds.count(), milliseconds.count());
}

struct LogFilePrinter {
    uc_log::FTXUIGui::Gui& gui;
    std::filesystem::path  logFilePath;
    std::ofstream          logFile;
    bool                   errorShown{false};

    LogFilePrinter(uc_log::FTXUIGui::Gui& gui_,
                   std::string const&     logDir)
      : gui{gui_}
      , logFilePath{std::filesystem::path{logDir}
                    / fmt::format("{}.rttlog",
                                  to_iso8601_UTC_string(std::chrono::system_clock::now()))}
      , logFile{logFilePath} {
        if(!logFile.is_open()) {
            fmt::print(stderr, "failed to open logfile: {:?}", logFilePath);
            std::terminate();
        }
        fmt::print(logFile, "recv_time_utc,channel,file,line,function,log_level,uc_time,message\n");
    }

    void operator()(std::chrono::system_clock::time_point recv_time,
                    uc_log::detail::LogEntry const&       entry) {
        if(logFile) {
            fmt::print(logFile,
                       "{},{},{:?},{},{:?},{:#},{},{:?}\n",
                       to_iso8601_UTC_string(recv_time),
                       entry.channel.channel,
                       entry.fileName,
                       entry.line,
                       entry.functionName,
                       entry.logLevel,
                       entry.ucTime.time,
                       entry.logMsg);
        } else {
            if(!errorShown) {
                gui.errorMessage(fmt::format("error writing logFile: {:?}", logFilePath));
                errorShown = true;
            }
        }
    }
};

struct TcpPrinter {
    TCPSender tcpSender;

    TcpPrinter(uc_log::FTXUIGui::Gui& gui,
               std::uint16_t          port)
      : tcpSender{port,
                  [&gui](auto const& msg) { gui.errorMessage(msg); }} {}

    void operator()(std::chrono::system_clock::time_point recv_time,
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
    CLI::App app{};

    std::uint32_t speed{};
    std::string   device{};
    std::uint32_t channels{};
    std::string   mapFile{};
    std::string   hexFile{};
    std::string   stringConstantsFile{};
    std::string   host{};
    std::string   logDir{};
    std::string   buildCommand{};
    std::uint16_t port{};

    app.add_option("--metrics_port", port, "tcp for metrics")->required();
    app.add_option("--speed", speed, "swd speed")->required();
    app.add_option("--device", device, "mpu device")->required();
    app.add_option("--channels", channels, "rtt channels")->required();
    app.add_option("--build_command", buildCommand, "build command")->required();
    app.add_option("--map_file", mapFile, "map file")->required()->check(CLI::ExistingFile);
    app.add_option("--hex_file", hexFile, "hex file")->required()->check(CLI::ExistingFile);
    app.add_option("--string_constants_file", stringConstantsFile, "string constants map file")
      ->required()
      ->check(CLI::ExistingFile);
    app.add_option("--log_dir", logDir, "log file directory")
      ->required()
      ->check(CLI::ExistingDirectory);
    app.add_option("--host", host, "jlink host");

    CLI11_PARSE(app, argc, argv)

    uc_log::FTXUIGui::Gui gui{};
    LogFilePrinter        logFilePrinter{gui, logDir};
    TcpPrinter            tcpPrinter{gui, port};

    TimeDelayedQueue queue{
      [](auto const& entry) { return entry.entry.ucTime; },
      [&logFilePrinter, &tcpPrinter, &gui](std::chrono::system_clock::time_point recv_time,
                                           uc_log::detail::LogEntry const&       entry) {
          logFilePrinter(recv_time, entry);
          tcpPrinter(recv_time, entry);
          gui.add(recv_time, entry);
      }};

    JLinkRttReader rttReader{host,
                             device,
                             speed,
                             channels,
                             [&mapFile, &gui]() {
                                 auto const result = parseMapFileForControlBlockAddress(mapFile);
                                 if(!result.has_value()) { gui.fatalError(result.error()); }
                                 return result.value_or(0);
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

    return gui.run(rttReader, buildCommand);
}
