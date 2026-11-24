#include "uc_log/printer/rtx/Gui.hpp"

#include "support/FakeTargetPort.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/ProgramArgs.hpp"
#include "uc_log/printer/TargetOp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using std::string_view_literals::operator""sv;

constexpr std::array fileNames{"main.cpp"sv, "foo.hpp"sv, "bar.c"sv, "baz.hpp"sv};

constexpr std::array logMessages{
  "Hallo"sv,
  "oh noooooo"sv,
  "all bad"sv,
  "messed up"sv,
  "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"sv};

constexpr std::array functionNames{"main"sv, "print"sv, "drive"sv, "assert"sv};

constexpr std::array functionLines{20, 50, 129, 10023};

// "" = a line outside every module scope (UC_LOG_SCOPE_MODULE)
constexpr std::array moduleNames{""sv,
                                 "i2c"sv,
                                 "i2c.bus"sv,
                                 "i2c.scanner"sv,
                                 "usb.cdc.mixin"sv,
                                 "usb.device"sv,
                                 "myapp.regulator"sv};

namespace {
// uc_log_gui_test's command line, every option left out by default:
//   --modules <n>      n modules in a tree (areas of ten parts) instead of the few above - a filter with many
//   --metrics <n>      n metrics in groups of ten (group0::m0 ...), each its own frequency and unit, instead of the
//                      demo's - a metrics page with many series
//   --seed <n>         the same entries on every run
//   --frozen <rounds>  after the preload only that many rounds of the loop, without its pause, then nothing more - with
//                      --seed, a capture of each tab repeats
//   --preload <n>      n entries bulk-loaded as fast as possible (filter/scroll on a large log); with --metrics each
//                      line carries a value of the metrics in turn - a window that has run for hours
//   --rate <lines/s>   a flood instead of the slow stream, in batches every 10 ms - how the window keeps up
//   --format_errors    a format error every 2 s
//   --ui_fps <n>, --ui_stats: the window's pace and its report at quit, as uc_log_printer's
constexpr std::array<std::string_view, 7>
  numberOptions{"modules", "metrics", "seed", "frozen", "preload", "rate", "ui_fps"};
constexpr std::array<std::string_view, 2> flagOptions{"format_errors", "ui_stats"};

using MetricFunction = std::pair<std::tuple<std::string, std::string, std::string>,
                                 std::function<double(std::chrono::duration<double>)>>;

// --metrics <n>, or the few below
std::vector<MetricFunction> demoMetrics(std::optional<std::uint64_t> count) {
    if(!count) {
        return {
          {           {"foo", "sin", "V"},
           [](std::chrono::duration<double> t) { return std::sin(t.count()); }                            },
          {           {"bar", "tan", "A"},
           [](std::chrono::duration<double> t) { return std::tan(t.count()); }                            },
          {          {"foo", "line", "W"}, [](std::chrono::duration<double> t) { return 0.5 * t.count(); }},
          // an integer firmware's mp-units quantities, each sent in its own unit: shown as °C, A·s, µs, 1/°C
          {       {"heater", "tip", "c℃"},
           [](std::chrono::duration<double> t) {
           return std::round(31250.0 + (2000.0 * std::sin(t.count())));
           }                                                                                              },
          { {"heater", "er2Owed", "mA s"},
           [](std::chrono::duration<double> t) {
           return std::round(80000.0 * (1.0 + std::sin(0.3 * t.count())));
           }                                                                                              },
          {{"heater", "kp", "(10⁻³ 1/℃)"},            [](std::chrono::duration<double>) { return 2500.0; }},
          {          {"adc", "isr", "µs"},
           [](std::chrono::duration<double> t) {
           return std::round(600.0 + (50.0 * std::sin(3.0 * t.count())));
           }                                                                                              },
        };
    }
    constexpr std::array        units{"V"sv, "A"sv, "W"sv, "Hz"sv, "B"sv};
    auto const                  n = static_cast<std::size_t>(*count);
    std::vector<MetricFunction> out;
    out.reserve(n);
    for(std::size_t i = 0; i != n; ++i) {
        auto const speed = 1.0 + (0.1 * static_cast<double>(i));
        auto const scale = std::pow(10.0, static_cast<double>(i % 7) - 3.0);
        out.push_back({
          {"group" + std::to_string(i / 10),
           "m" + std::to_string(i % 10),
           std::string{units[i % units.size()]}},
          [speed, scale](std::chrono::duration<double> t) {
              return scale * std::sin(speed * t.count());
           }
        });
    }
    return out;
}
}   // namespace

