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

    void add(std::chrono::system_clock::time_point recv_time,
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
    std::uint32_t channels{};
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
          "channels",
          "rtt channels",
          cxxopts::value<std::uint32_t>())("build_command",
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
          cxxopts::value<std::string>()->default_value(
            ""))("disable_ui", "disable ui and just log to file and tcp");
        auto const result   = options.parse(argc, argv);
        port                = result["metrics_port"].as<std::uint16_t>();
        speed               = result["speed"].as<std::uint32_t>();
        device              = result["device"].as<std::string>();
        channels            = result["channels"].as<std::uint32_t>();
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

    if(!disableUi) {
        return gui.run(rttReader, buildCommand);
    } else {
        static std::atomic<bool> shutdown_requested(false);
        std::signal(SIGINT, [](int signal) {
            if(signal == SIGINT) { shutdown_requested = true; }
        });
        while(!shutdown_requested) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        return 0;
    }
}
