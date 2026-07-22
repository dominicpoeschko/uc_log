// Reader-loop tests with an in-memory fake transport: frame decode end to end, quiet
// target must NOT reconnect, overflow must inject a parsable marker entry, and a lost
// connection must reconnect.
#include "remote_fmt/type_identifier.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

struct FakeStatus {
    std::uint32_t numBytesTransferred{};
    std::uint32_t numBytesRead{};
    int           hostOverflowCount{};
    int           isRunning{1};
    int           numUpBuffers{1};
    int           numDownBuffers{0};
    std::uint32_t overflowMask{};
};

struct FakeTransport {
    using Status = FakeStatus;

    struct BufferDesc {
        std::string name;
    };

    struct Shared {
        std::mutex            mutex;
        std::deque<std::byte> upData;
        FakeStatus            status{};
        bool                  connected{true};
        std::atomic<int>      constructions{0};

        void feed(std::vector<std::byte> const& data) {
            std::lock_guard<std::mutex> const lock{mutex};
            upData.insert(upData.end(), data.begin(), data.end());
        }
    };

    static inline Shared* shared = nullptr;

    template<typename MessageF,
             typename ErrorF>
    FakeTransport(std::string const&,
                  std::uint32_t,
                  MessageF&&,
                  ErrorF&&) {
        ++shared->constructions;
        checkConnected();
    }

    template<typename MessageF,
             typename ErrorF>
    FakeTransport(std::string const&,
                  std::uint32_t,
                  std::string const&,
                  MessageF&&,
                  ErrorF&&) {
        ++shared->constructions;
        checkConnected();
    }

    void setResetType(std::uint8_t) {}

    void resetTarget() {}

    void flash(std::string const&) {}

    void go() {}

    void halt() {}

    void clearAllBreakpoints() {}

    bool isHalted() { return false; }

    void checkConnected() {
        std::lock_guard<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
    }

    FakeStatus readStatus() {
        std::lock_guard<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
        return shared->status;
    }

    FakeStatus startRtt(std::uint32_t,
                        std::uint32_t) {
        return readStatus();
    }

    std::optional<BufferDesc> rttBufferDesc(bool,
                                            std::uint32_t) {
        return std::nullopt;   // positional fallback path
    }

    std::span<std::byte> rttRead(std::uint32_t,
                                 std::span<std::byte> buffer) {
        std::lock_guard<std::mutex> const lock{shared->mutex};
        std::size_t const                 n = std::min(buffer.size(), shared->upData.size());
        for(std::size_t i = 0; i < n; ++i) {
            buffer[i] = shared->upData.front();
            shared->upData.pop_front();
        }
        shared->status.numBytesTransferred += static_cast<std::uint32_t>(n);
        shared->status.numBytesRead += static_cast<std::uint32_t>(n);
        return buffer.first(n);
    }

    std::size_t rttWrite(std::uint32_t,
                         std::span<std::byte const> data) {
        return data.size();
    }
};

// one cataloged remote_fmt frame with no arguments
static std::vector<std::byte> makeFrame(std::uint8_t catalogId) {
    auto const typeByte = remote_fmt::detail::fmtStringTypeIdentifier<
      remote_fmt::detail::FmtStringType::cataloged_normal>(remote_fmt::detail::RangeSize::_1);
    return {std::byte{0x55}, typeByte, std::byte{catalogId}, std::byte{0xAA}};
}

struct Collected {
    std::mutex               mutex;
    std::vector<std::string> entries;
    std::vector<std::string> messages;
    std::vector<std::string> errors;

    bool anyEntryContains(std::string_view needle) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(entries, [&](auto const& e) {
            return e.find(needle) != std::string::npos;
        });
    }

    bool anyMessageContains(std::string_view needle) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(messages, [&](auto const& m) {
            return m.find(needle) != std::string::npos;
        });
    }
};

template<typename Predicate>
static bool waitFor(Predicate&&               predicate,
                    std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline) {
        if(predicate()) { return true; }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

int main() {
    FakeTransport::Shared shared;
    FakeTransport::shared = &shared;

    Collected collected;

    BasicJLinkRttReader<FakeTransport> reader{
      "",
      "fake",
      4000,
      []() { return RttBlockInfo{0, 1}; },
      []() { return std::string{"fake.hex"}; },
      []() {
          return std::unordered_map<std::uint16_t, std::string>{
            {0, "hello from target"},
          };
      },
      [&collected](std::size_t, std::string_view msg) {
          std::lock_guard<std::mutex> const lock{collected.mutex};
          collected.entries.emplace_back(msg);
      },
      [&collected](std::string_view msg) {
          std::lock_guard<std::mutex> const lock{collected.mutex};
          collected.messages.emplace_back(msg);
      },
      [&collected](std::string_view msg) {
          std::lock_guard<std::mutex> const lock{collected.mutex};
          collected.errors.emplace_back(msg);
      },
      [](std::string_view) {},
      [](std::string_view) {}};

    reader.setNoLogTimeout(1);

    // frames decode end to end
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");

    // a quiet-but-alive target must not reconnect (the old code tore down every 15 s);
    // it must only warn
    CHECK(waitFor([&]() { return collected.anyMessageContains("no log messages"); }, 4000ms),
          "quiet warning emitted");
    CHECK(shared.constructions.load() == 1, "no reconnect while the link is alive");

    // overflow must inject a parsable error-level marker into the entry stream
    {
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.status.hostOverflowCount += 2;
        }
        CHECK(waitFor([&]() { return collected.anyEntryContains("RTT host overflow"); }, 3000ms),
              "overflow marker in entry stream");
        std::string marker;
        {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            for(auto const& e : collected.entries) {
                if(e.find("RTT host overflow") != std::string::npos) { marker = e; }
            }
        }
        uc_log::detail::LogEntry const parsed{0, marker};
        CHECK(parsed.parsedOk, "marker parses as a regular entry");
        CHECK(parsed.logLevel == uc_log::LogLevel::error, "marker is error level");
    }

    // a lost connection must reconnect
    {
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.connected = false;
        }
        std::this_thread::sleep_for(100ms);
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return shared.constructions.load() >= 2; }, 5000ms),
              "reconnected after connection loss");
        shared.feed(makeFrame(0));
        auto const before = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return collected.entries.size();
        }();
        CHECK(waitFor(
                [&]() {
                    std::lock_guard<std::mutex> const lock{collected.mutex};
                    return collected.entries.size() > before;
                },
                3000ms),
              "decoding works again after reconnect");
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