// `many`: --modules, 0 = the few above
template<typename Gen>
void updateMessage(uc_log::printer::LogEntry& e,
                   Gen&                       gen,
                   std::size_t                many) {
    e.channel.channel = std::uniform_int_distribution<std::size_t>{0, 5}(gen);
    e.logLevel        = static_cast<uc_log::LogLevel>(std::uniform_int_distribution<std::uint8_t>{
      static_cast<std::uint8_t>(uc_log::LogLevel::trace),
      static_cast<std::uint8_t>(uc_log::LogLevel::crit)}(gen));
    std::ranges::sample(fileNames, &e.fileName, 1, gen);
    std::ranges::sample(logMessages, &e.logMsg, 1, gen);
    std::ranges::sample(functionNames, &e.functionName, 1, gen);
    std::ranges::sample(functionLines, &e.line, 1, gen);
    if(many != 0) {
        auto const i = std::uniform_int_distribution<std::size_t>{0, many - 1}(gen);
        e.module     = "area" + std::to_string(i / 10) + ".part" + std::to_string(i % 10);
    } else {
        e.module
          = moduleNames[std::uniform_int_distribution<std::size_t>{0, moduleNames.size() - 1}(gen)];
    }
    e.parsedOk = true;   // fabricated entries are complete, not parser bailouts
}

namespace {

void updateStatus(uc_log::printer::TargetPort::LinkStatus& status) {
    status.numBytesTransferred += 10;
    status.numBytesRead += 10;

    if(status.numBytesTransferred > 1000) { status.isRunning = 0; }
}

}   // namespace

