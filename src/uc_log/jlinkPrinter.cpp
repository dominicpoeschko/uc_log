#include "jlink/JLink.hpp"

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/TimeDelayedQueue.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/TcpSender.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <vector>

static std::uint32_t parseMapFileForControllBlockAddress(std::string const& mapFile) {
    try {
        std::ifstream file(mapFile);
        std::string   line;
        while(std::getline(file, line)) {
            constexpr std::string_view needle{"::rttControlBlock"};
            if(std::search(line.begin(), line.end(), needle.begin(), needle.end()) != line.end()) {
                return std::stoull(line, nullptr, 16);
            }
        }
    } catch(std::exception const& e) {
        fmt::print(stderr, "read mapfile failed: {}", e.what());
    }
    return 0;
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
static std::string
to_time_string_with_milliseconds(std::chrono::system_clock::time_point const& value) {
    //00:00:00.000
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
      value - std::chrono::time_point_cast<std::chrono::minutes>(value));
    auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      value - std::chrono::time_point_cast<std::chrono::seconds>(value));
    return fmt::format("{:%H:%M}:{:02}.{:03}", value, seconds.count(), milliseconds.count());
}

int main(int argc, char** argv) {
    static std::atomic<bool> quit{};
    {
        struct sigaction sa {};
        sa.sa_handler = [](int) { quit = true; };
        ::sigemptyset(&sa.sa_mask);
        if(::sigaction(SIGINT, &sa, nullptr) == -1) {
            fmt::print(stderr, "Failed to register signal handler: {}\n", std::strerror(errno));
            return 1;
        }
    }
    CLI::App app{};

    std::uint32_t speed{};
    std::string   device{};
    std::uint32_t channels;
    std::string   mapFile;
    std::string   hexFile;
    std::string   stringConstantsFile;
    std::string   host{};
    std::string   logDir{};
    std::string   buildCommand{};
    std::uint16_t port;

    app.add_option("--trace_port", port, "tcp for trace")->required();
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

    CLI11_PARSE(app, argc, argv);

    std::string const logFileName
      = logDir + "/" + to_iso8601_UTC_string(std::chrono::system_clock::now()) + ".rttlog";
    std::ofstream logFile{logFileName};
    if(!logFile.is_open()) {
        fmt::print(stderr, "failed to open logfile \"{}\"\n", logFileName);
        return 1;
    }

    TCPSender  tcpSender{port};
    std::mutex ioMutex;

    std::map<uc_log::LogLevel, bool> enabledLogs{
      {uc_log::LogLevel::trace, true},
      {uc_log::LogLevel::debug, true},
      { uc_log::LogLevel::info, true},
      { uc_log::LogLevel::warn, true},
      {uc_log::LogLevel::error, true},
      { uc_log::LogLevel::crit, true}
    };

    bool printSysTime{true};
    bool printFunctionName{false};

    auto tcpPrinter
      = [&](std::chrono::system_clock::time_point, uc_log::detail::LogEntry const& e) {
            bool const isTrace = e.logLevel == uc_log::LogLevel::trace;
            if(isTrace) {
                tcpSender.send(fmt::format("/*{:%Q},{}*/\n", e.ucTime.time, e.logMsg));
            }
        };

    auto logFilePrinter
      = [&](std::chrono::system_clock::time_point recv_time, uc_log::detail::LogEntry const& e) {
            std::stringstream quotedMsg;
            quotedMsg << std::quoted(e.logMsg, '"', '"');

            std::stringstream quotedFilename;
            quotedFilename << std::quoted(e.fileName, '"', '"');

            std::stringstream quotedFunctionName;
            quotedFunctionName << std::quoted(e.functionName, '"', '"');

            auto const s = fmt::format(
              "{},{},{},{},{},{:#},{},{}\n",
              to_iso8601_UTC_string(recv_time),
              e.channel.channel,
              quotedFilename.str(),
              quotedFunctionName.str(),
              e.line,
              e.logLevel,
              e.ucTime.time,
              quotedMsg.str());

            logFile << s;
        };

    auto consolePrinter =
      [&](std::chrono::system_clock::time_point recv_time, uc_log::detail::LogEntry const& e) {
          std::size_t const terminal_width = []() -> std::size_t {
              struct winsize w {};
              if(-1 == ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) || w.ws_col > 1024) {
                  return 120;
              }
              return w.ws_col;
          }();

          std::lock_guard<std::mutex> lock(ioMutex);
          if(enabledLogs[e.logLevel]) {
              bool const any_disabled
                = std::any_of(enabledLogs.begin(), enabledLogs.end(), [](auto const& v) {
                      return !v.second;
                  });

              auto color
                = fmt::bg(any_disabled ? fmt::terminal_color::red : fmt::terminal_color::green);
              if(!any_disabled) {
                  color = color | fmt::fg(fmt::terminal_color::black);
              }
              fmt::print("{}", fmt::styled(" ", color));
              std::size_t charsPrinted = 1;

              if(printSysTime) {
                  auto const sysTimeString
                    = fmt::format("{}", to_time_string_with_milliseconds(recv_time));
                  charsPrinted += sysTimeString.size();
                  fmt::print("{}", fmt::styled(sysTimeString, fmt::fg(fmt::terminal_color::cyan)));
              }
              fmt::print(
                fmt::runtime(fmt::format(
                  "{{:<{}{}}}\n",
                  terminal_width - charsPrinted,
                  printFunctionName ? "#" : "")),
                e);
          }
      };

    auto printer
      = [&](std::chrono::system_clock::time_point recv_time, uc_log::detail::LogEntry const& e) {
            tcpPrinter(recv_time, e);
            logFilePrinter(recv_time, e);
            consolePrinter(recv_time, e);
        };

    TimeDelayedQueue<uc_log::detail::LogEntry, decltype([](auto const& e) {
                         return e.entry.ucTime;
                     })>
      q{printer};

    JLinkRttReader rttReader{
      host,
      device,
      speed,
      channels,
      [&mapFile]() { return parseMapFileForControllBlockAddress(mapFile); },
      [&hexFile]() { return hexFile; },
      [&stringConstantsFile]() {
          return remote_fmt::parseStringConstantsFromJsonFile(stringConstantsFile);
      },
      [&q](std::size_t channel, std::string_view msg) {
          q.append(uc_log::detail::LogEntry{channel, msg});
      },
      [&ioMutex](std::string_view msg) {
          std::lock_guard<std::mutex> lock(ioMutex);
          fmt::print(fmt::bg(fmt::terminal_color::green), "{}", msg);
          fmt::print("\n");
      }};

    {
        struct termios oldIos {};
        if(::tcgetattr(STDIN_FILENO, &oldIos) < 0) {
            fmt::print(stderr, "Error calling tcgetattr: {}\n", ::strerror(errno));
            return 1;
        }
        oldIos.c_lflag &= ~static_cast<decltype(oldIos.c_lflag)>(ICANON | ECHO);
        if(::tcsetattr(STDIN_FILENO, TCSANOW, &oldIos) < 0) {
            fmt::print(stderr, "Error calling tcsetattr: {}\n", ::strerror(errno));
            return 1;
        }
    }

    auto doBuild = [&]() {
        fmt::print(fmt::bg(fmt::terminal_color::green), "build");
        fmt::print("\n");
        int const ret = std::system(buildCommand.c_str());
        fmt::print(
          fmt::bg(ret == 0 ? fmt::terminal_color::green : fmt::terminal_color::red),
          "build {}",
          ret == 0 ? "succeeded" : "failed");
        fmt::print("\n");
        return ret == 0;
    };

    while(!quit) {
        char       c;
        auto const status = ::read(STDIN_FILENO, std::addressof(c), 1);
        if(status != 1) {
            if(quit) {
                std::lock_guard<std::mutex> lock(ioMutex);
                fmt::print(fmt::bg(fmt::terminal_color::bright_blue), "interrupt");
                fmt::print("\n");
            } else {
                quit = true;
                std::lock_guard<std::mutex> lock(ioMutex);
                fmt::print(fmt::bg(fmt::terminal_color::red), "read error");
                fmt::print("\n");
            }
        } else {
            switch(c) {
            case 'v':
                {
                    if(printSysTime) {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        printSysTime = false;
                        fmt::print(fmt::bg(fmt::terminal_color::red), "sysTime printing stopped");
                        fmt::print("\n");
                    } else {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        printSysTime = true;
                        fmt::print(
                          fmt::bg(fmt::terminal_color::green) | fmt::fg(fmt::terminal_color::black),
                          "sysTime printing started");
                        fmt::print("\n");
                    }
                }
                break;

            case 'n':
                {
                    if(printFunctionName) {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        printFunctionName = false;
                        fmt::print(
                          fmt::bg(fmt::terminal_color::red),
                          "functionName printing stopped");
                        fmt::print("\n");
                    } else {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        printFunctionName = true;
                        fmt::print(
                          fmt::bg(fmt::terminal_color::green) | fmt::fg(fmt::terminal_color::black),
                          "functionName printing started");
                        fmt::print("\n");
                    }
                }
                break;

            case 's':
                {
                    auto const s = rttReader.getStatus();

                    std::lock_guard<std::mutex> lock(ioMutex);
                    fmt::print(
                      fmt::bg(fmt::terminal_color::bright_blue),
                      "Connected: {}, BytesRead: {}, Overflows: {}, UpBuffers: {}, DownBuffers: "
                      "{},\nEnabledLogs: ",
                      s.isRunning != 0,
                      s.numBytesRead,
                      s.hostOverflowCount,
                      s.numUpBuffers,
                      s.numDownBuffers);
                    fmt::print("{}\n", enabledLogs);
                }
                break;
            case 'r':
                {
                    rttReader.resetTarget();
                }
                break;
            case 'x':
                {
                    {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        fmt::print(fmt::bg(fmt::terminal_color::red), "resetting JLink");
                        fmt::print("\n");
                    }
                    rttReader.resetJLink();
                }
                break;
            case 'f':
                {
                    bool buildOk = false;
                    {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        buildOk = doBuild();
                        if(!buildOk) {
                            fmt::print(
                              fmt::bg(fmt::terminal_color::red),
                              "not flashing target build failed");
                            fmt::print("\n");
                        }
                    }
                    if(buildOk) {
                        rttReader.flash();
                    }
                }
                break;
            case 'h':
                {
                    std::lock_guard<std::mutex> lock(ioMutex);
                    fmt::print(
                      fmt::bg(fmt::terminal_color::bright_blue),
                      "f: reflash target, b: build, s: status, r: reset target, "
                      "x: reset "
                      "jlink, v: show sysTime, h: help, q: quit, n: print function name, 0-5: "
                      "toggle log level printing");
                    fmt::print("\n");
                }
                break;
            case 'b':
                {
                    std::lock_guard<std::mutex> lock(ioMutex);
                    doBuild();
                }
                break;

            case 'q':
                {
                    quit = true;
                }
                break;

            default:
                {
                }
            }

            if(
              (c >= ('0' + static_cast<int>(uc_log::LogLevel::trace)))
              && (c <= ('0' + static_cast<int>(uc_log::LogLevel::crit))))
            {
                uc_log::LogLevel const levelToToggle = static_cast<uc_log::LogLevel>(c - '0');

                bool const oldState = enabledLogs[levelToToggle];

                auto const color
                  = oldState
                    ? fmt::bg(fmt::terminal_color::red)
                    : (fmt::bg(fmt::terminal_color::green) | fmt::fg(fmt::terminal_color::black));

                std::lock_guard<std::mutex> lock(ioMutex);

                enabledLogs[levelToToggle] = !oldState;

                fmt::print(
                  "{} {}\n",
                  levelToToggle,
                  fmt::styled(
                    fmt::format("printing {}", oldState ? "disabled" : "enabled"),
                    color));
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(ioMutex);
        fmt::print(fmt::bg(fmt::terminal_color::bright_blue), "quitting");
        fmt::print("\n");
    }
    return 0;
}

