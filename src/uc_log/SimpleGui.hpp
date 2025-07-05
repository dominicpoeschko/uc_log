#pragma once
#include "uc_log/detail/LogEntry.hpp"

#include <csignal>
#include <map>
#include <mutex>
#include <sys/ioctl.h>
#include <termios.h>

namespace uc_log {
struct SimpleGui {
    std::atomic<bool> quit{true};

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

    void add(std::chrono::system_clock::time_point recv_time,
             uc_log::detail::LogEntry const&       e) {
        std::size_t const terminal_width = []() -> std::size_t {
            struct winsize w{};
            if(-1 == ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) || w.ws_col > 1024) {
                return 120;
            }
            return w.ws_col;
        }();
        if(!quit) {
            std::lock_guard<std::mutex> lock(ioMutex);
            if(enabledLogs[e.logLevel]) {
                bool const any_disabled = std::any_of(enabledLogs.begin(),
                                                      enabledLogs.end(),
                                                      [](auto const& v) { return !v.second; });

                auto color
                  = fmt::bg(any_disabled ? fmt::terminal_color::red : fmt::terminal_color::green);
                if(!any_disabled) {
                    color = color | fmt::fg(fmt::terminal_color::black);
                }
                fmt::print("{}", fmt::styled(" ", color));
                std::size_t charsPrinted = 1;

                if(printSysTime) {
                    auto const sysTimeString
                      = fmt::format("{}", detail::to_time_string_with_milliseconds(recv_time));
                    charsPrinted += sysTimeString.size();
                    fmt::print("{}",
                               fmt::styled(sysTimeString, fmt::fg(fmt::terminal_color::cyan)));
                }
                fmt::print(fmt::runtime(fmt::format("{{:<{}{}}}\n",
                                                    terminal_width - charsPrinted,
                                                    printFunctionName ? "#" : "")),
                           e);
            }
        }
    }

    void fatalError(std::string_view msg) {
        errorMessage(fmt::format("Fatal Error: {}\nquitting now\n", msg));
        quit = true;
    }

    void statusMessage(std::string_view msg) {
        if(!quit) {
            std::lock_guard<std::mutex> lock(ioMutex);
            fmt::print(fmt::bg(fmt::terminal_color::green), "{}", msg);
            fmt::print("\n");
        }
    }

    void errorMessage(std::string_view msg) {
        if(!quit) {
            std::lock_guard<std::mutex> lock(ioMutex);
            fmt::print(stderr, "{}\n", msg);
        }
    }

    template<typename Reader>
    int run(Reader&            rttReader,
            std::string const& buildCommand) {
        {
            struct termios oldTerminalSettings{};
            if(::tcgetattr(STDIN_FILENO, &oldTerminalSettings) < 0) {
                std::lock_guard<std::mutex> lock(ioMutex);
                fmt::print(stderr, "Error calling tcgetattr: {}\n", ::strerror(errno));
                return 1;
            }
            oldTerminalSettings.c_lflag
              &= ~static_cast<decltype(oldTerminalSettings.c_lflag)>(ICANON | ECHO);
            if(::tcsetattr(STDIN_FILENO, TCSANOW, &oldTerminalSettings) < 0) {
                std::lock_guard<std::mutex> lock(ioMutex);
                fmt::print(stderr, "Error calling tcsetattr: {}\n", ::strerror(errno));
                return 1;
            }
        }
        auto doBuild = [&]() {
            fmt::print(fmt::bg(fmt::terminal_color::green), "build");
            fmt::print("\n");
            int const ret = std::system(buildCommand.c_str());
            fmt::print(fmt::bg(ret == 0 ? fmt::terminal_color::green : fmt::terminal_color::red),
                       "build {}",
                       ret == 0 ? "succeeded" : "failed");
            fmt::print("\n");
            return ret == 0;
        };

        static std::atomic<bool> gotSignal{};
        {
            struct sigaction signalAction{};
            signalAction.sa_handler = [](int) { gotSignal = true; };
            ::sigemptyset(&signalAction.sa_mask);
            if(::sigaction(SIGINT, &signalAction, nullptr) == -1) {
                std::lock_guard<std::mutex> lock(ioMutex);
                fmt::print(stderr, "Failed to register signal handler: {}\n", std::strerror(errno));
                return 1;
            }
        }

        quit = false;
        while(!quit && !gotSignal) {
            char       c;
            auto const status = ::read(STDIN_FILENO, std::addressof(c), 1);
            if(status != 1) {
                quit = true;
                if(gotSignal) {
                    std::lock_guard<std::mutex> lock(ioMutex);
                    fmt::print(fmt::bg(fmt::terminal_color::bright_blue), "interrupt");
                    fmt::print("\n");
                } else {
                    std::lock_guard<std::mutex> lock(ioMutex);
                    fmt::print(fmt::bg(fmt::terminal_color::red),
                               "read error {}",
                               std::strerror(errno));
                    fmt::print("\n");
                }
            } else {
                switch(c) {
                case 'v':
                    {
                        if(printSysTime) {
                            std::lock_guard<std::mutex> lock(ioMutex);
                            printSysTime = false;
                            fmt::print(fmt::bg(fmt::terminal_color::red),
                                       "sysTime printing stopped");
                            fmt::print("\n");
                        } else {
                            std::lock_guard<std::mutex> lock(ioMutex);
                            printSysTime = true;
                            fmt::print(fmt::bg(fmt::terminal_color::green)
                                         | fmt::fg(fmt::terminal_color::black),
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
                            fmt::print(fmt::bg(fmt::terminal_color::red),
                                       "functionName printing stopped");
                            fmt::print("\n");
                        } else {
                            std::lock_guard<std::mutex> lock(ioMutex);
                            printFunctionName = true;
                            fmt::print(fmt::bg(fmt::terminal_color::green)
                                         | fmt::fg(fmt::terminal_color::black),
                                       "functionName printing started");
                            fmt::print("\n");
                        }
                    }
                    break;

                case 's':
                    {
                        auto const s = rttReader.getStatus();

                        std::lock_guard<std::mutex> lock(ioMutex);
                        fmt::print(fmt::bg(fmt::terminal_color::bright_blue),
                                   "Connected: {}, BytesRead: {}, Overflows: {}, UpBuffers: {}, "
                                   "DownBuffers: "
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
                case 'd':
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
                                fmt::print(fmt::bg(fmt::terminal_color::red),
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
                          "d: reset "
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

                if((c >= ('0' + static_cast<int>(uc_log::LogLevel::trace)))
                   && (c <= ('0' + static_cast<int>(uc_log::LogLevel::crit))))
                {
                    uc_log::LogLevel const levelToToggle = static_cast<uc_log::LogLevel>(c - '0');

                    bool const oldState = enabledLogs[levelToToggle];

                    auto const color = oldState ? fmt::bg(fmt::terminal_color::red)
                                                : (fmt::bg(fmt::terminal_color::green)
                                                   | fmt::fg(fmt::terminal_color::black));

                    std::lock_guard<std::mutex> lock(ioMutex);

                    enabledLogs[levelToToggle] = !oldState;

                    fmt::print(
                      "{} {}\n",
                      levelToToggle,
                      fmt::styled(fmt::format("printing {}", oldState ? "disabled" : "enabled"),
                                  color));
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(ioMutex);
            fmt::print(fmt::bg(fmt::terminal_color::bright_blue), "quitting");
            fmt::print("\n");
        }
        return gotSignal ? 1 : 0;
    }
};
}   // namespace uc_log