// uc_log_gui_test: the printer's window on a fake target (no probe, no memory) and made-up lines.
int main(int    argc,
         char** argv) {
    auto const parsed = uc_log::printer::parseProgramArgs(argc, argv, numberOptions, flagOptions);
    if(!parsed) {
        std::fprintf(stderr,
                     "uc_log_gui_test: %s (the options: host/gui_test.cpp)\n",
                     parsed.error().c_str());
        return 2;
    }
    auto const& args = *parsed;
    if(!args.positional.empty()) {
        std::fprintf(stderr, "uc_log_gui_test takes no positional argument\n");
        return 2;
    }
    auto const fps = args.number("ui_fps").value_or(0);
    if(fps > 1000) {
        std::fprintf(stderr, "uc_log_gui_test: --ui_fps is 0..1000\n");
        return 2;
    }
    auto const many            = static_cast<std::size_t>(args.number("modules").value_or(0));
    auto const metricsCount    = args.number("metrics");
    auto const metricFunctions = demoMetrics(metricsCount);
    auto const seed            = args.number("seed");
    auto const frozen          = args.number("frozen");
    auto const preload         = args.number("preload");
    auto const rate            = args.number("rate");
    bool const formatErrors    = args.flag("format_errors");

    uc_log::printer::PrinterModel model{"127.0.0.1", 34565, 34600};
    auto&                         messageLog = model.messages();
    uc_log::printer::rtx::Gui     gui{
      model,
      uc_log::printer::rtx::WindowOptions{.frameInterval
                                          = fps == 0 ? std::chrono::microseconds{}
                                                     : std::chrono::microseconds{1'000'000 / fps},
                                          .stats = args.flag("ui_stats")}
    };

    std::vector<std::string> messages;
    std::mutex               msgMutex;
    auto const               say = [&](std::string const& msg) {
        std::scoped_lock<std::mutex> lock{msgMutex};
        messages.push_back(msg);
    };

    // the demo's target: no memory, no probe; every request is said in the log, the debugger buttons and the Health
    // and Inspect tabs refused
    uc_log::test::FakeTargetPort reader{
      [&](std::span<uc_log::printer::TargetOp const> ops) -> uc_log::printer::TargetPort::Result {
          for(auto const& op : ops) { say("Target op: " + uc_log::printer::describe(op)); }
          return std::unexpected{std::string{"no target in the gui demo"}};
      }};
    reader.onEvent   = say;
    reader.probeList = {
      {123456, "J-Link EDU Mini", "board-a",  true},
      {654321,     "J-Link PLUS",        "",  true},
      {111222,      "J-Link PRO",     "lab", false}
    };

    auto addTime = std::chrono::milliseconds{100};

    std::jthread runner([&](std::stop_token const& stoken) {
        uc_log::printer::LogEntry e{0, ""};
        e.ucTime.time = std::chrono::nanoseconds{0};
        std::mt19937 gen{seed ? static_cast<std::uint32_t>(*seed) : std::random_device{}()};
        std::size_t  loops{};
        std::size_t  rounds{frozen ? static_cast<std::size_t>(*frozen) : std::size_t{}};

        if(preload) {
            bool const metered = metricsCount && !metricFunctions.empty();
            for(std::size_t i = 0; i != *preload && !stoken.stop_requested(); ++i) {
                updateMessage(e, gen, many);
                if(metered) {
                    auto const& [meta, metricFunction]
                      = metricFunctions[i % metricFunctions.size()];
                    e.logMsg = std::format("@METRIC({}::{}[{}]={})",
                                           std::get<0>(meta),
                                           std::get<1>(meta),
                                           std::get<2>(meta),
                                           metricFunction(e.ucTime.time));
                } else if(i % 100 == 0) {
                    e.logMsg = "multi\nline\nmessage";
                }
                e.ucTime.time += std::chrono::milliseconds{1};
                gui.add(std::chrono::system_clock::now(), e);
            }
        }

        if(rate) {
            auto const batch = std::max<std::size_t>(1, static_cast<std::size_t>(*rate / 100));
            auto       next  = std::chrono::steady_clock::now();
            while(!stoken.stop_requested()) {
                for(std::size_t i = 0; i != batch; ++i) {
                    updateMessage(e, gen, many);
                    e.ucTime.time += std::chrono::microseconds{10};
                    gui.add(std::chrono::system_clock::now(), e);
                }
                reader.updateLink(updateStatus);
                next += std::chrono::milliseconds{10};
                std::this_thread::sleep_until(next);
            }
            return;
        }

        while(!stoken.stop_requested()) {
            if(frozen) {
                if(rounds == 0) {
                    std::this_thread::sleep_for(addTime);
                    continue;
                }
                --rounds;
            }
            updateMessage(e, gen, many);
            reader.updateLink(updateStatus);
            e.ucTime.time += addTime;
            gui.add(std::chrono::system_clock::now(), e);

            {
                std::string originalMessage = e.logMsg;
                for(auto const& [meta, metricFunction] : metricFunctions) {
                    e.logMsg = std::format("{} @METRIC({}::{}[{}]={})",
                                           originalMessage,
                                           std::get<0>(meta),
                                           std::get<1>(meta),
                                           std::get<2>(meta),
                                           metricFunction(e.ucTime.time));
                    gui.add(std::chrono::system_clock::now(), e);
                }
            }

            std::vector<std::string> localMessages;
            {
                std::scoped_lock<std::mutex> lock{msgMutex};
                localMessages = messages;
                messages.clear();
            }

            for(auto const& msg : localMessages) {
                messageLog.status(msg);
                messageLog.error(msg);
                messageLog.fatal(msg);
            }

            if(formatErrors && ++loops % 20 == 0) {
                messageLog.formatError(
                  "bad format for replacement field \"{:#06x}\": invalid format "
                  "specifier (gui_test)");
            }

            if(!frozen) { std::this_thread::sleep_for(addTime); }
        }
    });

    return gui.run(reader, "make", "", "board-a");
}
