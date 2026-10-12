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
#include <cstring>
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
        // a one-shot probe failure: the next RAM read inside [failReadFrom, failReadTo) throws (as JLINK_ReadMemHW does
        // when the J-Link drops off USB), then reads work again
        std::uint32_t                                failReadFrom{};
        std::uint32_t                                failReadTo{};
        std::atomic<int>                             constructions{0};
        std::atomic<bool>                            halted{false};
        std::array<std::optional<std::uint32_t>, 19> writtenRegisters{};   // guarded by mutex
        std::atomic<int> flashFailures{0};   // the next so many flash() calls throw
        // the flash window (an RP2040's XIP): a read while the SSI word in RAM at SsiWord says the boot ROM drives the
        // flash in plain SPI mode (TMOD 0, not EEPROM_READ) hangs the ROM, as the boot ROM stall did, and reads junk
        std::atomic<bool> romHung{false};
        std::atomic<int>  resetAfterFlashReads{
          -1};   // the core resets (S_RESET_ST) right after that many flash reads
        std::atomic<int> flashReadsAtArm{
          -1};   // flash reads made when the announce block was armed
        // the DLL lost the target (every access fails -1, as after the RP2040 stall recovery's watchdog reset): a new
        // connection sees it again - or, with deadAcrossConnects, only a reset through the reset line does
        std::atomic<bool> apDead{false};
        std::atomic<bool> deadAcrossConnects{false};
        std::atomic<bool> resetLineFrees{true};
        std::atomic<int>  resetLineResets{0};
        std::atomic<bool> deadOnRecoveryWrite{
          false};   // the WATCHDOG trigger write (0x400d8000) kills the access
        // a stall only a SYSRESETREQ frees (AIRCR 0xE000ED0C = 0x05FA0004): the word at stuckWord goes to 0 then
        std::atomic<std::uint32_t> stuckWord{0};
        std::atomic<bool>          sysresetFrees{false};
        std::atomic<int>           sysresets{0};
        std::atomic<long long>     recoveryAtUs{
          0};   // when the WATCHDOG trigger was written (steady clock, us)
        std::atomic<long long> connectAfterRecoveryUs{0};   // the first connection after it
        std::atomic<int>       flashReads{0};
        // a flash interface stuck where no register shows it: every flash() throws until the core has run after a reset
        // (the boot ROM booted the image once)
        std::atomic<bool>    wedged{false};
        std::atomic<bool>    resetSinceGo{false};
        std::atomic<bool>    wedgeOutlivesBoot{false};   // a wedge nothing frees
        std::atomic<int>     flashes{0};
        std::atomic<int>     resets{0};
        std::atomic<int>     ramFlashes{0};    // flashRamImage(): reset, download, start
        JLink::RamImageStart lastRamStart{};   // guarded by mutex
        std::atomic<int>     haltPolls{0};
        std::atomic<int>     goCalls{0};
        std::atomic<bool>    stackUnreadable{false};    // reads near sp throw
        std::atomic<bool>    holdSessionStart{false};   // startRtt waits while set
        std::atomic<bool> coreWasReset{false};   // DHCSR.S_RESET_ST: set by a reset, read clears it
        std::atomic<bool> lockup{
          false};   // DHCSR.S_LOCKUP; a halted core's PC then reads 0xFFFFFFFE
        std::atomic<bool>          asleep{false};
        std::atomic<std::uint32_t> lockupPc{
          0xFFFF'FFFEU};   // Armv6-M/v7-M; an Armv8-M core reads 0xEFFFFFFE         // DHCSR.S_SLEEP: the core waits in a wfi (the RP2040's park loop)
        std::atomic<bool> blockGone{false};   // no RTT id at the block address (a boot ROM runs)
        std::atomic<int>  slowWriteMs{0};     // a writeWord takes this long (a busy probe)
        // the DLL holds data the printer has not fetched yet: rttRead hands it out only after stopRtt
        std::atomic<bool>               holdUntilRttStop{false};
        std::function<void()>           onReset;
        std::vector<JLink::MemoryWrite> preResetAtLastReset;           // guarded by mutex
        std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;   // guarded by mutex
        // the RTT control block at 0x20000100 with one buffer, as words (id, counts, descriptor);
        // empty: the address pattern, which holds no "SEGGER RTT" id
        std::optional<std::array<std::uint32_t, 12>> controlBlock;   // guarded by mutex

        // target RAM for the printer's own RTT reader (DirectRtt): [RamBase, RamBase + ram.size()); empty: none.
        // Reads there take ramReadDelay (a probe's round trip).
        static constexpr std::uint32_t RamBase   = 0x2000'1000;
        static constexpr std::uint32_t FlashBase = 0x1000'0000;
        static constexpr std::uint32_t SsiWord
          = RamBase + 0x7f0;             // the fake SSI's CTRLR0, then SSIENR
        std::vector<std::byte>    ram;   // guarded by mutex
        std::chrono::microseconds ramReadDelay{0};

        bool inRam(std::uint32_t address,
                   std::size_t   n) const {
            return !ram.empty() && address >= RamBase && address + n <= RamBase + ram.size();
        }

        void putRam(std::uint32_t address,
                    std::uint32_t value) {
            for(std::size_t i = 0; i != 4; ++i) {
                ram[address - RamBase + i] = static_cast<std::byte>(value >> (8 * i));
            }
        }

        std::uint32_t getRam(std::uint32_t address) const {
            std::uint32_t v{};
            for(std::size_t i = 0; i != 4; ++i) {
                v |= std::to_integer<std::uint32_t>(ram[address - RamBase + i]) << (8 * i);
            }
            return v;
        }

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
        if(!shared->deadAcrossConnects) { shared->apDead = false; }
        if(shared->recoveryAtUs != 0 && shared->connectAfterRecoveryUs == 0) {
            shared->connectAfterRecoveryUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count();
        }
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
        shared->holdUntilRttStop = false;
    }

    std::uint8_t resetType{};

    void setResetType(std::uint8_t type) { resetType = type; }

    std::vector<JLink::MemoryWrite> preResetCommands;

    void setPreResetCommands(std::vector<JLink::MemoryWrite> commands) {
        preResetCommands = std::move(commands);
    }

    void resetTarget() {
        if(resetType == 2) {
            ++shared->resetLineResets;
            if(shared->resetLineFrees) { shared->apDead = false; }
        }
        if(shared->apDead) { throw std::runtime_error{"JLINK_Reset: -1"}; }
        shared->touch();
        shared->resetSinceGo = true;
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
        shared->resetSinceGo = false;   // a download resets and halts the core
        if(shared->wedged) { throw std::runtime_error{"fake flash failure: flash info"}; }
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

    void writeRegister(CoreRegister  r,
                       std::uint32_t value) {
        shared->touch();
        std::lock_guard<std::mutex> const lock{shared->mutex};
        shared->writtenRegisters[static_cast<std::size_t>(r)] = value;
    }

    std::uint32_t readRegister(CoreRegister r) {
        shared->touch();
        {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            if(auto const v = shared->writtenRegisters[static_cast<std::size_t>(r)]) { return *v; }
        }
        if(r == CoreRegister::sp) { return 0x20000100; }
        if(r == CoreRegister::pc && shared->lockup) { return shared->lockupPc.load(); }
        return 0x1000U + static_cast<std::uint32_t>(r);
    }

    void go() {
        shared->touch();
        ++shared->goCalls;
        if(shared->resetSinceGo.exchange(false) && !shared->wedgeOutlivesBoot) {
            shared->wedged = false;
        }
        shared->halted = false;
    }

    void halt() {
        shared->touch();
        shared->halted = true;
    }

    void clearAllBreakpoints() { shared->touch(); }

    bool isHalted() {
        if(shared->apDead) { throw std::runtime_error{"JLINK_IsHalted: -1 (4 tries)"}; }
        shared->touch();
        ++shared->haltPolls;
        return shared->halted;
    }

    void writeWord(std::uint32_t address,
                   std::uint32_t value) {
        if(shared->apDead) { throw std::runtime_error{"JLINK_WriteU32: -1"}; }
        if(address == 0xE000'ED0C && value == 0x05FA'0004) {
            ++shared->sysresets;
            if(shared->sysresetFrees) {
                std::lock_guard<std::mutex> const lock{shared->mutex};
                shared->putRam(shared->stuckWord, 0);
            }
        }
        if(address == 0x400d'8000 && value == 0x8000'0000) {
            shared->recoveryAtUs           = std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count();
            shared->connectAfterRecoveryUs = 0;
        }
        if(shared->deadOnRecoveryWrite && address == 0x400d'8000 && value == 0x8000'0000) {
            shared->apDead
              = true;   // the recovery's watchdog reset: this connection does not see the target again
            return;
        }
        shared->touch();
        if(auto const ms = shared->slowWriteMs.load(); ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{ms});
        }
        std::lock_guard<std::mutex> const lock{shared->mutex};
        if(shared->inRam(address, 4)) {
            shared->putRam(address, value);
            return;
        }
        shared->writes.emplace_back(address, value);
        if(address == 0x20000100 && value == 0) { shared->blockGone = true; }   // the id cleared
        if(shared->controlBlock && address >= 0x20000100 && address < 0x20000100 + 48) {
            (*shared->controlBlock)[(address - 0x20000100) / 4] = value;
        }
        if(address >= Shared::AnnounceAddress && address < Shared::AnnounceAddress + 16) {
            if(address == Shared::AnnounceAddress + 4 && value != 0) {
                shared->flashReadsAtArm = shared->flashReads.load();
            }
            shared->announce[(address - Shared::AnnounceAddress) / 4] = value;
            if(address == Shared::AnnounceAddress + 12) {   // the ack
                shared->ackedAt   = std::chrono::steady_clock::now();
                shared->resetting = true;
            }
        }
    }

    void writeMemory(std::uint32_t,
                     std::span<std::byte const>) {
        shared->touch();
    }

    void readMemory(std::uint32_t        address,
                    std::span<std::byte> out) {
        if(shared->apDead) { throw std::runtime_error{"JLINK_ReadMemHW: -1"}; }
        shared->touch();
        {
            std::unique_lock<std::mutex> lock{shared->mutex};
            if(shared->failReadTo != 0 && address < shared->failReadTo
               && address + out.size() > shared->failReadFrom)
            {
                shared->failReadFrom = shared->failReadTo = 0;
                throw std::runtime_error{"JLINK_ReadMemHW: 1"};
            }
            if(shared->inRam(address, out.size())) {
                auto const delay = shared->ramReadDelay;
                std::memcpy(out.data(), &shared->ram[address - Shared::RamBase], out.size());
                lock.unlock();
                std::this_thread::sleep_for(delay);
                return;
            }
        }
        if(address >= Shared::FlashBase && address < Shared::FlashBase + 0x10'0000) {
            std::lock_guard<std::mutex> const lock{shared->mutex};
            std::uint32_t                     ssi{};
            std::memcpy(&ssi, &shared->ram[Shared::SsiWord - Shared::RamBase], 4);
            bool const xip = (ssi & 0x300U) == 0x300U;
            if(!xip) { shared->romHung = true; }
            if(++shared->flashReads == shared->resetAfterFlashReads) {
                shared->coreWasReset = true;
            }
            std::ranges::fill(out, static_cast<std::byte>(xip ? 0x5aU : 0xeeU));
            return;
        }
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
            std::uint32_t const dhcsr = 0x0100'0000U
                                      | (shared->coreWasReset.exchange(false) ? 1U << 25 : 0U)
                                      | (shared->lockup && !shared->halted ? 1U << 19 : 0U)
                                      | (shared->asleep ? 1U << 18 : 0U);
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
        if(shared->holdUntilRttStop) { return buffer.first(0); }
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

    bool anyToolContains(std::string_view needle) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(tools, [&](auto const& t) {
            return t.find(needle) != std::string::npos;
        });
    }

    // for a failed check: what the reader said, in order of kind
    void dump() {
        std::lock_guard<std::mutex> const lock{mutex};
        for(auto const& t : tools) { std::printf("    tool:  %s\n", t.c_str()); }
        for(auto const& m : messages) { std::printf("    msg:   %s\n", m.c_str()); }
        for(auto const& e : errors) { std::printf("    error: %s\n", e.c_str()); }
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

    // where the map says the control block is (a rebuild changes it)
    std::atomic<std::uint32_t> blockAddress{0x20000100};

    // in an optional: the last test stops it
    std::optional<BasicJLinkRttReader<FakeTransport>> readerStorage;
    auto&                                             reader = readerStorage.emplace(
      "",
      "fake",
      4000,
      "",
      [&blockAddress]() { return RttBlockInfo{blockAddress.load(), 1}; },
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
      [](std::string_view) {});

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
        std::array<uc_log::detail::TargetOp, 2> const pieces{
          uc_log::detail::target_op::Read{0x20000010, 4},
          uc_log::detail::target_op::Read{0x200000FE, 3}
        };
        auto const result = reader.accessTarget(pieces, 2000ms);
        CHECK(result.has_value(), "memory read through the reader thread");
        if(result) {
            CHECK(result->size() == 2 && (*result)[0].size() == 4 && (*result)[1].size() == 3,
                  "one buffer per piece, of its size");
            CHECK((*result)[0][0] == std::byte{0x10} && (*result)[0][3] == std::byte{0x13}
                    && (*result)[1][2] == std::byte{0x00},
                  "the fake target's bytes: the low byte of each address");
        }

        std::array<uc_log::detail::TargetOp, 2> const writes{
          uc_log::detail::target_op::WriteWord{0x5000'000C, 0U},
          uc_log::detail::target_op::WriteWord{0x5000'0010, 7U}
        };
        auto const written = reader.accessTarget(writes, 2000ms);
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

        // A write the reader thread has started is not reported as timed out: it happens, so the caller hears
        // that it did (a slow probe once made a poke "time out" and still take effect).
        shared.slowWriteMs = 400;
        std::array<uc_log::detail::TargetOp, 1> const slow{
          uc_log::detail::target_op::WriteWord{0x5000'0014, 9U}
        };
        auto const late    = reader.accessTarget(slow, 100ms);
        shared.slowWriteMs = 0;
        bool written9      = false;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            written9
              = !shared.writes.empty()
             && shared.writes.back() == std::pair<std::uint32_t, std::uint32_t>{0x5000'0014U, 9U};
        }
        CHECK(written9, "the slow write happened");
        CHECK(late.has_value(), "and the caller is told it did, not that it timed out");
    }

    {
        // halt and resume through the probe's own calls; DHCSR writes refused (they hung the J-Link DLL)
        namespace to    = uc_log::detail::target_op;
        using Access    = uc_log::detail::TargetOp;
        auto const core = [&](Access c) {
            std::array<Access, 1> const op{c};
            return reader.accessTarget(op, 2000ms);
        };
        auto const running = core(to::ReadRegisters{});
        CHECK(!running && running.error().find("halt it first") != std::string::npos,
              "registers of a running core: refused");
        auto const halted = core(to::Halt{});
        auto const polls  = shared.haltPolls.load();
        CHECK(
          waitFor([&]() { return shared.haltPolls.load() >= polls + 3; }, 3000ms)
            && !collected.anyErrorContains("core halted") &&
            [&] {
                std::lock_guard<std::mutex> const lock{collected.mutex};
                return std::ranges::any_of(collected.tools, [](auto const& t) {
                    return t.find("halt request") != std::string::npos;
                });
            }(),
          "a requested halt is a tool message, not a 'core halted' error");
        auto const regs = core(to::ReadRegisters{});
        auto const word = [&](std::size_t i) {
            std::uint32_t v = 0;
            for(std::size_t b = 0; b != 4; ++b) {
                v |= std::to_integer<std::uint32_t>(regs->front()[i * 4 + b]) << (8 * b);
            }
            return v;
        };
        CHECK(regs && regs->front().size() == 19 * 4 && word(0) == 0x1000U
                && word(13) == 0x2000'0100U && word(15) == 0x1000U + 15U
                && word(18) == 0x1000U + 18U,
              "registers of a halted core: r0..r12, sp, lr, pc, xpsr, msp, psp from the probe");
        std::array<Access, 2> const writes{
          to::WriteRegister{ 0,      0x1234U},
          to::WriteRegister{15, 0x1000'2000U}
        };
        auto const wrote = reader.accessTarget(writes, 2000ms);
        auto const again = core(to::ReadRegisters{});
        CHECK(wrote && again && std::to_integer<std::uint32_t>(again->front()[0]) == 0x34U
                && std::to_integer<std::uint32_t>(again->front()[15 * 4 + 1]) == 0x20U,
              "registers written through the probe's own calls read back");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.writtenRegisters = {};   // the halts below expect the fake's own values
        }
        CHECK(halted && halted->size() == 1 && halted->front().front() == std::byte{1}
                && shared.halted,
              "halt: the core halted, said as 1");
        auto const                  ran = core(to::Go{});
        std::array<Access, 1> const runningWrite{
          to::WriteRegister{0, 1U}
        };
        auto const refusedWrite = reader.accessTarget(runningWrite, 2000ms);
        CHECK(!refusedWrite && refusedWrite.error().find("halt it first") != std::string::npos,
              "a register write to a running core: refused");
        CHECK(ran && ran->front().front() == std::byte{0} && !shared.halted,
              "go: the core runs, said as 0");
        auto const goes = shared.goCalls.load();
        CHECK(core(to::Go{}).has_value() && shared.goCalls.load() == goes,
              "go on a running core does not call the probe's go (it hung on one)");
        std::array<Access, 1> const dhcsr{
          to::WriteWord{0xE000'EDF0U, 0xA05F0003U}
        };
        auto const refused = reader.accessTarget(dhcsr, 2000ms);
        CHECK(!refused && refused.error().find("DHCSR") != std::string::npos && !shared.halted,
              "a DHCSR write is refused, and nothing is halted");
    }

    {
        // the halts of this block only
        auto const firstError = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return collected.errors.size();
        }();
        auto const haltEntries = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            std::vector<std::string>          found;
            for(std::size_t i = firstError; i < collected.errors.size(); ++i) {
                if(collected.errors[i].find("core halted") != std::string::npos) {
                    found.push_back(collected.errors[i]);
                }
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
            std::array<uc_log::detail::TargetOp, 1> const piece{
              uc_log::detail::target_op::Read{0x20000010, 4}
            };
            CHECK(reader.accessTarget(piece, 2000ms).has_value(),
                  "memory reads are served meanwhile");
        }
        shared.blockGone = false;
        CHECK(waitFor([&]() { return reader.sessionCount() > waiting; }, 3000ms),
              "the session starts once the block is back");

        // the files changed to another image (its map: another block address) and a reset: the wait is at that
        // address, where nothing is; the files back - the wait follows the map and the session starts
        blockAddress        = 0x20000300;
        shared.coreWasReset = true;
        CHECK(waitFor([&]() { return collected.anyMessageContains("control block at 0x20000300"); },
                      3000ms),
              "the wait is where the map says");
        auto const atOther = reader.sessionCount();
        std::this_thread::sleep_for(300ms);
        CHECK(reader.sessionCount() == atOther,
              "no session while the map points where no block is");
        blockAddress = 0x20000100;
        CHECK(
          waitFor([&]() { return reader.sessionCount() > atOther; }, 3000ms),
          "the map back at the block: the session starts (it waited for good at the old address)");
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
            std::array<uc_log::detail::TargetOp, 1> const piece{
              uc_log::detail::target_op::Read{0x20000010, 4}
            };
            auto const t0     = std::chrono::steady_clock::now();
            auto const result = reader.accessTarget(piece, 2000ms);
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

    // The printer stops while the DLL holds lines it read from the target: they are decoded, not dropped (the
    // target's read offset has moved past them, so nobody else would ever see them). Without this ~10 ms of
    // every channel went missing at each printer restart.
    {
        auto const before = [&]() {
            std::lock_guard<std::mutex> const lock{collected.mutex};
            return collected.entries.size();
        }();
        shared.holdUntilRttStop = true;
        shared.feed(makeFrame(0));
        std::this_thread::sleep_for(50ms);   // passes of the loop that see nothing
        readerStorage.reset();               // the printer stops
        std::lock_guard<std::mutex> const lock{collected.mutex};
        CHECK(collected.entries.size() == before + 1,
              "the line the DLL held at the stop is decoded");
    }

    // The printer's own RTT reader at the end of a session: one read per ring, up to the write offset - never a cut
    // inside a record (the stopping session would drop its head, the next one start on its tail: "corrupted data"
    // and an unannounced loss), and never a drain that goes on while the target floods (a stop that outlasted its
    // caller's wait). A target that floods a 1 KiB ring with whole 4-byte records; every probe read 1 ms.
    {
        constexpr std::uint32_t        RingSize = 1024;
        static constexpr std::uint32_t Block    = FakeTransport::Shared::RamBase;
        constexpr std::uint32_t        Desc     = Block + 24;
        constexpr std::uint32_t        Ring     = Block + 0x100;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ram.assign(0x800, std::byte{});
            std::memcpy(shared.ram.data(), "SEGGER RTT", 10);
            shared.putRam(Block + 16, 1);   // up buffers
            shared.putRam(Block + 20, 0);   // down buffers
            shared.putRam(Desc + 4, Ring);
            shared.putRam(Desc + 8, RingSize);
            shared.ramReadDelay = 1ms;
        }
        std::atomic<bool>          flooding{true};
        std::atomic<std::uint64_t> produced{0};
        std::thread                producer{[&]() {
            auto const frame = makeFrame(0);
            while(flooding) {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                auto const                        wr = shared.getRam(Desc + 12);
                auto const                        rd = shared.getRam(Desc + 16);
                auto const free = rd > wr ? rd - wr - 1 : RingSize - 1 - (wr - rd);
                if(free < frame.size()) { continue; }   // skip mode: whole or nothing
                auto pos = wr;
                for(auto const b : frame) {
                    shared.ram[Ring - FakeTransport::Shared::RamBase + pos] = b;
                    pos                                                     = (pos + 1) % RingSize;
                }
                shared.putRam(Desc + 12, pos);   // the write offset after the bytes
                ++produced;
            }
        }};
        Collected                  direct;
        auto const                 startReader = [&]() {
            return std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{Block, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [&direct](std::size_t, std::string_view msg, std::optional<remote_fmt::catalog_id>) {
                  std::lock_guard<std::mutex> const lock{direct.mutex};
                  direct.entries.emplace_back(msg);
              },
              [&direct](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{direct.mutex};
                  direct.messages.emplace_back(msg);
              },
              [&direct](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{direct.mutex};
                  direct.errors.emplace_back(msg);
              },
              [&direct](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{direct.mutex};
                  direct.tools.emplace_back(msg);
              },
              [](std::string_view) {});
        };
        auto const entries = [&]() {
            std::lock_guard<std::mutex> const lock{direct.mutex};
            return direct.entries.size();
        };
        for(int session = 0; session != 5; ++session) {
            auto       r     = startReader();
            auto const start = entries();
            CHECK(waitFor([&]() { return entries() > start + 500; }, 5000ms),
                  "the printer's own reader decodes");
            auto const t0 = std::chrono::steady_clock::now();
            r.reset();   // the printer stops while the target floods on
            auto const took = std::chrono::steady_clock::now() - t0;
            CHECK(took < 100ms, "a stop under a flood ends after one read per ring");
            if(took >= 100ms) {
                std::printf("  stop took %lld ms\n",
                            static_cast<long long>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
            }
        }
        flooding = false;
        producer.join();
        {
            auto r = startReader();   // the rest of the ring, then quiet
            std::this_thread::sleep_for(300ms);
            r.reset();
        }
        CHECK(!direct.anyErrorContains("corrupted") && !direct.anyErrorContains("resync"),
              "no session started inside a record");
        CHECK(!direct.anyMessageContains("unfinished"), "no session ended inside a record");
        CHECK(direct.anyToolContains("RTT read by the printer"),
              "the printer's own reader was used");
        std::lock_guard<std::mutex> const lock{direct.mutex};
        CHECK(direct.entries.size() == produced, "every record exactly once across the sessions");
        if(direct.entries.size() != produced) {
            std::printf("  decoded %zu, produced %llu\n",
                        direct.entries.size(),
                        static_cast<unsigned long long>(produced.load()));
        }
    }

    // A quiet target whose chip package can tell a stall from one word (the RP2350's QMI stuck BUSY): said as an
    // error, the package's recovery writes run, and a session ends for the new boot. A word that matches only now and
    // then is not a stall.
    {
        constexpr std::uint32_t Word = FakeTransport::Shared::RamBase + 0x700;
        Collected               quiet;
        // a second word that must hold as well (a signature of several words, the RP2040's stuck boot ROM)
        auto const start = [&](std::uint32_t wordValue, std::uint32_t secondValue = 0x0007'0000) {
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Word, wordValue);
                shared.putRam(Word + 4, secondValue);
                shared.ramReadDelay = 0ms;
                shared.writes.clear();
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [&quiet](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{quiet.mutex};
                  quiet.messages.emplace_back(msg);
              },
              [&quiet](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{quiet.mutex};
                  quiet.errors.emplace_back(msg);
              },
              [](std::string_view) {},
              [](std::string_view) {});
            r->setStallSignature(BasicJLinkRttReader<FakeTransport>::StallSignature{
              {                            {Word, 0x3, 0x3},{Word + 4, 0xffff'ffff, 0x0007'0000}                      },
              {JLink::MemoryWrite{0x4001'8008, 0x01ff'fffe},
               JLink::MemoryWrite{0x400d'8000, 0x8000'0000}}
            });
            return r;
        };
        {
            auto r = start(0x0301'0803);   // EN and BUSY: stuck
            CHECK(
              waitFor([&]() { return quiet.anyErrorContains("the target is stalled"); }, 6000ms),
              "a stall is said");
            CHECK(waitFor(
                    [&]() {
                        std::lock_guard<std::mutex> const lock{shared.mutex};
                        return std::ranges::count(
                                 shared.writes,
                                 std::pair<std::uint32_t, std::uint32_t>{0x400d'8000, 0x8000'0000})
                            >= 1;
                    },
                    3000ms),
                  "and the package's recovery runs");
            // a download that fails because the flash interface is stalled: recovered and flashed again, the
            // recovery not counted as one of the attempts (an RP2350 QMI left busy: "RAMCode did not respond")
            auto const flashesBefore = shared.flashes.load();
            shared.flashFailures
              = 3;   // as many as FlashAttempts: without the recovery path the flash fails
            auto const flashed = r->flashAndWait(20000ms);
            CHECK(flashed.has_value() && shared.flashes.load() == flashesBefore + 1
                    && quiet.anyErrorContains("the flash failed on a stalled target"),
                  "a flash on a stalled target: the recovery, then the download again");
            shared.flashFailures = 0;
        }
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {   // a download wedged where no register shows it (an RP2350: "Error while determining flash info", the QMI
            // idle) until the chip booted once: before the next attempt the package's recovery runs and the core is let
            // run after a reset - inside the attempts; a wedge that outlives that still ends there
            auto       r             = start(0x0000'0000);
            auto const flashesBefore = shared.flashes.load();
            shared.wedged            = true;
            auto const flashed       = r->flashAndWait(20000ms);
            CHECK(flashed.has_value() && shared.flashes.load() == flashesBefore + 1
                    && quiet.anyErrorContains(
                      "running the chip package's stall recovery before the next attempt"),
                  "a flash wedged until the chip boots: the recovery, a reset that runs, then the "
                  "download again");
            shared.wedgeOutlivesBoot = true;
            shared.wedged            = true;
            auto const failed        = r->flashAndWait(20000ms);
            CHECK(!failed.has_value() && shared.flashes.load() == flashesBefore + 1,
                  "a wedge nothing frees: reported after the attempts, not retried for ever");
            shared.wedgeOutlivesBoot = false;
            shared.wedged            = false;
        }
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {
            auto              r = start(0x0000'0000);
            std::atomic<bool> toggling{true};
            std::thread       toggler{[&]() {   // set now and then: a running image's busy flag
                for(std::uint32_t i = 0; toggling; ++i) {
                    {
                        std::lock_guard<std::mutex> const lock{shared.mutex};
                        shared.putRam(Word, (i % 4) == 0 ? 0x3U : 0x0U);
                    }
                    std::this_thread::sleep_for(3ms);
                }
            }};
            std::this_thread::sleep_for(5000ms);
            toggling = false;
            toggler.join();
            CHECK(!quiet.anyErrorContains("the target is stalled"),
                  "a word set only now and then is no stall");
        }
        {
            auto r
              = start(0x0301'0803,
                      0x005f'0300);   // the first word stuck, the second not as the signature says
            std::this_thread::sleep_for(4000ms);
            CHECK(!quiet.anyErrorContains("the target is stalled"),
                  "every condition must hold, not one of them");
        }

        // After the stall recovery the DLL's connection may not see the target again (every call -1: the RP2040's
        // watchdog reset under a connected DLL, for minutes): the printer connects again at once and finds it.
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
            quiet.messages.clear();
        }
        // The stall ladder goes on across reconnects: the package's recovery, then SYSRESETREQ in the live session - which frees this one - and the
        // connect after a recovery waits off the probe first.
        {
            {
                std::lock_guard<std::mutex> const lock{quiet.mutex};
                quiet.errors.clear();
                quiet.tools.clear();
            }
            shared.stuckWord     = Word;
            shared.sysresetFrees = true;
            shared.sysresets     = 0;
            shared.recoveryAtUs  = 0;
            auto r
              = start(0x0301'0803);   // EN and BUSY: stuck - the recovery writes do not free it
            CHECK(waitFor([&]() { return quiet.anyErrorContains("(2 of 3)"); }, 20000ms),
                  "ladder: the second step comes, across the reconnect");
            CHECK(waitFor([&]() { return shared.sysresets.load() >= 1; }, 3000ms)
                    && quiet.anyErrorContains("SYSRESETREQ"),
                  "ladder: step 2 is a SYSRESETREQ written in the live session");
            auto const pause = shared.connectAfterRecoveryUs.load() - shared.recoveryAtUs.load();
            CHECK(shared.recoveryAtUs.load() != 0 && pause >= 1'400'000,
                  "ladder: off the probe ~1.5 s after the recovery");
            if(pause < 1'400'000) { std::printf("  connect %lld us after the recovery\n", pause); }
            shared.sysresetFrees = false;
            shared.stuckWord     = 0;
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(Word, 0);
        }
        {   // the stall found while waiting for a new boot's block (a reset from outside, the block gone)
            auto r = start(0);
            CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 4000ms),
                  "dead after recovery: a session first");
            shared.deadOnRecoveryWrite = true;
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Word, 0x0301'0803);   // EN and BUSY: stuck
            }
            shared.blockGone    = true;
            shared.coreWasReset = true;
            CHECK(
              waitFor([&]() { return quiet.anyErrorContains("the target is stalled"); }, 8000ms),
              "dead after recovery: stall said");
            auto const constructionsAtStall = shared.constructions.load();
            // at once - not only after the silence bound (TargetSilentLimit, 3 s): the recovery itself reconnects
            CHECK(waitFor(
                    [&]() {
                        return shared.constructions.load() > constructionsAtStall
                            && !shared.apDead.load();
                    },
                    2500ms),
                  "dead after recovery: the probe connects again at once and sees the target");
            shared.deadOnRecoveryWrite = false;
            shared.blockGone           = false;
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Word, 0);
            }
        }
        // Silent across connections: connect anew, then the reset line, then a power-cycle request - never minutes of -1.
        for(bool const lineFrees : {true, false}) {
            {
                std::lock_guard<std::mutex> const lock{quiet.mutex};
                quiet.errors.clear();
            }
            shared.resetLineResets    = 0;
            shared.resetLineFrees     = lineFrees;
            shared.deadAcrossConnects = true;
            shared.apDead             = true;
            shared.blockGone          = true;   // a wait for the block, as after a reset
            auto r                    = start(0);
            CHECK(
              waitFor([&]() { return quiet.anyErrorContains("no answer from the target for 3 s"); },
                      8000ms),
              "silent: said within seconds");
            CHECK(waitFor([&]() { return shared.resetLineResets.load() >= 1; }, 20000ms),
                  "silent: the reset line after reconnects");
            if(lineFrees) {
                CHECK(waitFor([&]() { return !shared.apDead.load(); }, 2000ms),
                      "silent: the reset line freed it");
            } else {
                CHECK(waitFor([&]() { return quiet.anyErrorContains("power-cycle it"); }, 12000ms),
                      "silent: a power cycle asked for, and it keeps trying");
            }
            shared.deadAcrossConnects = false;
            shared.apDead             = false;
            shared.blockGone          = false;
        }

        // a core in lockup: DHCSR.S_LOCKUP, or halted at the lockup address (a debugger's connect halts a locked-up core
        // and that clears S_LOCKUP) - said and reset; a core halted anywhere else (someone halted it) is left alone
        auto const lockupSaid = [&]() { return quiet.anyErrorContains("in lockup"); };
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {
            shared.lockup = true;
            auto r        = start(0);
            CHECK(waitFor(lockupSaid, 6000ms), "DHCSR.S_LOCKUP: said");
        }
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {
            shared.lockup = true;
            shared.halted = true;
            auto r        = start(0);
            CHECK(waitFor(lockupSaid, 6000ms), "halted at the lockup address: said too");
        }
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {
            shared.lockup   = true;
            shared.halted   = true;
            shared.lockupPc = 0xEFFF'FFFEU;
            auto r          = start(0);
            CHECK(waitFor(lockupSaid, 6000ms), "halted at the Armv8-M lockup address: said too");
            shared.lockupPc = 0xFFFF'FFFEU;
        }
        {
            std::lock_guard<std::mutex> const lock{quiet.mutex};
            quiet.errors.clear();
        }
        {
            shared.lockup = false;
            shared.halted = true;
            auto r        = start(0);
            std::this_thread::sleep_for(4000ms);
            CHECK(!lockupSaid(), "halted at an ordinary PC: no lockup");
        }
        shared.halted = false;

        // After a lockup recovery the printer must go on answering control requests (2026-10: on a board, after such a
        // recovery the requests timed out for good while the log was still decoded). Both places a lockup is
        // recovered: a quiet session, and a closed decode gate (the firmware check failing - the flash unreadable).
        for(bool const viaGate : {false, true}) {
            {
                std::lock_guard<std::mutex> const lock{quiet.mutex};
                quiet.errors.clear();
            }
            std::atomic<bool> gateOpen{!viaGate};
            auto const        resetsBefore = shared.resets.load();
            shared.onReset = [&]() {   // the reset ends the lockup; the image then runs again
                shared.lockup = false;
                shared.halted = false;
                gateOpen      = true;
            };
            shared.lockup = true;
            shared.halted = true;
            auto r        = start(0);
            if(viaGate) {
                r->setDecodeGate([&gateOpen](auto const&) { return gateOpen.load(); });
            }
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 8000ms),
                  viaGate ? "lockup behind a closed gate: reset"
                          : "lockup in a quiet session: reset");
            int answered = 0;
            int failed   = 0;
            for(auto const until = std::chrono::steady_clock::now() + 4000ms;
                std::chrono::steady_clock::now() < until;)
            {
                std::array<uc_log::detail::TargetOp, 1> const piece{
                  uc_log::detail::target_op::Read{FakeTransport::Shared::RamBase, 4}
                };
                if(r->accessTarget(piece, 2000ms).has_value()) {
                    ++answered;
                } else {
                    ++failed;
                }
                std::this_thread::sleep_for(100ms);
            }
            CHECK(failed == 0 && answered > 10,
                  viaGate
                    ? "after a lockup recovery behind the gate, control requests are answered"
                    : "after a lockup recovery in a quiet session, control requests are answered");
            shared.onReset = {};
        }
        shared.lockup = false;
        shared.halted = false;
    }

    // a chip the J-Link parked in its boot ROM during its own reboot (the RP2040: CHIP_RESET.HAD_PSM_RESTART set,
    // the core asleep in the boot ROM's wfi loop, the RTT block gone). While the printer waits for the block it sees
    // the sign and resets the chip itself, said as an error; with the word alone (the core awake) it only says why
    // it does not. A reset brings the image back.
    for(bool const asleep : {true, false}) {
        constexpr std::uint32_t Word    = FakeTransport::Shared::RamBase + 0x740;
        constexpr std::uint32_t Rescued = 1U << 20;
        Collected               parked;
        auto const              blockId = [&](bool there) {   // the RTT block's id at RamBase
            std::lock_guard<std::mutex> const lock{shared.mutex};
            std::memcpy(&shared.ram[0], there ? "SEGGER RTT" : "\0\0\0\0\0\0\0\0\0\0", 10);
        };
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(Word, 0);
            shared.ramReadDelay = 0ms;
        }
        blockId(true);
        auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
          [&parked](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{parked.mutex};
              parked.messages.emplace_back(msg);
          },
          [&parked](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{parked.mutex};
              parked.errors.emplace_back(msg);
          },
          [&parked](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{parked.mutex};
              parked.tools.emplace_back(msg);
          },
          [](std::string_view) {});
        r->setParkSignature(
          BasicJLinkRttReader<FakeTransport>::ParkSignature{Word, Rescued, Rescued, 0x30});
        CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 3000ms),
              "park: a session runs first");
        auto const resetsBefore = shared.resets.load();
        shared.onReset          = [&]() {   // the printer's reset: the image boots again
            shared.asleep = false;
            blockId(true);
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(Word, 0);
        };
        {   // the reboot and the DLL's rescue: the block gone, the word set, the core (maybe) asleep
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(Word, Rescued);
        }
        shared.asleep = asleep;
        blockId(false);
        shared.coreWasReset = true;
        if(asleep) {
            CHECK(waitFor(
                    [&]() {
                        return parked.anyErrorContains("J-Link parked the target in its boot ROM");
                    },
                    4000ms),
                  "park: said as an error");
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 3000ms),
                  "park: the printer resets it");
            CHECK(parked.anyErrorContains("the core asleep as in the park loop at 0x30"),
                  "park: the sign named");
        } else {
            CHECK(waitFor([&]() { return parked.anyToolContains("but the core is not asleep"); },
                          4000ms),
                  "park: the word alone is said, not acted on");
            std::this_thread::sleep_for(1500ms);
            CHECK(shared.resets.load() == resetsBefore && !parked.anyErrorContains("J-Link parked"),
                  "park: no reset while the core is awake");
        }
        r.reset();
        shared.onReset = {};
        shared.asleep  = false;
        blockId(true);
    }

    // The firmware check reads the flash only while it is mapped (--flash_ready_signature): an unannounced reset lands
    // the boot ROM in its plain-SPI loop, and an XIP read then hangs it (the boot ROM stall). The session start's read
    // waits for the chip's words (here: SSI CTRLR0.TMOD EEPROM_READ), then reads; never mapped, it fails without
    // touching the flash.
    {
        Collected  flashCheck;
        auto const put = [&](std::uint32_t address, std::uint32_t v) {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(address, v);
        };
        auto const ssi = [&](std::uint32_t v) { put(FakeTransport::Shared::SsiWord, v); };
        {   // a control block with one up ring whose name lies in flash (an image's .rodata)
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ram.assign(0x800, std::byte{});
            std::memcpy(shared.ram.data(), "SEGGER RTT", 10);
            shared.putRam(FakeTransport::Shared::RamBase + 16, 1);
            shared.putRam(FakeTransport::Shared::RamBase + 20, 0);
            shared.putRam(FakeTransport::Shared::RamBase + 24,
                          FakeTransport::Shared::FlashBase + 0x200);   // its name
            shared.putRam(FakeTransport::Shared::RamBase + 28,
                          FakeTransport::Shared::RamBase + 0x100);
            shared.putRam(FakeTransport::Shared::RamBase + 32, 256);
            shared.blockGone    = false;
            shared.ramReadDelay = 0ms;
        }
        auto const start = [&](std::vector<std::byte>& got,
                               std::atomic<int>&       results,
                               std::atomic<bool>&      readOk) {
            {   // no connection until the callbacks are set: the reader's thread runs from its constructor
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [&flashCheck](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{flashCheck.mutex};
                  flashCheck.messages.emplace_back(msg);
              },
              [&flashCheck](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{flashCheck.mutex};
                  flashCheck.errors.emplace_back(msg);
              },
              [&flashCheck](std::string_view msg) {
                  std::lock_guard<std::mutex> const lock{flashCheck.mutex};
                  flashCheck.tools.emplace_back(msg);
              },
              [](std::string_view) {});
            r->setFlashReadySignature({
              {    FakeTransport::Shared::SsiWord, 0x300, 0x300},
              {FakeTransport::Shared::SsiWord + 4,   0x1,   0x1}
            });
            r->setOnSessionStart([&](auto const& read) {
                std::vector<std::byte> b(16);
                readOk = read(FakeTransport::Shared::FlashBase + 0x100, b);
                got    = b;
                ++results;
            });
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = true;
            }
            return r;
        };
        {
            put(FakeTransport::Shared::SsiWord + 4, 1);   // SSIENR: enabled
            ssi(0x0007'0000);   // the ROM's plain SPI transfer: SPI_FRF 0, DFS_32 7, TMOD 0
            shared.romHung    = false;
            shared.flashReads = 0;
            std::vector<std::byte> got;
            std::atomic<int>       results{0};
            std::atomic<bool>      readOk{false};
            auto                   r = start(got, results, readOk);
            std::this_thread::sleep_for(600ms);
            CHECK(shared.flashReads.load() == 0 && results.load() == 0,
                  "flash ready: no read while the boot ROM drives the flash, the check waits");
            ssi(0x005f'0300);   // boot2's XIP: SPI_FRF QUAD, DFS_32 31, TMOD EEPROM_READ
            bool const checked = waitFor([&]() { return results.load() >= 1; }, 8000ms);
            CHECK(checked, "flash ready: the check reads once mapped");
            if(!checked) { flashCheck.dump(); }
            CHECK(readOk.load() && !got.empty() && got[0] == std::byte{0x5a}
                    && !shared.romHung.load(),
                  "flash ready: the read sees the flash, the boot ROM never saw an XIP access");
        }
        {
            put(FakeTransport::Shared::SsiWord + 4,
                0);   // SSIENR 0: not mapped, though TMOD says so
            ssi(0x005f'0300);
            shared.romHung    = false;
            shared.flashReads = 0;
            std::vector<std::byte> got;
            std::atomic<int>       results{0};
            std::atomic<bool>      readOk{true};
            auto                   r = start(got, results, readOk);
            CHECK(waitFor([&]() { return results.load() >= 1; }, 6000ms),
                  "flash ready: a check that waited ends");
            CHECK(!readOk.load() && flashCheck.anyToolContains("the flash is not mapped"),
                  "flash ready: never mapped - said");
            std::this_thread::sleep_for(
              4000ms);   // the session reads its rings' names now (in flash)
            CHECK(shared.flashReads.load() == 0 && !shared.romHung.load(),
                  "flash ready: never mapped - no flash read, the ring names' neither");
        }
        {   // a core-0-only reset in the middle of the check: the words still say "mapped" (the SSI keeps boot2's setup),
            // S_RESET_ST says it - the read it fell into is dropped, no XIP read after it
            put(FakeTransport::Shared::SsiWord + 4, 1);
            ssi(0x005f'0300);
            shared.flashReads           = 0;
            shared.resetAfterFlashReads = 1;
            std::atomic<int> okReads{0};
            std::atomic<int> done{0};
            std::atomic<int> readsAtEnd{-1};
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {});
            r->setFlashReadySignature({
              {FakeTransport::Shared::SsiWord, 0x300, 0x300}
            });
            r->setAnnouncedReset([]() -> std::optional<std::uint32_t> {
                return FakeTransport::Shared::AnnounceAddress;
            });
            r->setOnSessionStart([&](auto const& read) {
                if(done.load() != 0) { return; }
                for(int i = 0; i != 4; ++i) {
                    std::vector<std::byte> b(16);
                    if(read(FakeTransport::Shared::FlashBase
                              + 0x1000U * static_cast<std::uint32_t>(i),
                            b))
                    {
                        ++okReads;
                    }
                }
                readsAtEnd = shared.flashReads.load();
                ++done;
            });
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.announce  = {uc_log::detail::announced_reset::Magic, 0, 0, 0};
                shared.connected = true;
            }
            CHECK(waitFor([&]() { return done.load() >= 1; }, 6000ms),
                  "reset in the check: the check ends");
            CHECK(okReads.load() == 0 && readsAtEnd.load() == 1,
                  "reset in the check: the read it hit dropped, no XIP read after it");
            shared.resetAfterFlashReads = -1;
            r.reset();
        }
        {   // a reset before the check's first read: that read is not made at all
            shared.flashReads = 0;
            std::atomic<int> readsAtEnd{-1};
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {});
            r->setFlashReadySignature({
              {FakeTransport::Shared::SsiWord, 0x300, 0x300}
            });
            r->setOnSessionStart([&](auto const& read) {
                if(readsAtEnd.load() >= 0) { return; }
                shared.coreWasReset = true;   // the reset lands right before the check reads
                std::vector<std::byte> b(16);
                static_cast<void>(read(FakeTransport::Shared::FlashBase, b));
                readsAtEnd = shared.flashReads.load();
            });
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = true;
            }
            CHECK(waitFor([&]() { return readsAtEnd.load() >= 0; }, 6000ms)
                    && readsAtEnd.load() == 0,
                  "reset before the check: no XIP read at all");
            r.reset();
        }
        {   // the announce block is armed only after the session start's flash reads (an announced reset waits for them)
            shared.flashReads      = 0;
            shared.flashReadsAtArm = -1;
            std::atomic<int> done{0};
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
                shared.announce  = {uc_log::detail::announced_reset::Magic, 0, 0, 0};
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {});
            r->setFlashReadySignature({
              {FakeTransport::Shared::SsiWord, 0x300, 0x300}
            });
            r->setAnnouncedReset([]() -> std::optional<std::uint32_t> {
                return FakeTransport::Shared::AnnounceAddress;
            });
            r->setOnSessionStart([&](auto const& read) {
                std::vector<std::byte> b(16);
                static_cast<void>(read(FakeTransport::Shared::FlashBase, b));
                static_cast<void>(read(FakeTransport::Shared::FlashBase + 0x1000, b));
                ++done;
            });
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = true;
            }
            CHECK(waitFor([&]() { return shared.flashReadsAtArm.load() >= 0; }, 6000ms),
                  "armed: the block is armed");
            CHECK(shared.flashReadsAtArm.load() >= 2,
                  "armed: only after the session start's two flash reads");
            r.reset();
        }
        {   // a chip parked in its boot ROM is reset at once by the session start's flash read, not polled for the
            // gate's whole wait first: the health checks run once before any polling
            constexpr std::uint32_t Word    = FakeTransport::Shared::RamBase + 0x740;
            constexpr std::uint32_t Rescued = 1U << 20;
            ssi(0x0007'0000);   // the boot ROM drives the flash
            put(Word, Rescued);
            shared.asleep                       = true;
            shared.flashReads                   = 0;
            shared.romHung                      = false;
            auto const             resetsBefore = shared.resets.load();
            std::atomic<long long> resetAfterMs{-1};
            auto const             connectedAt
              = std::make_shared<std::atomic<std::chrono::steady_clock::time_point>>();
            shared.onReset = [&]() {   // the reset: the image boots and maps its flash
                if(resetAfterMs.load() < 0) {
                    resetAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - connectedAt->load())
                                     .count();
                }
                shared.asleep = false;
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Word, 0);
                shared.putRam(FakeTransport::Shared::SsiWord, 0x005f'0300);
            };
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {});
            r->setFlashReadySignature({
              {FakeTransport::Shared::SsiWord, 0x300, 0x300}
            });
            r->setParkSignature(
              BasicJLinkRttReader<FakeTransport>::ParkSignature{Word, Rescued, Rescued, 0x30});
            connectedAt->store(std::chrono::steady_clock::now());
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = true;
            }
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 8000ms),
                  "park in the gate: reset");
            CHECK(
              resetAfterMs.load() >= 0 && resetAfterMs.load() < 2000 && !shared.romHung.load(),
              "park in the gate: reset before the gate's wait ran out, no flash read while parked");
            r.reset();
            shared.onReset = {};
            shared.asleep  = false;
            put(Word, 0);
        }
        {   // a control-socket read in the flash window goes through the same gate
            ssi(0x0007'0000);   // the boot ROM drives the flash
            shared.flashReads = 0;
            shared.romHung    = false;
            {   // no session before the gate is set: the reader thread starts in the constructor (a session begun
                // before setFlashReadySignature reads the flash ungated, and the counts below are off)
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = false;
            }
            auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
              "",
              "fake",
              4000,
              "",
              []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
              []() { return std::string{"fake.hex"}; },
              []() {
                  return std::unordered_map<std::uint16_t, std::string>{
                    {0, "hello direct"}
                  };
              },
              [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {},
              [](std::string_view) {});
            r->setFlashReadySignature({
              {FakeTransport::Shared::SsiWord, 0x300, 0x300}
            });
            r->setFlashWindow(
              std::pair<std::uint32_t, std::uint32_t>{FakeTransport::Shared::FlashBase, 0x10'0000});
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.connected = true;
            }
            std::array<uc_log::detail::TargetOp, 1> const flashOp{
              uc_log::detail::target_op::Read{FakeTransport::Shared::FlashBase + 0x40, 16}
            };
            auto const refused = r->accessTarget(flashOp, 8000ms);
            CHECK(!refused.has_value() && shared.flashReads.load() == 0 && !shared.romHung.load(),
                  "window: not read while the boot ROM drives the flash");
            std::array<uc_log::detail::TargetOp, 1> const ramOp{
              uc_log::detail::target_op::Read{FakeTransport::Shared::RamBase, 16}
            };
            CHECK(r->accessTarget(ramOp, 4000ms).has_value(), "window: RAM is read as before");
            ssi(0x005f'0300);
            CHECK(r->accessTarget(flashOp, 8000ms).has_value() && shared.flashReads.load() == 1,
                  "window: read once mapped");
            r.reset();
        }
        put(FakeTransport::Shared::SsiWord, 0);
        put(FakeTransport::Shared::SsiWord + 4, 0);
    }

    // A core halted in a fault handler while the printer waits for the block (the RP2350 boot ROM's HardFault after a
    // watchdog reboot: the J-Link DLL's connect halts a core in lockup and leaves it in Debug state at the handler):
    // nobody asked for the halt, nothing can be read - said and reset. A halt a client asked for is left alone.
    for(bool const requested : {false, true}) {
        Collected  faulted;
        auto const blockId = [&](bool there) {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            std::memcpy(&shared.ram[0], there ? "SEGGER RTT" : "\0\0\0\0\0\0\0\0\0\0", 10);
        };
        auto const faultRegisters = [&](bool on) {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            using R = FakeTransport::CoreRegister;
            shared.writtenRegisters[static_cast<std::size_t>(R::xpsr)]
              = on ? std::optional<std::uint32_t>{0x0100'0003U} : std::nullopt;
            shared.writtenRegisters[static_cast<std::size_t>(R::pc)]
              = on ? std::optional<std::uint32_t>{0x2f8U} : std::nullopt;
        };
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
        }
        blockId(true);
        auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
          [&faulted](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{faulted.mutex};
              faulted.messages.emplace_back(msg);
          },
          [&faulted](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{faulted.mutex};
              faulted.errors.emplace_back(msg);
          },
          [&faulted](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{faulted.mutex};
              faulted.tools.emplace_back(msg);
          },
          [](std::string_view) {});
        CHECK(waitFor([&]() { return r->sessionCount() >= 1; }, 3000ms),
              "fault halt: a session runs first");
        if(requested) {
            std::array<uc_log::detail::TargetOp, 1> const halt{uc_log::detail::target_op::Halt{}};
            CHECK(r->accessTarget(halt, 2000ms).has_value() && shared.halted,
                  "fault halt: a client halts the core");
        }
        auto const resetsBefore = shared.resets.load();
        shared.onReset          = [&]() {   // the printer's reset: the image boots again
            shared.halted = false;
            faultRegisters(false);
            blockId(true);
        };
        faultRegisters(
          true);   // the reboot: the block gone, the core halted at the boot ROM's HardFault
        shared.halted = true;
        blockId(false);
        shared.coreWasReset = true;
        if(!requested) {
            bool const said = waitFor(
              [&]() { return faulted.anyErrorContains("halted in a fault handler (exception 3"); },
              5000ms);
            CHECK(said, "fault halt: said as an error");
            if(!said) { faulted.dump(); }
            CHECK(waitFor([&]() { return shared.resets.load() > resetsBefore; }, 3000ms),
                  "fault halt: the printer resets it");
            CHECK(waitFor([&]() { return r->sessionCount() >= 2; }, 5000ms),
                  "fault halt: the log comes back");
        } else {
            std::this_thread::sleep_for(3000ms);
            CHECK(shared.resets.load() == resetsBefore
                    && !faulted.anyErrorContains("halted in a fault handler"),
                  "fault halt: a halt a client asked for is left alone");
        }
        r.reset();
        shared.onReset = {};
        shared.halted  = false;
        faultRegisters(false);
        blockId(true);
    }

    // a stop while the decoder lags behind the reader (a slow consumer of the decoded lines, a flood) - every
    // record already read is still decoded, and the reader still ends promptly. Before, the decoder honoured the stop:
    // the rest piled up undecoded and the oversized-buffer resync ground through it for minutes.
    {
        constexpr std::uint32_t RingSize = 1024;
        constexpr std::uint32_t Desc     = FakeTransport::Shared::RamBase + 24;
        constexpr std::uint32_t Ring     = FakeTransport::Shared::RamBase + 0x100;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ramReadDelay = 0ms;
            shared.putRam(Desc + 12, 0);
            shared.putRam(Desc + 16, 0);
        }
        std::atomic<bool>          flooding{true};
        std::atomic<std::uint64_t> produced{0};
        std::thread                producer{[&]() {
            auto const frame = makeFrame(0);
            while(flooding) {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                auto const                        wr = shared.getRam(Desc + 12);
                auto const                        rd = shared.getRam(Desc + 16);
                auto const free = rd > wr ? rd - wr - 1 : RingSize - 1 - (wr - rd);
                if(free < frame.size()) { continue; }
                auto pos = wr;
                for(auto const b : frame) {
                    shared.ram[Ring - FakeTransport::Shared::RamBase + pos] = b;
                    pos                                                     = (pos + 1) % RingSize;
                }
                shared.putRam(Desc + 12, pos);
                ++produced;
            }
        }};
        std::atomic<std::uint64_t> decoded{0};
        auto                       r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{FakeTransport::Shared::RamBase, 1}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [&decoded](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {
              std::this_thread::sleep_for(20us);   // a slow consumer: the decoder lags behind
              ++decoded;
          },
          [](std::string_view) {},
          [](std::string_view) {},
          [](std::string_view) {},
          [](std::string_view) {});
        std::this_thread::sleep_for(500ms);
        flooding = false;
        producer.join();
        auto const t0 = std::chrono::steady_clock::now();
        r.reset();   // the stop
        auto const took = std::chrono::steady_clock::now() - t0;
        CHECK(took < 3000ms, "the reader ends promptly with a backlog");
        if(took >= 3000ms) {
            std::printf("  the stop took %lld ms\n",
                        static_cast<long long>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
        }
        // what the stopped session read was decoded whole; the rest is still in the ring (read by the next session)
        std::uint64_t left{};
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            auto const                        wr = shared.getRam(Desc + 12);
            auto const                        rd = shared.getRam(Desc + 16);
            left                                 = (wr >= rd ? wr - rd : RingSize - rd + wr) / 4;
        }
        CHECK(decoded + left == produced,
              "every record read was decoded, every other one is still in the ring");
        if(decoded + left != produced) {
            std::printf("  decoded %llu + left %llu != produced %llu\n",
                        static_cast<unsigned long long>(decoded.load()),
                        static_cast<unsigned long long>(left),
                        static_cast<unsigned long long>(produced.load()));
        }
    }

    // the probe is lost in the middle of a pass (a J-Link that drops off USB): readAll() has read ring 0 and moved
    // its read offset in the target, then the read of ring 1 throws. What the dying session read must still be
    // decoded, and nothing may be left damaged for the next session - before, ring 0's bytes died with the exception
    // (an unannounced loss of up to a ring) and the next session began inside a record ("corrupted data").
    {
        // 1030 bytes: the per-pass cap (half a ring while both rings hold data) is 515 - reads end inside 4-byte records,
        // so the dying session also holds the head of a cut record
        constexpr std::uint32_t RingSize = 1030;
        constexpr std::uint32_t Block    = FakeTransport::Shared::RamBase;
        constexpr std::uint32_t Desc0    = Block + 24;
        constexpr std::uint32_t Desc1    = Desc0 + 24;
        constexpr std::uint32_t Ring0    = Block + 0x100;
        constexpr std::uint32_t Ring1    = Ring0 + RingSize;
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ram.assign(0x100 + 2 * RingSize, std::byte{});
            std::memcpy(shared.ram.data(), "SEGGER RTT", 10);
            shared.putRam(Block + 16, 2);   // up buffers
            shared.putRam(Block + 20, 0);   // down buffers
            shared.putRam(Desc0 + 4, Ring0);
            shared.putRam(Desc0 + 8, RingSize);
            shared.putRam(Desc1 + 4, Ring1);
            shared.putRam(Desc1 + 8, RingSize);
            shared.ramReadDelay = 0ms;
            shared.putRam(
              Block + 0xF0,
              8);   // the image's RTT epoch: constant (even: the block is ready), no boot here
            shared.connected = false;   // no session before the epoch source is set (below)
        }
        std::atomic<bool>          flooding{true};
        std::atomic<std::uint64_t> produced{0};
        std::thread                producer{[&]() {
            auto const frame = makeFrame(0);
            auto const put   = [&](std::uint32_t desc, std::uint32_t ring) {
                auto const wr   = shared.getRam(desc + 12);
                auto const rd   = shared.getRam(desc + 16);
                auto const free = rd > wr ? rd - wr - 1 : RingSize - 1 - (wr - rd);
                if(free < frame.size()) { return; }
                auto pos = wr;
                for(auto const b : frame) {
                    shared.ram[ring - FakeTransport::Shared::RamBase + pos] = b;
                    pos                                                     = (pos + 1) % RingSize;
                }
                shared.putRam(desc + 12, pos);
                ++produced;
            };
            while(flooding) {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                put(Desc0, Ring0);
                put(Desc1, Ring1);
            }
        }};
        std::atomic<std::uint64_t> decoded{0};
        Collected                  lost;
        auto                       r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{Block, 2}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [&decoded](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {
              ++decoded;
          },
          [&lost](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{lost.mutex};
              lost.messages.emplace_back(msg);
          },
          [&lost](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{lost.mutex};
              lost.errors.emplace_back(msg);
          },
          [](std::string_view) {},
          [](std::string_view) {});
        r->setRttEpochSource([]() { return std::optional<std::uint32_t>{Block + 0xF0}; });
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        std::this_thread::sleep_for(
          1500ms);   // the first session up (a connect is retried every second)
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.failReadFrom = Ring1;   // the probe goes while ring 1 is read, after ring 0 was
            shared.failReadTo   = Ring1 + RingSize;
        }
        std::this_thread::sleep_for(800ms);   // a new session
        flooding = false;
        producer.join();
        waitFor(
          [&]() {
              std::lock_guard<std::mutex> const lock{shared.mutex};
              return shared.getRam(Desc0 + 12) == shared.getRam(Desc0 + 16)
                  && shared.getRam(Desc1 + 12) == shared.getRam(Desc1 + 16);
          },
          3000ms);
        std::this_thread::sleep_for(200ms);
        r.reset();
        bool failed{};
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            failed = shared.failReadTo == 0;
        }
        CHECK(failed, "the probe failure happened");
        CHECK(!lost.anyErrorContains("corrupted"), "no damaged frame after the probe came back");
        CHECK(decoded == produced,
              "every record of both rings is decoded - none died with the session");
        if(decoded != produced) {
            std::printf("  decoded %llu != produced %llu\n",
                        static_cast<unsigned long long>(decoded.load()),
                        static_cast<unsigned long long>(produced.load()));
        }
    }

    {
        // boots nobody read, counted exactly by the image's count of boots that logged (detail/BootLedger.hpp; the RTT
        // epoch moves at every start, also one the J-Link DLL parks before main - it may not be counted)
        constexpr std::uint32_t        Block    = FakeTransport::Shared::RamBase;
        constexpr std::uint32_t        RingSize = 256;
        constexpr std::uint32_t        Desc0    = Block + 24;
        constexpr std::uint32_t        Ring0    = Block + 0x100;
        static constexpr std::uint32_t Epoch    = Block + 0xF0;
        static constexpr std::uint32_t Logged   = Block + 0xF4;
        // a start of the target: its epoch, its count of boots that logged, and its ring (two records if it logged, or
        // `bytes` of them: 2 is half a record)
        auto const start
          = [&](std::uint32_t epoch, std::uint32_t logged, bool logs, std::uint32_t bytes = 8) {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Epoch, epoch);
                shared.putRam(Logged,
                              logged << 16U
                                | (epoch & 0xFFFFU));   // the count in 31:16 (detail/RttEpoch.hpp)
                shared.putRam(Logged + 4,
                              ~(logged << 16U | (epoch & 0xFFFFU)));   // and its complement
                shared.putRam(Desc0 + 12, logs ? bytes : 0U);
                shared.putRam(Desc0 + 16, 0);
                shared.coreWasReset = true;
            };
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ram.assign(0x100 + RingSize, std::byte{});
            std::memcpy(shared.ram.data(), "SEGGER RTT", 10);
            shared.putRam(Block + 16, 1);
            shared.putRam(Block + 20, 0);
            shared.putRam(Desc0 + 4, Ring0);
            shared.putRam(Desc0 + 8, RingSize);
            shared.ramReadDelay = 0ms;
            shared.putRam(Epoch, 8);
            shared.putRam(Logged, 7U << 16U | 8U);
            shared.putRam(Logged + 4, ~(7U << 16U | 8U));
            auto const frame
              = makeFrame(0);   // what a boot that logged left: two records ("hello direct")
            for(std::size_t i = 0; i != 8; ++i) { shared.ram[0x100 + i] = frame[i % frame.size()]; }
            shared.putRam(Desc0 + 12, 8);   // boot 7 logged 8 bytes: read by the first session
            shared.connected = false;
        }
        Collected                                 said;
        std::mutex                                unreadMutex;
        std::vector<std::array<std::uint32_t, 3>> unread;
        auto r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{Block, 1}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [](std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>) {},
          [&said](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{said.mutex};
              said.messages.emplace_back(msg);
          },
          [&said](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{said.mutex};
              said.errors.emplace_back(msg);
          },
          [](std::string_view) {},
          [](std::string_view) {});
        r->setRttEpochSource([]() { return std::optional<std::uint32_t>{Epoch}; });
        r->setLoggedBootsSource([]() { return std::optional<std::uint32_t>{Logged}; });
        r->setUnreadBootsCallback([&](std::uint32_t n, std::uint32_t from, std::uint32_t to) {
            std::lock_guard<std::mutex> const lock{unreadMutex};
            unread.push_back({n, from, to});
        });
        auto const unreadCount = [&] {
            std::lock_guard<std::mutex> const lock{unreadMutex};
            return unread.size();
        };
        auto const read = [&] {   // the session has read its boot's bytes
            return waitFor(
              [&]() {
                  std::lock_guard<std::mutex> const lock{shared.mutex};
                  return shared.getRam(Desc0 + 16) == shared.getRam(Desc0 + 12);
              },
              4000ms);
        };
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(read(), "boots: boot 7 read");

        // (b) boot 8 logged and nobody read it, boot 9 logged and is read: exactly 1
        start(10, 9, true);
        CHECK(read() && waitFor([&]() { return unreadCount() == 1; }, 4000ms),
              "boots: one that logged unread is noticed");
        {
            std::lock_guard<std::mutex> const  lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 7, 8};
            CHECK(unread.size() == 1 && unread.back() == want, "boots: exactly 1 (counts 7 -> 8)");
        }
        CHECK(said.anyErrorContains("1 boot passed unread (boots that logged 7 -> 8)"),
              "boots: and said");

        // (a) a start parked before main (the epoch moves, nothing logged), then the printer's reset: nothing unread
        start(12, 9, false);
        std::this_thread::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(), "boots: the printer's reset");
        std::this_thread::sleep_for(500ms);
        CHECK(unreadCount() == 1, "boots: a start that never logged is no unread boot");

        // (c) two starts, one parked before main and one that logged, then the printer's flash: exactly 1
        start(
          14,
          10,
          false);   // boot 12 logged (its count moved) - its line was not kept (a full ring), nothing read
        std::this_thread::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(), "boots: the printer's reset again");
        CHECK(waitFor([&]() { return unreadCount() == 2; }, 4000ms),
              "boots: the one that logged is counted");
        {
            std::lock_guard<std::mutex> const  lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 9, 10};
            CHECK(unread.size() == 2 && unread.back() == want,
                  "boots: exactly 1 of the two starts (counts 9 -> 10)");
        }

        // (d) boot 11 logged, a session read its first bytes - half a record - and ended on a probe failure: no line of it,
        // so not read after all; the printer's reset then counts it
        start(16, 11, true, 2);
        CHECK(read(), "boots: half a record of boot 11 read");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.failReadFrom
              = Desc0;   // the next read of the ring's offsets: the J-Link drops off USB
            shared.failReadTo = Desc0 + 24;
        }
        std::this_thread::sleep_for(1500ms);
        CHECK(r->resetAndWait(5000ms).has_value(),
              "boots: the printer's reset after the probe failure");
        CHECK(waitFor([&]() { return unreadCount() == 3; }, 4000ms),
              "boots: the boot whose bytes gave no line is counted");
        {
            std::lock_guard<std::mutex> const  lock{unreadMutex};
            std::array<std::uint32_t, 3> const want{1, 10, 11};
            CHECK(unread.size() == 3 && unread.back() == want,
                  "boots: exactly that one (counts 10 -> 11)");
        }

        // (e) a reset from outside while the boot ROM runs: RAM still holds the old boot's block and epoch - no session (no
        // firmware check reading a flash the ROM drives) until the new boot moved the epoch
        {
            auto const before = r->sessionCount();
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.coreWasReset = true;
            }
            std::this_thread::sleep_for(1500ms);
            CHECK(r->sessionCount() == before,
                  "boots: the old boot's block in RAM starts no session");
            start(18, 12, true);
            CHECK(waitFor([&]() { return r->sessionCount() > before; }, 4000ms),
                  "boots: the new epoch's does");
        }

        // a count that went back (a power-on, a download over RAM): said, no number
        start(20, 3, true);
        CHECK(waitFor([&]() { return said.anyErrorContains("how many is unknown"); }, 4000ms),
              "boots: a count that went back is said as not countable");
        CHECK(unreadCount() == 3, "boots: with no number handed over");

        // (g) the count not kept (its complement does not match: a watchdog reboot that reset the SRAM flipped its bits)
        // - no number claimed, said, the restart row; the next kept count is a new baseline
        {
            auto const restartsBefore = [&] {
                std::lock_guard<std::mutex> const lock{said.mutex};
                return std::ranges::count_if(said.messages, [](auto const& m) {
                    return m.find("was not kept") != std::string::npos;
                });
            }();
            {
                std::lock_guard<std::mutex> const lock{shared.mutex};
                shared.putRam(Epoch, 22);
                shared.putRam(Logged,
                              1U << 16U | 22U);   // the boot found junk and started the count again
                shared.putRam(Logged + 4,
                              ~(1U << 16U | 22U) ^ 1U);   // its complement, marked: restarted here
                shared.putRam(Desc0 + 12, 8);
                shared.putRam(Desc0 + 16, 0);
                shared.coreWasReset = true;
            }
            CHECK(read()
                    && waitFor(
                      [&]() {
                          std::lock_guard<std::mutex> const lock{said.mutex};
                          return std::ranges::count_if(said.messages,
                                                       [](auto const& m) {
                                                           return m.find("was not kept")
                                                               != std::string::npos;
                                                       })
                               > restartsBefore;
                      },
                      4000ms),
                  "boots: a count not kept is said");
            CHECK(unreadCount() == 3, "boots: and no number claimed for it");
        }

        // (f) the printer's own flash: its download wrote over the target's RAM (J-Link's flash loader; the count read
        // 9723 before, 51937 after) - the count starts again, said, never compared across it
        start(24, 2, true);   // the boot after the restarted one (1)
        CHECK(read(), "boots: a boot read before the flash");
        auto const unknownBefore = [&] {
            std::lock_guard<std::mutex> const lock{said.mutex};
            return std::ranges::count_if(said.errors, [](auto const& e) {
                return e.find("how many is unknown") != std::string::npos;
            });
        };
        auto const before = unknownBefore();
        shared.onReset    = [&]() {
            start(26, 51937, true);
        };   // the new image's first boot: a count from RAM junk
        CHECK(r->flashAndWait(8000ms).has_value(), "boots: the printer's flash");
        CHECK(read()
                && waitFor(
                  [&]() {
                      return said.anyMessageContains("the count of boots that logged starts again");
                  },
                  4000ms),
              "boots: the restart is said");
        std::this_thread::sleep_for(500ms);
        CHECK(unknownBefore() == before && unreadCount() == 3,
              "boots: no unknown, no number across the download");
        shared.onReset = {};
        r.reset();
    }

    {
        // A torn tail across a reboot ("channel 0 corrupted data removed"): a session
        // reads one and a half records, a probe read fails (the half is carried), the target reboots - its epoch moves in
        // preMemoryInit, but the .data copy re-initialises the block only after that. A session opened in between must
        // not read on from the OLD block's read offset with the carry dropped: that starts mid-record. With the epoch
        // odd while a boot initialises (RttEpoch.hpp) it waits for the new block.
        constexpr std::uint32_t        Block    = FakeTransport::Shared::RamBase;
        constexpr std::uint32_t        RingSize = 256;
        constexpr std::uint32_t        Desc0    = Block + 24;
        constexpr std::uint32_t        Ring0    = Block + 0x100;
        static constexpr std::uint32_t Epoch    = Block + 0xF0;
        auto const                     frame    = makeFrame(0);
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.ram.assign(0x100 + RingSize, std::byte{});
            std::memcpy(shared.ram.data(), "SEGGER RTT", 10);
            shared.putRam(Block + 16, 1);
            shared.putRam(Block + 20, 0);
            shared.putRam(Desc0 + 4, Ring0);
            shared.putRam(Desc0 + 8, RingSize);
            shared.ramReadDelay = 0ms;
            shared.putRam(Epoch, 20);
            for(std::size_t i = 0; i != 12; ++i) {
                shared.ram[0x100 + i] = frame[i % frame.size()];
            }
            shared.putRam(Desc0 + 12, 6);   // one record and half of the next
            shared.putRam(Desc0 + 16, 0);
            shared.connected = false;
        }
        Collected torn;
        auto      r = std::make_unique<BasicJLinkRttReader<FakeTransport>>(
          "",
          "fake",
          4000,
          "",
          []() { return RttBlockInfo{Block, 1}; },
          []() { return std::string{"fake.hex"}; },
          []() {
              return std::unordered_map<std::uint16_t, std::string>{
                {0, "hello direct"}
              };
          },
          [&torn](std::size_t, std::string_view msg, std::optional<remote_fmt::catalog_id>) {
              std::lock_guard<std::mutex> const lock{torn.mutex};
              torn.entries.emplace_back(msg);
          },
          [&torn](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{torn.mutex};
              torn.messages.emplace_back(msg);
          },
          [&torn](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{torn.mutex};
              torn.errors.emplace_back(msg);
          },
          [&torn](std::string_view msg) {
              std::lock_guard<std::mutex> const lock{torn.mutex};
              torn.tools.emplace_back(msg);
          },
          [](std::string_view) {});
        r->setRttEpochSource([]() { return std::optional<std::uint32_t>{Epoch}; });
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.connected = true;
        }
        CHECK(waitFor([&]() { return torn.anyEntryContains("hello direct"); }, 4000ms),
              "torn tail: the first record");
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(
              Desc0 + 12,
              12);   // the old boot wrote the rest, then rebooted: the epoch moved (odd: its
            shared.putRam(Epoch, 21);   // init runs), the block is still the old one
            shared.failReadFrom
              = Desc0;   // and the probe drops a read: the session ends holding half a record
            shared.failReadTo = Desc0 + 24;
        }
        std::this_thread::sleep_for(1500ms);
        {
            std::lock_guard<std::mutex> const lock{shared.mutex};
            shared.putRam(Desc0 + 12,
                          4);   // the new boot's block: re-initialised, one record of its own
            shared.putRam(Desc0 + 16, 0);
            shared.putRam(Epoch, 22);
        }
        CHECK(waitFor(
                [&]() {
                    std::lock_guard<std::mutex> const lock{torn.mutex};
                    return std::ranges::count_if(torn.entries,
                                                 [](auto const& e) {
                                                     return e.find("hello direct")
                                                         != std::string::npos;
                                                 })
                        >= 2;
                },
                4000ms),
              "torn tail: the new boot's record");
        CHECK(torn.anyMessageContains("the RTT epoch stays odd"),
              "torn tail: an epoch odd for long is said");
        bool const clean
          = !torn.anyErrorContains("corrupted data") && !torn.anyErrorContains("damaged frame");
        CHECK(clean, "torn tail: no read from the middle of a record");
        if(!clean) { torn.dump(); }
        r.reset();
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
