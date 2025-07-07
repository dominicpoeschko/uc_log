#include "uc_log/Gui.hpp"

#include <chrono>
#include <random>
#include <string_view>
#include <thread>

struct Status {
    std::uint32_t numBytesTransferred{};
    std::uint32_t numBytesRead{};
    int           hostOverflowCount{};
    int           isRunning{};
    int           numUpBuffers{};
    int           numDownBuffers{};
    std::uint32_t overflowMask{};
    std::uint32_t padding{};
};

struct Reader {
    std::mutex statusMutex{};
    Status     status;

    Status getStatus() {
        std::lock_guard lock{statusMutex};
        return status;
    }

    void resetJLink() {
        if(msg) {
            msg("Reset debugger");
        }
        {
            std::lock_guard lock{statusMutex};
            status.hostOverflowCount = 0;
            status.isRunning         = true;
        }
        if(msg) {
            msg("Reset debugger done");
        }
    }

    void resetTarget() {
        if(msg) {
            msg("Reset target");
        }
        {
            std::lock_guard lock{statusMutex};
            status.numBytesTransferred = 0;
            status.numBytesRead        = 0;
        }
        if(msg) {
            msg("Reset target done");
        }
    }

    void flash() {
        if(msg) {
            msg("Flash target");
            msg("Flash target done");
        }
    }

    std::function<void(std::string const&)> msg;
};

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

template<typename Gen>
void updateMessage(uc_log::detail::LogEntry& e,
                   Gen&                      gen) {
    e.channel.channel = std::uniform_int_distribution<std::size_t>{0, 5}(gen);
    e.logLevel        = static_cast<uc_log::LogLevel>(std::uniform_int_distribution<std::uint8_t>{
      static_cast<std::uint8_t>(uc_log::LogLevel::trace),
      static_cast<std::uint8_t>(uc_log::LogLevel::crit)}(gen));
    std::ranges::sample(fileNames, &e.fileName, 1, gen);
    std::ranges::sample(logMessages, &e.logMsg, 1, gen);
    std::ranges::sample(functionNames, &e.functionName, 1, gen);
    std::ranges::sample(functionLines, &e.line, 1, gen);
}

static void updateStatus(Status& status) {
    status.numBytesTransferred += 10;
    status.numBytesRead += 10;

    if(status.numBytesTransferred > 1000) {
        status.isRunning = false;
    }
}

int main(int    argc,
         char** argv) {
    (void)argv;
    uc_log::Gui gui{argc == 1 ? "ftxui" : "simple"};

    Reader reader;

    std::vector<std::string> messages;
    std::mutex               msgMutex;

    reader.msg = [&](std::string const& msg) {
        std::lock_guard lock{msgMutex};
        messages.push_back(msg);
    };

    auto addTime = std::chrono::milliseconds{100};

    std::jthread runner([&](std::stop_token stoken) {
        uc_log::detail::LogEntry e{0, ""};
        e.ucTime.time = std::chrono::nanoseconds{0};
        std::mt19937 gen{std::random_device{}()};
        while(!stoken.stop_requested()) {
            updateMessage(e, gen);
            {
                std::lock_guard lock{reader.statusMutex};
                updateStatus(reader.status);
            }
            e.ucTime.time += addTime;
            gui.add(std::chrono::system_clock::now(), e);

            std::vector<std::string> localMessages;
            {
                std::lock_guard lock{msgMutex};
                localMessages = messages;
                messages.clear();
            }

            for(auto const& msg : localMessages) {
                gui.statusMessage(msg);
                gui.errorMessage(msg);
                gui.fatalError(msg);
            }

            std::this_thread::sleep_for(addTime);
        }
    });

    return gui.run(reader, "make");
}
