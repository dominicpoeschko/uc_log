#include "uc_log/printer/tui/Gui.hpp"

#include "support/FakeTargetPort.hpp"
#include "uc_log/printer/Environment.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/TargetOp.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

template<typename Gen>
void updateMessage(uc_log::printer::LogEntry& e,
                   Gen&                       gen) {
    e.channel.channel = std::uniform_int_distribution<std::size_t>{0, 5}(gen);
    e.logLevel        = static_cast<uc_log::LogLevel>(std::uniform_int_distribution<std::uint8_t>{
      static_cast<std::uint8_t>(uc_log::LogLevel::trace),
      static_cast<std::uint8_t>(uc_log::LogLevel::crit)}(gen));
    std::ranges::sample(fileNames, &e.fileName, 1, gen);
    std::ranges::sample(logMessages, &e.logMsg, 1, gen);
    std::ranges::sample(functionNames, &e.functionName, 1, gen);
    std::ranges::sample(functionLines, &e.line, 1, gen);
    e.module
      = moduleNames[std::uniform_int_distribution<std::size_t>{0, moduleNames.size() - 1}(gen)];
    e.parsedOk = true;   // fabricated entries are complete, not parser bailouts
}

namespace {

void updateStatus(uc_log::printer::TargetPort::LinkStatus& status) {
    status.numBytesTransferred += 10;
    status.numBytesRead += 10;

    if(status.numBytesTransferred > 1000) { status.isRunning = 0; }
}

}   // namespace

int main() {
    std::array<std::pair<std::tuple<std::string, std::string, std::string>,
                         std::function<double(std::chrono::duration<double>)>>,
               3>
      metricFunctions{
        {
         {{"foo", "sin", "V"},
           [](std::chrono::duration<double> t) { return std::sin(t.count()); }},
         {{"bar", "tan", "A"},
           [](std::chrono::duration<double> t) { return std::tan(t.count()); }},
         {{"foo", "line", "W"}, [](std::chrono::duration<double> t) { return 0.5 * t.count(); }},
         }
    };

    uc_log::printer::PrinterModel model{"127.0.0.1", 34565, 34600};
    auto&                         messageLog = model.messages();
    uc_log::printer::tui::Gui     gui{model};

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

    // GUI_TEST_SEED=<n>: the same entries on every run; GUI_TEST_FROZEN=<rounds>: after the preload only that many
    // rounds of the loop below, without its pause, then nothing more - with both, a capture of each tab repeats
    char const* const seed   = uc_log::printer::environment("GUI_TEST_SEED");
    char const* const frozen = uc_log::printer::environment("GUI_TEST_FROZEN");

    std::jthread runner([&](std::stop_token const& stoken) {
        uc_log::printer::LogEntry e{0, ""};
        e.ucTime.time = std::chrono::nanoseconds{0};
        std::mt19937 gen{seed != nullptr
                           ? static_cast<std::uint32_t>(std::strtoul(seed, nullptr, 10))
                           : std::random_device{}()};
        std::size_t  loops{};
        std::size_t  rounds{frozen != nullptr
                              ? static_cast<std::size_t>(std::strtoull(frozen, nullptr, 10))
                              : std::size_t{}};

        // GUI_TEST_PRELOAD=1000000 bulk-loads entries as fast as possible to test
        // filter/scroll performance on large logs.
        if(char const* env = uc_log::printer::environment("GUI_TEST_PRELOAD")) {
            auto const preload = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
            for(std::size_t i = 0; i != preload && !stoken.stop_requested(); ++i) {
                updateMessage(e, gen);
                if(i % 100 == 0) { e.logMsg = "multi\nline\nmessage"; }
                e.ucTime.time += std::chrono::milliseconds{1};
                gui.add(std::chrono::system_clock::now(), e);
            }
        }

        while(!stoken.stop_requested()) {
            if(frozen != nullptr) {
                if(rounds == 0) {
                    std::this_thread::sleep_for(addTime);
                    continue;
                }
                --rounds;
            }
            updateMessage(e, gen);
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

            // GUI_TEST_FORMAT_ERRORS=1: a format error every 2 s
            if(uc_log::printer::environment("GUI_TEST_FORMAT_ERRORS") != nullptr
               && ++loops % 20 == 0)
            {
                messageLog.formatError(
                  "bad format for replacement field \"{:#06x}\": invalid format "
                  "specifier (gui_test)");
            }

            if(frozen == nullptr) { std::this_thread::sleep_for(addTime); }
        }
    });

    return gui.run(reader, "make", "", "board-a");
}
