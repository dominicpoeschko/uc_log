// Reader-loop tests with an in-memory fake transport: frame decode end to end, quiet
// target must NOT reconnect, overflow must inject a parsable marker entry, and a lost
// connection must reconnect.
#include "remote_fmt/type_identifier.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <expected>
#include <functional>
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
        std::atomic<bool>     halted{false};
        std::atomic<int>      flashFailures{0};   // the next so many flash() calls throw
        std::atomic<int>      flashes{0};
        std::atomic<int>      resets{0};
        std::atomic<int>      ramFlashes{0};    // flashRamImage(): reset, download, start
        JLink::RamImageStart  lastRamStart{};   // guarded by mutex
        std::atomic<int>      haltPolls{0};
        std::atomic<bool>     stackUnreadable{false};    // reads near sp throw
        std::atomic<bool>     holdSessionStart{false};   // startRtt waits while set
        std::atomic<bool> coreWasReset{false};   // DHCSR.S_RESET_ST: set by a reset, read clears it
        std::atomic<bool> blockGone{false};      // no RTT id at the block address (a boot ROM runs)
        std::function<void()>           onReset;
        std::vector<JLink::MemoryWrite> preResetAtLastReset;           // guarded by mutex
        std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;   // guarded by mutex
        // the RTT control block at 0x20000100 with one buffer, as words (id, counts, descriptor);
        // empty: the address pattern, which holds no "SEGGER RTT" id
        std::optional<std::array<std::uint32_t, 12>> controlBlock;   // guarded by mutex

        // an announced reset: the block at AnnounceAddress, words magic armed request ack
        static constexpr std::uint32_t AnnounceAddress = 0x2000'0300;
        std::array<std::uint32_t, 4>   announce{};         // guarded by mutex
        std::atomic<bool>              resetting{false};   // set by the ack: the chip resets
        std::atomic<std::chrono::steady_clock::time_point> ackedAt{};   // when the ack was written
        std::atomic<int>                                   touchedWhileResetting{0};
        std::atomic<int>                                   open{0};   // transports alive
        std::atomic<int>                                   closedWhileResetting{0};
        std::atomic<int>                                   rttStops{0};
        // the firmware's wait for the ack runs out right after the printer's first read of a
        // request: the chip resets, and the new boot's .data holds a fresh block
        std::atomic<bool> gaveUpBeforeAck{false};

        // every call that reaches the target; one while the chip resets is the bug
        void touch() {
            if(resetting) { ++touchedWhileResetting; }
        }

        void feed(std::vector<std::byte> const& data) {
            std::lock_guard<std::mutex> const lock{mutex};
            upData.insert(upData.end(), data.begin(), data.end());
        }
    };

    static inline Shared* shared = nullptr;

    struct Connection {
        std::string   host{};
        std::uint16_t port{19020};
        std::string   probe{};
    };

    struct Probe {
        std::uint32_t serialNumber{};
        std::string   product{};
        std::string   nickName{};
        bool          onUsb{};
    };

    static std::vector<Probe> listProbes() {
        return {
          Probe{1, "fake", "fake", true}
        };
    }

    template<typename MessageF,
             typename ErrorF>
    FakeTransport(std::string const&,
                  std::uint32_t,
                  Connection const&,
                  MessageF&&,
                  ErrorF&&) {
        ++shared->constructions;
        shared->touch();   // the connect
        ++shared->open;
        checkConnected();
    }

    FakeTransport(FakeTransport const&)            = delete;
    FakeTransport& operator=(FakeTransport const&) = delete;

    // JLINK_Close
    ~FakeTransport() {
        --shared->open;
        if(shared->resetting) { ++shared->closedWhileResetting; }
    }

    void stopRtt() {
        shared->touch();
        ++shared->rttStops;
    }

    void setResetType(std::uint8_t) {}

    std::vector<JLink::MemoryWrite> preResetCommands;

    void setPreResetCommands(std::vector<JLink::MemoryWrite> commands) {
        preResetCommands = std::move(commands);
    }

    void resetTarget() {
        shared->touch();
        {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            shared->preResetAtLastReset = preResetCommands;
        }
        ++shared->resets;
        shared->coreWasReset = true;
        if(shared->onReset) { shared->onReset(); }
    }

    void flash(std::string const&) {
        shared->touch();
        if(shared->flashFailures > 0) {
            --shared->flashFailures;
            throw std::runtime_error{"fake flash failure"};
        }
        ++shared->flashes;
        shared->coreWasReset = true;
    }

    void flashRamImage(std::string const&,
                       JLink::RamImageStart const& start) {
        shared->touch();
        if(shared->flashFailures > 0) {
            --shared->flashFailures;
            throw std::runtime_error{"fake flash failure"};
        }
        {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            shared->preResetAtLastReset = preResetCommands;
            shared->lastRamStart        = start;
        }
        ++shared->ramFlashes;
        shared->coreWasReset = true;
    }

    enum class CoreRegister : int { sp = 13, lr = 14, pc = 15, xpsr = 16, msp = 17, psp = 18 };

    std::uint32_t readRegister(CoreRegister r) {
        shared->touch();
        if(r == CoreRegister::sp) { return 0x20000100; }
        return 0x1000U + static_cast<std::uint32_t>(r);
    }

    void go() { shared->touch(); }

    void halt() { shared->touch(); }

    void clearAllBreakpoints() { shared->touch(); }

    bool isHalted() {
        shared->touch();
        ++shared->haltPolls;
        return shared->halted;
    }

    void writeWord(std::uint32_t address,
                   std::uint32_t value) {
        shared->touch();
        std::lock_guard<std::mutex> const lock{shared->mutex};
        shared->writes.emplace_back(address, value);
        if(address == 0x20000100 && value == 0) { shared->blockGone = true; }   // the id cleared
        if(shared->controlBlock && address >= 0x20000100 && address < 0x20000100 + 48) {
            (*shared->controlBlock)[(address - 0x20000100) / 4] = value;
        }
        if(address >= Shared::AnnounceAddress && address < Shared::AnnounceAddress + 16) {
            shared->announce[(address - Shared::AnnounceAddress) / 4] = value;
            if(address == Shared::AnnounceAddress + 12) {   // the ack
                shared->ackedAt   = std::chrono::steady_clock::now();
                shared->resetting = true;
            }
        }
    }

    void readMemory(std::uint32_t        address,
                    std::span<std::byte> out) {
        shared->touch();
        if(address == Shared::AnnounceAddress && out.size() == 16) {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            for(std::size_t i = 0; i != out.size(); ++i) {
                out[i] = static_cast<std::byte>(shared->announce[i / 4] >> (8 * (i % 4)));
            }
            if(shared->gaveUpBeforeAck && shared->announce[2] != 0) {
                shared->gaveUpBeforeAck = false;
                shared->announce        = {uc_log::detail::announced_reset::Magic, 0, 0, 0};
            }
            return;
        }
        if(shared->stackUnreadable && address >= 0x20000100 && address < 0x20000200) {
            throw std::runtime_error{"JLINK_ReadMem: -1"};
        }
        if(address == 0x20000100 && out.size() == 10) {   // the id check, not the block dump
            std::string_view const id{shared->blockGone ? "\0\0\0\0\0\0\0\0\0\0" : "SEGGER RTT",
                                      10};
            for(std::size_t i = 0; i != out.size(); ++i) { out[i] = static_cast<std::byte>(id[i]); }
            return;
        }
        if(address == 0x20000100 && out.size() == 48) {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            if(shared->controlBlock) {
                for(std::size_t i = 0; i != out.size(); ++i) {
                    out[i]
                      = static_cast<std::byte>((*shared->controlBlock)[i / 4] >> (8 * (i % 4)));
                }
                return;
            }
        }
        if(address == 0xE000'EDF0 && out.size() == 4) {   // DHCSR: S_RETIRE_ST, S_RESET_ST
            std::uint32_t const dhcsr
              = 0x0100'0000U | (shared->coreWasReset.exchange(false) ? 1U << 25 : 0U);
            for(std::size_t i = 0; i != out.size(); ++i) {
                out[i] = static_cast<std::byte>(dhcsr >> (8 * i));
            }
            return;
        }
        for(std::size_t i = 0; i != out.size(); ++i) {
            out[i] = static_cast<std::byte>((address + i) & 0xFFU);
        }
    }

    void checkConnected() {
        shared->touch();
        std::lock_guard<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
    }

    FakeStatus readStatus() {
        shared->touch();
        std::lock_guard<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
        return shared->status;
    }

    FakeStatus startRtt(std::uint32_t,
                        std::uint32_t) {
        while(shared->holdSessionStart) { std::this_thread::sleep_for(1ms); }
        return readStatus();
    }

    std::optional<BufferDesc> rttBufferDesc(bool,
                                            std::uint32_t) {
        return std::nullopt;   // positional fallback path
    }

    std::span<std::byte> rttRead(std::uint32_t,
                                 std::span<std::byte> buffer) {
        shared->touch();
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
        shared->touch();
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
    std::vector<std::string> tools;   // the tool's own messages (toolMessageCallback)
    std::vector<std::string> errors;

    bool anyEntryContains(std::string_view needle) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(entries, [&](auto const& e) {
            return e.find(needle) != std::string::npos;
        });
    }

    bool anyErrorContains(std::string_view needle) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(errors, [&](auto const& e) {
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
    // never freed: FakeTransport::shared points at it until the program ends (still reachable, not a leak)
    FakeTransport::shared = new FakeTransport::Shared;
    auto& shared          = *FakeTransport::shared;

    Collected collected;

    BasicJLinkRttReader<FakeTransport> reader{
      "",
      "fake",
      4000,
      "",
      []() { return RttBlockInfo{0x20000100, 1}; },
      []() { return std::string{"fake.hex"}; },
      []() {
          return std::unordered_map<std::uint16_t, std::string>{
            {0, "hello from target"},
          };
      },
      [&collected](std::size_t, std::string_view msg, std::optional<remote_fmt::catalog_id>) {
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
      [&collected](std::string_view msg) {
          std::lock_guard<std::mutex> const lock{collected.mutex};
          collected.tools.emplace_back(msg);
      },
      [](std::string_view) {}};

    using Reader = BasicJLinkRttReader<FakeTransport>;
    reader.setNoLogTimeout(1);
    std::atomic<int>      sessionStarts{0};
    std::atomic<unsigned> sessionByte{0};
    reader.setOnSessionStart([&](auto const& read) {
        std::array<std::byte, 1> b{};
        if(read(0x20000042, b)) { sessionByte = std::to_integer<unsigned>(b[0]); }
        ++sessionStarts;
    });

    // frames decode end to end
    shared.feed(makeFrame(0));
    CHECK(waitFor([&]() { return collected.anyEntryContains("hello from target"); }, 3000ms),
          "frame decoded through the reader loop");

    // a quiet-but-alive target must not reconnect (the old code tore down every 15 s);
    // it must only warn
    CHECK(waitFor([&]() { return collected.anyMessageContains("no log messages"); }, 4000ms),
          "quiet warning emitted");
    CHECK(shared.constructions.load() == 1, "no reconnect while the link is alive");

    {
        std::array<Reader::MemoryAccess, 2> const pieces{
          Reader::MemoryAccess{0x20000010, 4},
          Reader::MemoryAccess{0x200000FE, 3}
        };
        auto const result = reader.accessMemory(pieces, 2000ms);
        CHECK(result.has_value(), "memory read through the reader thread");
        if(result) {
            CHECK(result->size() == 2 && (*result)[0].size() == 4 && (*result)[1].size() == 3,
                  "one buffer per piece, of its size");
            CHECK((*result)[0][0] == std::byte{0x10} && (*result)[0][3] == std::byte{0x13}
                    && (*result)[1][2] == std::byte{0x00},
                  "the fake target's bytes: the low byte of each address");
        }

        std::array<Reader::MemoryAccess, 2> const writes{
          Reader::MemoryAccess{0x5000'000C, 4, 0U},
          Reader::MemoryAccess{0x5000'0010, 4, 7U}
        };
        auto const written = reader.accessMemory(writes, 2000ms);
        CHECK(written.has_value() && written->size() == 2 && (*written)[1].size() == 4
                && (*written)[1][0] == std::byte{0x10},
              "a word per write, read back from the target");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            CHECK((shared.writes
                   == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x5000'000CU, 0U},
                                                                          {0x5000'0010U, 7U}}),
                  "both words written, in order");
        }
    }

    {
        auto const haltEntries = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            std::vector<std::string>          found;
            for(auto const& e : collected.errors) {
                if(e.find("core halted") != std::string::npos) { found.push_back(e); }
            }
            return found;
        };
        // the reader has polled isHalted() at least n more times
        auto const polled = [&](int n) {
            auto const from = shared.haltPolls.load();
            return waitFor([&]() { return shared.haltPolls.load() >= from + n; }, 3000ms);
        };

        shared.halted = true;
        CHECK(waitFor([&]() { return !haltEntries().empty(); }, 3000ms),
              "halt captured as a status error");
        CHECK(polled(20), "the reader polls on while halted");
        auto entries = haltEntries();
        CHECK(entries.size() == 1, "one entry per halt, not one per poll");
        std::string const entry = entries.empty() ? std::string{} : entries.front();
        CHECK(entry.find("pc=0x0000100f") != std::string::npos
                && entry.find("sp=0x20000100") != std::string::npos,
              "the core registers");
        CHECK(entry.find("stack: 03020100 07060504") != std::string::npos, "the top of the stack");
        CHECK(!collected.anyEntryContains("core halted"),
              "the log holds the target's lines only: no halt entry in it");
        CHECK(reader.isHalted(), "isHalted()");
        shared.halted = false;
        CHECK(polled(2), "the reader saw the core run again");

        // SP outside RAM: the stack reads fail, the entry is still written, the session lives
        auto const sessions      = reader.sessionCount();
        auto const constructions = shared.constructions.load();
        shared.stackUnreadable   = true;
        shared.halted            = true;
        CHECK(waitFor([&]() { return haltEntries().size() == 2; }, 3000ms),
              "a halt with an unreadable stack is still reported");
        CHECK(polled(20), "the reader polls on after an unreadable stack");
        entries = haltEntries();
        CHECK(entries.size() == 2, "still one entry per halt");
        std::string const unreadable = entries.size() == 2 ? entries.back() : std::string{};
        CHECK(unreadable.find("pc=0x0000100f") != std::string::npos
                && unreadable.find("lr=0x0000100e") != std::string::npos,
              "pc and lr are reported without the stack");
        CHECK(unreadable.find("stack: ???????? ????????") != std::string::npos,
              "unreadable words are marked");
        CHECK(reader.sessionCount() == sessions && shared.constructions.load() == constructions,
              "no session restart");
        shared.halted          = false;
        shared.stackUnreadable = false;
        CHECK(polled(2), "the reader saw the core run again");
    }

    {
        std::vector<JLink::MemoryWrite> const commands{
          JLink::MemoryWrite{0x4001A004, 0x01000000},
          JLink::MemoryWrite{0x4001B004, 0x01000000}
        };
        reader.setPreResetCommands(commands);
        std::atomic<std::uint64_t> sessionAtReset{0};
        shared.onReset    = [&]() { sessionAtReset = reader.sessionCount(); };
        auto const before = reader.sessionCount();
        auto const r      = reader.resetAndWait(5000ms);
        CHECK(r.has_value() && shared.resets.load() >= 1 && reader.sessionCount() > before,
              "resetAndWait: a new session");
        CHECK(reader.sessionCount() > sessionAtReset.load(),
              "resetAndWait returns with a session begun after the reset");

        // a reset requested while a session is starting: that session is not the reset's
        shared.holdSessionStart = true;
        auto const built        = shared.constructions.load();
        reader.resetJLink();
        CHECK(waitFor([&]() { return shared.constructions.load() > built; }, 3000ms),
              "a session is starting");
        auto const                       resetsBefore = shared.resets.load();
        std::expected<void, std::string> during{std::unexpected{std::string{"not run"}}};
        std::jthread                     requester{[&]() { during = reader.resetAndWait(5000ms); }};
        std::this_thread::sleep_for(50ms);
        shared.holdSessionStart = false;
        requester.join();
        CHECK(during.has_value() && shared.resets.load() > resetsBefore,
              "resetAndWait during a session start waits for its reset");
        CHECK(reader.sessionCount() > sessionAtReset.load(), "and for a session begun after it");
        shared.onReset = nullptr;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            CHECK(shared.preResetAtLastReset == commands,
                  "the pre-reset commands reach the transport before its reset");
        }
        auto const f = reader.flashAndWait(8000ms);
        CHECK(f.has_value() && shared.flashes.load() == 1, "flashAndWait flashes once");
        shared.flashFailures = 1;
        auto const again     = reader.flashAndWait(12000ms);
        CHECK(again.has_value() && shared.flashes.load() == 2, "one failed download is retried");
        shared.flashFailures  = 3;
        auto const beforeFail = reader.sessionCount();
        auto const failed     = reader.flashAndWait(12000ms);
        CHECK(waitFor([&]() { return reader.sessionCount() > beforeFail; }, 5000ms),
              "after a failed flash the log comes back");
        CHECK(!failed.has_value() && failed.error().find("fake flash failure") != std::string::npos,
              "three failed downloads are reported, not retried for ever");
        CHECK(shared.flashes.load() == 2 && !reader.isFlashing(),
              "and the flash request is dropped");
        CHECK(sessionStarts.load() >= 2 && sessionByte.load() == 0x42, "session start callback");

        // a RAM image: flash and reset load it and start it, and never reset after that
        JLink::RamImageStart const start{0x2000'0000, 0x2008'2000, 0x2000'0199};
        CHECK(!reader.isRamImage(), "an ordinary image by default");
        reader.setRamImage([&]() { return Reader::RamImageStartResult{start}; });
        CHECK(reader.isRamImage(), "a RAM image once told");
        auto const ramResetsBefore = shared.resets.load();
        auto const flashesBefore   = shared.flashes.load();
        auto const sessionsRam     = reader.sessionCount();
        auto const ram             = reader.flashAndWait(8000ms);
        CHECK(ram.has_value() && shared.ramFlashes.load() == 1,
              "a RAM image is loaded and started");
        CHECK(reader.sessionCount() > sessionsRam, "and its log comes back");
        CHECK(shared.resets.load() == ramResetsBefore && shared.flashes.load() == flashesBefore,
              "no reset after it (that would boot flash), no plain download");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            CHECK(shared.lastRamStart == start, "started at its vector table");
            CHECK(shared.preResetAtLastReset == commands, "the pre-reset commands are there too");
        }
        auto const ramReset = reader.resetAndWait(5000ms);
        CHECK(ramReset.has_value() && shared.ramFlashes.load() == 2
                && shared.resets.load() == ramResetsBefore,
              "a reset of a RAM image loads it again instead of a reset");

        // where it starts is unknown (no map symbol, no table in the hex): told, not tried
        reader.setRamImage(
          []() { return Reader::RamImageStartResult{std::unexpected{std::string{"no map"}}}; });
        auto const t0       = std::chrono::steady_clock::now();
        auto const noReset  = reader.resetAndWait(5000ms);
        auto const tookLong = std::chrono::steady_clock::now() - t0 > 3000ms;
        CHECK(!noReset && noReset.error() == "not reset: no map" && !tookLong,
              "a reset says why at once");
        auto const noFlash = reader.flashAndWait(12000ms);
        CHECK(!noFlash && noFlash.error() == "no map", "so does a flash");
        CHECK(shared.ramFlashes.load() == 2 && shared.resets.load() == ramResetsBefore,
              "and neither touches the target");
        reader.setRamImage({});
        CHECK(!reader.isRamImage(), "an ordinary image again");
        // the failed download ended the session: the next checks need a running one
        CHECK(waitFor([&]() { return reader.getStatus().isRunning != 0; }, 5000ms),
              "the log comes back after the refused flash");
    }

    {
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.status.hostOverflowCount += 2;
        }
        CHECK(waitFor([&]() { return collected.anyErrorContains("RTT host overflow"); }, 3000ms),
              "overflow told as a status error");
        // the fake memory holds no "SEGGER RTT" id
        CHECK(
          waitFor([&]() { return collected.anyErrorContains("RTT control block at 0x20000100"); },
                  3000ms),
          "control block dump follows the data loss");
        CHECK(collected.anyErrorContains("CORRUPT"), "a block without the RTT id is corrupt");
        CHECK(!collected.anyEntryContains("RTT host overflow")
                && !collected.anyEntryContains("RTT control block"),
              "neither of them is an entry of the log");
        std::size_t before = 0;
        {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            before
              = static_cast<std::size_t>(std::ranges::count_if(collected.errors, [](auto const& e) {
                    return e.find("RTT host overflow") != std::string::npos;
                }));
        }
        for(int i = 0; i != 20; ++i) {
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.status.hostOverflowCount += 1;
            }
            std::this_thread::sleep_for(10ms);
        }
        {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            auto const                        after
              = static_cast<std::size_t>(std::ranges::count_if(collected.errors, [](auto const& e) {
                    return e.find("RTT host overflow") != std::string::npos;
                }));
            CHECK(after == before, "a running overflow is summarised, not repeated");
        }
    }

    // the host's offset in the control block out of range (the channel would be dead for good): set
    // to the target's offset at the next loss
    {
        auto const block = [](std::uint32_t wr, std::uint32_t rd) {
            return std::array<std::uint32_t, 12>{0x47474553U,   // "SEGGER RTT\0..."
                                                 0x52205245U,
                                                 0x00005454U,
                                                 0U,
                                                 1U,   // up
                                                 0U,   // down
                                                 0U,   // name
                                                 0x20000400U,
                                                 1024U,
                                                 wr,
                                                 rd,
                                                 0U};
        };
        auto const loss = [&]() {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.status.hostOverflowCount += 1;
        };
        auto const word = [&](std::size_t i) {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            return (*shared.controlBlock)[i];
        };
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.controlBlock = block(334, 3222675632U);
        }
        loss();
        CHECK(waitFor([&]() { return word(10) == 334; }, 3000ms),
              "an up buffer's read offset out of range is set to the write offset");
        CHECK(
          collected.anyErrorContains("the host's read offset was 3222675632 in a 1024-byte ring"),
          "and said so");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.controlBlock = block(5000, 7);   // the target's own offset: its RAM, not ours
        }
        loss();
        std::this_thread::sleep_for(300ms);
        CHECK(word(9) == 5000 && word(10) == 7, "a target-owned offset out of range is left alone");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.controlBlock.reset();
        }
    }

    // a reset from outside (picotool, a watchdog): SEGGER's reader would read an old lap of a
    // ring once the new image rewrites the control block, so the session starts over - after the
    // lines already read, without the rest of a line the reset cut off
    {
        CHECK(!collected.anyMessageContains("reset from outside"),
              "the printer's own resets and flashes are not taken for one from outside");
        auto const sessions = reader.sessionCount();
        auto const built    = shared.constructions.load();
        auto const hellos   = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.entries, [](auto const& e) {
                return e.find("hello from target") != std::string::npos;
            });
        };
        auto const before = hellos();
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            auto const                        frame = makeFrame(0);
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.end());
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.begin() + 2);
            shared.coreWasReset = true;
        }
        CHECK(waitFor([&]() { return collected.anyMessageContains("reset from outside"); }, 3000ms),
              "a reset from outside is told as a status message");
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions; }, 3000ms)
                && shared.constructions.load() > built,
              "and a new session starts");
        CHECK(hellos() == before + 1, "the line read before the reset is kept, once");
        CHECK(collected.anyMessageContains("2 bytes of a line cut off by the reset dropped"),
              "the cut-off line is dropped and counted");
        CHECK(!collected.anyErrorContains("resync"), "and does not show up as a stuck frame");
        auto const after = reader.sessionCount();
        std::this_thread::sleep_for(200ms);
        CHECK(reader.sessionCount() == after, "one new session, not one per pass");

        // into a boot ROM that clears the block: the next session waits for it, quietly
        shared.blockGone    = true;
        shared.coreWasReset = true;
        CHECK(waitFor(
                [&]() { return collected.anyMessageContains("waiting for the RTT control block"); },
                3000ms),
              "a session after a reset from outside waits for the control block");
        auto const waiting = reader.sessionCount();
        std::this_thread::sleep_for(300ms);
        CHECK(reader.sessionCount() == waiting, "and starts no session while it is gone");
        {
            std::array<Reader::MemoryAccess, 1> const piece{
              Reader::MemoryAccess{0x20000010, 4}
            };
            CHECK(reader.accessMemory(piece, 2000ms).has_value(),
                  "memory reads are served meanwhile");
        }
        shared.blockGone = false;
        CHECK(waitFor([&]() { return reader.sessionCount() > waiting; }, 3000ms),
              "the session starts once the block is back");
    }

    // an announced reset (Kvasir_SDK Util/AnnouncedReset.hpp): the printer arms the block, and on
    // a request drains the log, stops RTT, clears the control block id, writes the ack and closes
    // the session - and touches nothing until the pause is over, then connects again
    {
        using namespace uc_log::detail::announced_reset;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce = {Magic, 0, 0, 0};
        }
        reader.setAnnouncedReset(
          []() -> std::optional<std::uint32_t> { return FakeTransport::Shared::AnnounceAddress; });
        auto const sessions = reader.sessionCount();
        reader.resetJLink();
        auto const armed = [&]() {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == HostArmed;
        };
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions && armed(); }, 3000ms),
              "a session start arms the block");
        auto const hellos = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.entries, [](auto const& e) {
                return e.find("hello from target") != std::string::npos;
            });
        };
        std::this_thread::sleep_for(100ms);
        CHECK(!reader.isPausedForReset(), "no pause without a request");
        auto const before = hellos();
        auto const built  = shared.constructions.load();
        {
            // the firmware's last line, then its request
            std::lock_guard<std::mutex> const lock{shared.mutex};
            auto const                        frame = makeFrame(0);
            shared.upData.insert(shared.upData.end(), frame.begin(), frame.end());
            shared.announce[2] = 1;
        }
        CHECK(waitFor([&]() { return shared.resetting.load(); }, 3000ms), "the ack is written");
        auto const ackedAt = shared.ackedAt.load();   // the write itself, not when we noticed it
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            CHECK(shared.announce[3] == 1, "ack = request");
            using Write           = std::pair<std::uint32_t, std::uint32_t>;
            auto const  idCleared = std::ranges::find(shared.writes, Write{0x20000100U, 0U});
            Write const ackWrite  = {FakeTransport::Shared::AnnounceAddress + 12, 1U};
            CHECK(idCleared != shared.writes.end(), "the control block's id is cleared");
            CHECK(!shared.writes.empty() && shared.writes.back() == ackWrite,
                  "the ack is the last write");
        }
        CHECK(shared.rttStops.load() >= 1, "RTT is stopped before it");
        CHECK(hellos() == before + 1, "the last line before the reset is in the log");
        CHECK(collected.anyMessageContains("the target announced a reset"),
              "told as a status message");
        CHECK(waitFor([&]() { return shared.open.load() == 0; }, 1000ms), "the session is closed");
        CHECK(reader.isPausedForReset(), "the reader is paused");
        {
            std::array<Reader::MemoryAccess, 1> const piece{
              Reader::MemoryAccess{0x20000010, 4}
            };
            auto const t0     = std::chrono::steady_clock::now();
            auto const result = reader.accessMemory(piece, 2000ms);
            CHECK(!result && result.error().find("announced reset") != std::string::npos
                    && std::chrono::steady_clock::now() - t0 < 100ms,
                  "a memory access during the pause fails at once");
        }
        // the chip reboots for ~1 s; then the new boot's .data: a fresh block and the RTT id
        std::this_thread::sleep_until(ackedAt + 1000ms);
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce = {Magic, 0, 0, 0};
        }
        shared.resetting = false;
        shared.blockGone = false;
        CHECK(shared.touchedWhileResetting.load() == 0, "no DLL call touched the resetting target");
        CHECK(shared.constructions.load() == built, "no reconnect during the pause");
        CHECK(waitFor([&]() { return shared.constructions.load() > built && armed(); }, 3000ms),
              "connected again after the pause, and armed again");
        CHECK(std::chrono::steady_clock::now() - ackedAt >= Reader::AnnouncedResetPause,
              "not before the pause is over");
        CHECK(!reader.isPausedForReset(), "the pause is over");
        shared.feed(makeFrame(0));
        CHECK(waitFor([&]() { return hellos() == before + 2; }, 3000ms), "the log runs again");
    }

    // a request the printer answers too late - it was busy (a halt it caught, a slow probe) and
    // the firmware's bounded wait ran out: the chip has reset already. The printer must not write
    // into the new boot: a cleared id there would hide its control block from every session
    {
        using namespace uc_log::detail::announced_reset;
        auto const armed = [&]() {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == HostArmed;
        };
        CHECK(waitFor(armed, 3000ms), "armed");
        std::size_t writesBefore{};
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            writesBefore           = shared.writes.size();
            shared.gaveUpBeforeAck = true;
            shared.announce[2]     = 2;
        }
        CHECK(
          waitFor(
            [&]() { return collected.anyMessageContains("did it before the printer answered"); },
            3000ms),
          "told that the reset came first");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            using Write      = std::pair<std::uint32_t, std::uint32_t>;
            auto const after = std::ranges::subrange(shared.writes.begin()
                                                       + static_cast<std::ptrdiff_t>(writesBefore),
                                                     shared.writes.end());
            CHECK(std::ranges::find(after, Write{0x20000100U, 0U}) == after.end(),
                  "the new boot's control block keeps its id");
            CHECK(std::ranges::find_if(after,
                                       [](Write const& w) {
                                           return w.first
                                               == FakeTransport::Shared::AnnounceAddress + 12;
                                       })
                    == after.end(),
                  "no ack into the new boot's block");
        }
        CHECK(!shared.blockGone.load(), "the block is still there for the next session");
        CHECK(waitFor(armed, 3000ms), "the next session arms the new boot's block");
    }

    // a session that starts before the boot has set the block up (right after a flash, .data not
    // copied yet) looks for it again and arms it once it is there - in the same session, and a
    // request afterwards is served as usual (on the RP2040 a reset the printer does not know of
    // ends in the Rescue DP, so a session must never stay unarmed)
    {
        using namespace uc_log::detail::announced_reset;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce = {0, 0, 0, 0};
        }
        auto const sessions = reader.sessionCount();
        auto const notYet   = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return std::ranges::count_if(collected.tools, [](auto const& m) {
                return m.find("announced resets: no block at") != std::string::npos;
            });
        };
        auto const notYetBefore = notYet();
        reader.resetJLink();
        CHECK(waitFor([&]() { return reader.sessionCount() > sessions; }, 3000ms),
              "a session starts without the block");
        auto const armed = [&]() {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            return shared.announce[1] == HostArmed;
        };
        std::this_thread::sleep_for(150ms);
        CHECK(!armed(), "nothing armed while the block is not there");
        CHECK(notYet() == notYetBefore + 1, "said once, not once per look");
        auto const session = reader.sessionCount();
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce = {Magic, 0, 0, 0};
        }
        CHECK(waitFor(armed, 500ms), "armed once the block is set up");
        CHECK(reader.sessionCount() == session, "in the same session");
        CHECK(notYet() == notYetBefore + 1, "no further message");
        auto const built = shared.constructions.load();
        {
            // a watchdog left to run out: the request asks the printer to stay away 3 s
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce[2] = (30U << 16) | 1U;
        }
        CHECK(waitFor([&]() { return shared.resetting.load(); }, 3000ms),
              "a request after the late arming is acked");
        auto const ackedAt = shared.ackedAt.load();
        CHECK(shared.announce[3] == ((30U << 16) | 1U), "the whole request word is the ack");
        CHECK(collected.anyMessageContains("the probe stays off it for 3000 ms"),
              "the time the firmware asked for is told");
        std::this_thread::sleep_for(1000ms);
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.announce = {Magic, 0, 0, 0};
        }
        shared.resetting = false;
        shared.blockGone = false;
        CHECK(waitFor([&]() { return shared.constructions.load() > built && armed(); }, 5000ms),
              "and the session after the pause is armed again");
        CHECK(std::chrono::steady_clock::now() - ackedAt >= 3000ms,
              "not before the 3 s the firmware asked for");
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

    // byte counters that went down (restarted by the DLL) are not a wrap to ~4 G
    {
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.status.numBytesTransferred = 0;
            shared.status.numBytesRead        = 0;
            shared.status.hostOverflowCount += 1;
        }
        CHECK(waitFor(
                [&]() {
                    return collected.anyErrorContains(
                      "(1 event; since the last report 0 bytes from the target, 0 read");
                },
                3000ms),
              "a restarted counter counts from zero");
        CHECK(!collected.anyErrorContains("42949"), "no wrapped byte count");
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
