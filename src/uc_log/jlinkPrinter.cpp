#include "jlink/JLink.hpp"

#include "remote_fmt/catalog_helpers.hpp"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <regex>

static std::pair<std::uint32_t,
                 std::string>
parseMapFileForControlBlockAddress(std::string const& mapFile) {
    try {
        std::ifstream file(mapFile);
        std::string   line;
        while(std::getline(file, line)) {
            constexpr std::string_view needle{"::rttControlBlock"};
            if(std::search(line.begin(), line.end(), needle.begin(), needle.end()) != line.end()) {
                return {static_cast<std::uint32_t>(std::stoull(line, nullptr, 16)), {}};
            }
        }
    } catch(std::exception const& e) {
        return {0, fmt::format("read mapfile failed: {}", e.what())};
    }
    return {0, "error can't find rtt control block address"};
}

static std::string to_iso8601_UTC_string(std::chrono::system_clock::time_point const& value) {
    //1970-01-01T00:00:00.000Z
    auto const t{std::chrono::system_clock::to_time_t(value)};
    auto const utc     = fmt::gmtime(t);
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
      value - std::chrono::time_point_cast<std::chrono::minutes>(value));
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      value - std::chrono::time_point_cast<std::chrono::seconds>(value));
    return fmt::format("{:%FT%H:%M}:{:02}.{:03}Z", utc, seconds.count(), milliseconds.count());
}

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

    std::string const logFileName
      = logDir + "/" + to_iso8601_UTC_string(std::chrono::system_clock::now()) + ".rttlog";
    std::ofstream logFile{logFileName};
    if(!logFile.is_open()) {
        fmt::print(stderr, "failed to open logfile \"{}\"\n", logFileName);
        return 1;
    }

    uc_log::FTXUIGui::Gui gui{};
    auto logFilePrinter = [&gui, &logFile](std::chrono::system_clock::time_point recv_time,
                                           uc_log::detail::LogEntry const&       e) {
        if(logFile) {
            std::stringstream quotedMsg;
            quotedMsg << std::quoted(e.logMsg, '"', '"');

            std::stringstream quotedFilename;
            quotedFilename << std::quoted(e.fileName, '"', '"');

            std::stringstream quotedFunctionName;
            quotedFunctionName << std::quoted(e.functionName, '"', '"');

            auto const s = fmt::format("{},{},{},{},{},{:#},{},{}\n",
                                       to_iso8601_UTC_string(recv_time),
                                       e.channel.channel,
                                       quotedFilename.str(),
                                       quotedFunctionName.str(),
                                       e.line,
                                       e.logLevel,
                                       e.ucTime.time,
                                       quotedMsg.str());

            logFile << s;
        } else {
            gui.errorMessage("error writing logFile");
        }
    };
    TCPSender tcpSender{port, [&gui](auto msg) { gui.errorMessage(msg); }};

    auto tcpPrinter = [&tcpSender](std::chrono::system_clock::time_point recv_time,
                                   uc_log::detail::LogEntry const&       e) {
        auto const metrics = uc_log::extractMetrics(recv_time, e);
        for(auto const& metric : metrics) {
            tcpSender.send(
              fmt::format("/*{{\"name\":\"{}\",\"scope\":\"{}\",\"unit\":\"{}\",\"time\":{:%Q},"
                          "\"value\":{}}}*/\n",
                          metric.first.name,
                          metric.first.scope,
                          metric.first.unit,
                          metric.second.uc_time.time,
                          metric.second.value));
        }
    };
    auto printer
      = [&tcpPrinter, &logFilePrinter, &gui](std::chrono::system_clock::time_point recv_time,
                                             uc_log::detail::LogEntry const&       e) {
            tcpPrinter(recv_time, e);
            logFilePrinter(recv_time, e);
            gui.add(recv_time, e);
        };
    TimeDelayedQueue<uc_log::detail::LogEntry,
                     decltype([](auto const& e) { return e.entry.ucTime; })>
      q{printer};

    JLinkRttReader rttReader{host,
                             device,
                             speed,
                             channels,
                             [&mapFile, &gui]() {
                                 auto result = parseMapFileForControlBlockAddress(mapFile);
                                 if(result.second.empty()) {
                                     return result.first;
                                 }
                                 gui.fatalError(result.second);
                                 return decltype(result.first){};
                             },
                             [&hexFile]() { return hexFile; },
                             [&stringConstantsFile, &gui]() {
                                 auto result = remote_fmt::parseStringConstantsFromJsonFile(
                                   stringConstantsFile);
                                 if(result.second.empty()) {
                                     return result.first;
                                 }
                                 gui.fatalError(result.second);
                                 return decltype(result.first){};
                             },
                             [&q](std::size_t channel, std::string_view msg) {
                                 q.append(uc_log::detail::LogEntry{channel, msg});
                             },
                             [&gui](std::string_view msg) { gui.statusMessage(msg); },
                             [&gui](std::string_view msg) { gui.errorMessage(msg); }};

    return gui.run(rttReader, buildCommand);
}
