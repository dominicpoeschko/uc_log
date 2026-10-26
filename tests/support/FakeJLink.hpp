#pragma once

// The rtt_reader tests' fake J-Link and the pieces every case uses (tests/rtt_reader/*_tests.hpp, one file of cases per
// concern, one binary: rtt_reader_tests.cpp): FakeTransport and its Shared target state, Collected (what a reader said),
// Scenario (a fresh Shared per case), Setup + makeReader / ReaderUnderTest (the reader as the printer constructs it),
// RamTarget, the stall reader, FlashGate. The reader on FakeTransport is instantiated in rtt_reader_tests.cpp.
// Every scenario is a case of its own (uc_log::test::run): a fresh fake and its own reader, nothing left over from the
// case before, so each runs alone (`--case <name>`) and ctest runs them as separate entries (tests/CMakeLists.txt
// CASES - a new case goes into its file's table and that list).
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/wire.hpp"
#include "support/Check.hpp"
#include "support/FakeRtt.hpp"
#include "uc_log/printer/AnnouncedReset.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/JLinkRttReader.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/RttChannel.hpp"
#include "uc_log/printer/RttControlBlock.hpp"
#include "uc_log/printer/TargetOp.hpp"

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
#include <utility>
#include <vector>

namespace uc_log_test_rtt_reader {
using namespace std::chrono_literals;
using uc_log::printer::Clock;

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
        std::mutex mutex;

        // ---- the link: the DLL's connection, its status and RTT, the control block it finds
        std::deque<std::byte> upData;
        // isRunning 1 (FakeStatus' default): a running RTT link; a "not connected" status is spelled isRunning = 0
        FakeStatus status{};
        bool       connected{true};
        // a one-shot probe failure: the next RAM read inside [failReadFrom, failReadTo) throws (as JLINK_ReadMemHW does
        // when the J-Link drops off USB), then reads work again
        std::uint32_t    failReadFrom{};
        std::uint32_t    failReadTo{};
        std::atomic<int> constructions{0};
        // the DLL lost the target (every access fails -1, as after the RP2040 stall recovery's watchdog reset): a new
        // connection sees it again - or, with deadAcrossConnects, only a reset through the reset line does
        std::atomic<bool> apDead{false};
        std::atomic<bool> deadAcrossConnects{false};
        std::atomic<int>  slowWriteMs{0};            // a writeWord takes this long (a busy probe)
        std::atomic<bool> holdSessionStart{false};   // startRtt waits while set
        // the DLL holds data the printer has not fetched yet: rttRead hands it out only after stopRtt
        std::atomic<bool> holdUntilRttStop{false};
        std::atomic<bool> blockGone{false};   // no RTT id at the block address (a boot ROM runs)
        // the RTT control block at 0x20000100 with one buffer, as words (id, counts, descriptor);
        // empty: the address pattern, which holds no "SEGGER RTT" id
        std::optional<std::array<std::uint32_t, 12>> controlBlock;   // guarded by mutex

        // ---- memory: what the printer wrote, and reads that fail (RAM and rings: below)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;   // guarded by mutex
        // after every word the printer writes into RAM, with mutex held (a target that refills its ring at once:
        // a flood that does not depend on a thread getting the CPU)
        std::function<void()> afterRamWrite;            // guarded by mutex
        std::atomic<bool>     stackUnreadable{false};   // reads near sp throw

        // ---- flash: flash() calls, the flash window's reads, a wedged flash interface
        std::atomic<int>     flashFailures{0};   // the next so many flash() calls throw
        std::atomic<int>     flashes{0};
        std::atomic<int>     ramFlashes{0};    // flashRamImage(): reset, download, start
        JLink::RamImageStart lastRamStart{};   // guarded by mutex
        // the flash window (an RP2040's XIP): a read while the SSI word in RAM at ssiWord says the boot ROM drives the
        // flash in plain SPI mode (TMOD 0, not EEPROM_READ) hangs the ROM, as the boot ROM stall did, and reads junk
        std::atomic<bool> romHung{false};
        std::atomic<int>  flashReads{0};
        std::atomic<int>  resetAfterFlashReads{
          -1};   // the core resets (S_RESET_ST) right after that many flash reads
        std::atomic<int> flashReadsAtArm{
          -1};   // flash reads made when the announce block was armed
        // a flash interface stuck where no register shows it: every flash() throws until the core has run after a reset
        // (the boot ROM booted the image once)
        std::atomic<bool> wedged{false};
        std::atomic<bool> resetSinceGo{false};
        std::atomic<bool> wedgeOutlivesBoot{false};   // a wedge nothing frees

        // ---- resets and halts: the core's state (DHCSR), resets by every way, the stall recovery's
        std::atomic<bool> halted{false};
        std::atomic<int>  breakpointClears{0};
        std::atomic<int>  resetTypeSet{-1};   // the last JLINK_SetResetType
        std::array<std::optional<std::uint32_t>, 19> writtenRegisters{};   // guarded by mutex
        std::atomic<int>                             haltPolls{0};
        std::atomic<int>                             goCalls{0};
        std::atomic<bool> haltsOnGo{false};      // go() runs into a bkpt at once (a raised panic)
        std::atomic<bool> coreWasReset{false};   // DHCSR.S_RESET_ST: set by a reset, read clears it
        std::atomic<bool> lockup{false};   // DHCSR.S_LOCKUP; a halted core's PC then reads lockupPc
        std::atomic<std::uint32_t> lockupPc{
          0xFFFF'FFFEU};   // Armv6-M/v7-M; an Armv8-M core reads 0xEFFFFFFE
        std::atomic<bool> asleep{
          false};   // DHCSR.S_SLEEP: the core waits in a wfi (the RP2040's park loop)
        std::atomic<int>                resets{0};
        std::function<void()>           onReset;
        std::vector<JLink::MemoryWrite> preResetAtLastReset;   // guarded by mutex
        std::atomic<bool>               resetLineFrees{true};
        std::atomic<int>                resetLineResets{0};
        std::atomic<int>  resetLinePulses{0};   // of those, by a pulse of the pin (pulseResetPin)
        std::atomic<bool> deadOnRecoveryWrite{
          false};   // the WATCHDOG trigger write (0x400d8000) kills the access
        // a stall only a SYSRESETREQ frees (AIRCR 0xE000ED0C = 0x05FA0004): the word at stuckWord goes to 0 then
        std::atomic<std::uint32_t> stuckWord{0};
        std::atomic<bool>          sysresetFrees{false};
        std::atomic<int>           sysresets{0};
        std::atomic<long long>     recoveryAtUs{
          0};   // when the WATCHDOG trigger was written (steady clock, us)
        std::atomic<long long> connectAfterRecoveryUs{0};   // the first connection after it

        // target RAM for the printer's own RTT reader (DirectRtt): [ramBase, ramBase + ram.size()); empty: none.
        // Reads there take ramReadDelay (a probe's round trip).
        static constexpr std::uint32_t ramBase   = 0x2000'1000;
        static constexpr std::uint32_t flashBase = 0x1000'0000;
        static constexpr std::uint32_t ssiWord
          = ramBase + 0x7f0;             // the fake SSI's CTRLR0, then SSIENR
        std::vector<std::byte>    ram;   // guarded by mutex
        std::chrono::microseconds ramReadDelay{0};

        [[nodiscard]] bool inRam(std::uint32_t address,
                                 std::size_t   n) const {
            return !ram.empty() && address >= ramBase && address + n <= ramBase + ram.size();
        }

        void putRam(std::uint32_t address,
                    std::uint32_t value) {
            for(std::size_t i = 0; i != 4; ++i) {
                ram[address - ramBase + i] = static_cast<std::byte>(value >> (8 * i));
            }
        }

        [[nodiscard]] std::uint32_t getRam(std::uint32_t address) const {
            std::uint32_t v{};
            for(std::size_t i = 0; i != 4; ++i) {
                v |= std::to_integer<std::uint32_t>(ram[address - ramBase + i]) << (8 * i);
            }
            return v;
        }

        // an announced reset: the block at announceAddress, words magic armed request ack
        static constexpr std::uint32_t announceAddress = 0x2000'0300;
        std::array<std::uint32_t, 4>   announce{};         // guarded by mutex
        std::atomic<bool>              resetting{false};   // set by the ack: the chip resets
        std::atomic<Clock::time_point> ackedAt{};          // when the ack was written
        std::atomic<int>               touchedWhileResetting{0};
        std::atomic<int>               open{0};   // transports alive
        std::atomic<int>               closedWhileResetting{0};
        std::atomic<int>               rttStops{0};
        // the firmware's wait for the ack runs out right after the printer's first read of a
        // request: the chip resets, and the new boot's .data holds a fresh block
        std::atomic<bool> gaveUpBeforeAck{false};

        // every call that reaches the target; one while the chip resets is the bug
        void touch() {
            if(resetting) { ++touchedWhileResetting; }
        }

        void feed(std::vector<std::byte> const& data) {
            std::scoped_lock<std::mutex> const lock{mutex};
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
                                               Clock::now().time_since_epoch())
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

    void setResetType(std::uint8_t type) {
        resetType            = type;
        shared->resetTypeSet = type;
    }

    std::vector<JLink::MemoryWrite> preResetCommands;

    void setPreResetCommands(std::vector<JLink::MemoryWrite> commands) {
        preResetCommands = std::move(commands);
    }

    // JLINK_ClrRESET / JLINK_SetRESET: one pulse on the pin, no reset strategy
    void pulseResetPin() {
        ++shared->resetLinePulses;
        auto const type = std::exchange(resetType, std::uint8_t{2});
        resetTarget();
        resetType = type;
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
            std::scoped_lock<std::mutex> const lock{shared->mutex};
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
            std::scoped_lock<std::mutex> const lock{shared->mutex};
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
        std::scoped_lock<std::mutex> const lock{shared->mutex};
        shared->writtenRegisters[static_cast<std::size_t>(r)] = value;
    }

    std::uint32_t readRegister(CoreRegister r) {
        shared->touch();
        {
            std::scoped_lock<std::mutex> const lock{shared->mutex};
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
        shared->halted = shared->haltsOnGo.load();
    }

    void halt() {
        shared->touch();
        shared->halted = true;
    }

    void clearAllBreakpoints() {
        shared->touch();
        ++shared->breakpointClears;
    }

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
                std::scoped_lock<std::mutex> const lock{shared->mutex};
                shared->putRam(shared->stuckWord, 0);
            }
        }
        if(address == 0x400d'8000 && value == 0x8000'0000) {
            shared->recoveryAtUs           = std::chrono::duration_cast<std::chrono::microseconds>(
                                               Clock::now().time_since_epoch())
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
            Clock::sleep_for(std::chrono::milliseconds{ms});
        }
        std::scoped_lock<std::mutex> const lock{shared->mutex};
        if(shared->inRam(address, 4)) {
            shared->putRam(address, value);
            if(shared->afterRamWrite) { shared->afterRamWrite(); }
            return;
        }
        shared->writes.emplace_back(address, value);
        if(address == 0x20000100 && value == 0) { shared->blockGone = true; }   // the id cleared
        if(shared->controlBlock && address >= 0x20000100 && address < 0x20000100 + 48) {
            (*shared->controlBlock)[(address - 0x20000100) / 4] = value;
        }
        if(address >= Shared::announceAddress && address < Shared::announceAddress + 16) {
            if(address == Shared::announceAddress + 4 && value != 0) {
                shared->flashReadsAtArm = shared->flashReads.load();
            }
            shared->announce[(address - Shared::announceAddress) / 4] = value;
            if(address == Shared::announceAddress + 12) {   // the ack
                shared->ackedAt   = Clock::now();
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
                std::memcpy(out.data(), &shared->ram[address - Shared::ramBase], out.size());
                lock.unlock();
                Clock::sleep_for(delay);
                return;
            }
        }
        if(address >= Shared::flashBase && address < Shared::flashBase + 0x10'0000) {
            std::scoped_lock<std::mutex> const lock{shared->mutex};
            std::uint32_t                      ssi{};
            if(shared->inRam(Shared::ssiWord,
                             4)) {   // RAM too small for it: 0, the boot ROM's plain SPI
                std::memcpy(&ssi, &shared->ram[Shared::ssiWord - Shared::ramBase], 4);
            }
            bool const xip = (ssi & 0x300U) == 0x300U;
            if(!xip) { shared->romHung = true; }
            if(++shared->flashReads == shared->resetAfterFlashReads) {
                shared->coreWasReset = true;
            }
            std::ranges::fill(out, static_cast<std::byte>(xip ? 0x5aU : 0xeeU));
            return;
        }
        if(address == Shared::announceAddress && out.size() == 16) {
            std::scoped_lock<std::mutex> const lock{shared->mutex};
            for(std::size_t i = 0; i != out.size(); ++i) {
                out[i] = static_cast<std::byte>(shared->announce[i / 4] >> (8 * (i % 4)));
            }
            if(shared->gaveUpBeforeAck && shared->announce[2] != 0) {
                shared->gaveUpBeforeAck = false;
                shared->announce        = {uc_log::printer::announced_reset::magicValue, 0, 0, 0};
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
            std::scoped_lock<std::mutex> const lock{shared->mutex};
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
        std::scoped_lock<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
    }

    FakeStatus readStatus() {
        shared->touch();
        std::scoped_lock<std::mutex> const lock{shared->mutex};
        if(!shared->connected) { throw std::runtime_error{"fake transport disconnected"}; }
        return shared->status;
    }

    FakeStatus startRtt(std::uint32_t,
                        std::uint32_t) {
        while(shared->holdSessionStart) { Clock::sleep_for(1ms); }
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
        std::scoped_lock<std::mutex> const lock{shared->mutex};
        std::size_t const                  n = std::min(buffer.size(), shared->upData.size());
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
inline std::vector<std::byte> makeFrame(std::uint8_t catalogId) {
    return {remote_fmt::protocol::startMarker,
            std::byte{catalogId},
            std::byte{0},
            remote_fmt::protocol::endMarker};
}

struct Collected {
    std::mutex               mutex;
    std::vector<std::string> entries;
    std::vector<std::string> messages;
    std::vector<std::string> tools;   // the tool's own messages (MessageLevel::tool)
    std::vector<std::string> errors;
    std::vector<uc_log::printer::HaltCapture>
      halts;   // MessageKind::halt (its text is in errors too)
    // the reader's ReaderSink::messages: status, error and tool messages land in the lists above (tool errors are not
    // collected)
    uc_log::printer::MessageLog log;

    Collected() {
        log.addObserver([this](uc_log::printer::MessageLog::Said const& said) {
            using uc_log::printer::MessageLevel;
            std::scoped_lock<std::mutex> const lock{mutex};
            switch(said.level) {
            case MessageLevel::status:    messages.emplace_back(said.text); break;
            case MessageLevel::fatal:
            case MessageLevel::error:     errors.emplace_back(said.text); break;
            case MessageLevel::tool:      tools.emplace_back(said.text); break;
            case MessageLevel::toolError: break;
            }
            if(said.kind == uc_log::printer::MessageKind::halt && said.halt != nullptr) {
                halts.push_back(*said.halt);
            }
        });
    }

    Collected(Collected const&)            = delete;
    Collected& operator=(Collected const&) = delete;

    bool anyEntryContains(std::string_view needle) {
        std::scoped_lock<std::mutex> const lock{mutex};
        return std::ranges::any_of(entries, [&](auto const& e) {
            return e.find(needle) != std::string::npos;
        });
    }

    bool anyErrorContains(std::string_view needle) {
        std::scoped_lock<std::mutex> const lock{mutex};
        return std::ranges::any_of(errors, [&](auto const& e) {
            return e.find(needle) != std::string::npos;
        });
    }

    bool anyToolContains(std::string_view needle) {
        std::scoped_lock<std::mutex> const lock{mutex};
        return std::ranges::any_of(tools, [&](auto const& t) {
            return t.find(needle) != std::string::npos;
        });
    }

    // for a failed check: what the reader said, in order of kind
    void dump() {
        std::scoped_lock<std::mutex> const lock{mutex};
        for(auto const& t : tools) { std::printf("    tool:  %s\n", t.c_str()); }
        for(auto const& m : messages) { std::printf("    msg:   %s\n", m.c_str()); }
        for(auto const& e : errors) { std::printf("    error: %s\n", e.c_str()); }
    }

    bool anyMessageContains(std::string_view needle) {
        std::scoped_lock<std::mutex> const lock{mutex};
        return std::ranges::any_of(messages, [&](auto const& m) {
            return m.find(needle) != std::string::npos;
        });
    }
};

template<typename Predicate>
bool waitFor(Predicate&&               predicate,
             std::chrono::milliseconds timeout) {
    auto const deadline = Clock::now() + timeout;
    while(Clock::now() < deadline) {
        if(predicate()) { return true; }
        Clock::sleep_for(5ms);
    }
    return predicate();
}

using Reader = BasicJLinkRttReader<FakeTransport>;

namespace rb = uc_log::printer::rtt_block;
using uc_log::test::FakeMemory;
using uc_log::test::FakeRttBlock;
using uc_log::test::RingSpec;

// One case's fake target: a fresh Shared, nothing left over from the case before. FakeTransport::shared points at it for
// the case's lifetime. Declare it first in a case: every reader of the case is then gone before it.
struct Scenario {
    FakeTransport::Shared shared;

    Scenario() { FakeTransport::shared = &shared; }

    ~Scenario() { FakeTransport::shared = nullptr; }

    Scenario(Scenario const&)            = delete;
    Scenario& operator=(Scenario const&) = delete;
};

// What a case sets of ImageSource beyond the three every reader is given (block, hexFile, catalog: makeReader), in
// ImageSource's order.
struct ImageExtras {
    std::function<remote_fmt::FrameCheck()>                       frameCheck{};
    std::function<std::optional<std::uint32_t>()>                 announcedReset{};
    std::function<std::optional<std::uint32_t>()>                 rttEpoch{};
    std::function<std::optional<std::uint32_t>()>                 loggedBoots{};
    std::function<uc_log::printer::RamImageStartResult()>         ramImage{};
    std::function<bool(uc_log::printer::DirectMemoryRead const&)> decodeGate{};
    std::function<void(uc_log::printer::DirectMemoryRead const&)> onSessionStart{};
};

// What a case sets of the reader: the image's block and its one catalog string (default: the address-pattern target's
// block, "hello from target"), a hook after an entry is collected, the unread-boots hand-over, and the rest of the
// reader's configuration as the printer gives it.
struct Setup {
    // the map's block address and buffer count; the default is the address-pattern target's block
    std::function<uc_log::printer::RttBlockInfo()> block
      = []() { return uc_log::printer::RttBlockInfo{0x20000100, 1}; };
    std::string           catalogText = "hello from target";   // catalog id 0
    std::function<void()> onEntry{};   // after an entry is collected (a slow consumer, a counter)
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> unreadBoots{};
    ImageExtras                                                      image{};
    uc_log::printer::ReaderOptions                                   options{};
};

// The reader on the fake transport (Scenario's), constructed in this one place; what it says goes into `into`.
inline std::unique_ptr<Reader> makeReader(Collected& into,
                                          Setup      setup = {}) {
    auto&                        extras = setup.image;
    uc_log::printer::ImageSource image{.block   = std::move(setup.block),
                                       .hexFile = []() { return std::string{"fake.hex"}; },
                                       .catalog =
                                         [text = std::move(setup.catalogText)]() {
                                             // a described string without arguments (remote_fmt: the format string, 0x1e, no descriptors)
                                             return std::unordered_map<std::uint16_t, std::string>{
                                               {0, text + '\x1e'}
                                             };
                                         },
                                       .frameCheck     = std::move(extras.frameCheck),
                                       .announcedReset = std::move(extras.announcedReset),
                                       .rttEpoch       = std::move(extras.rttEpoch),
                                       .loggedBoots    = std::move(extras.loggedBoots),
                                       .ramImage       = std::move(extras.ramImage),
                                       .decodeGate     = std::move(extras.decodeGate),
                                       .onSessionStart = std::move(extras.onSessionStart)};
    return std::make_unique<Reader>(
      uc_log::printer::ReaderConnection{.host = "", .device = "fake", .speed = 4000, .probe = ""},
      uc_log::printer::ReaderSink{
        .messages = into.log,
        .entry =
          [&into, onEntry = std::move(setup.onEntry)](std::size_t,
                                                      std::string_view msg,
                                                      std::optional<remote_fmt::catalog_id>) {
              {
                  std::scoped_lock<std::mutex> const lock{into.mutex};
                  into.entries.emplace_back(msg);
              }
              if(onEntry) { onEntry(); }
          },
        .unreadBoots = std::move(setup.unreadBoots)},
      std::move(image),
      std::move(setup.options));
}

// A reader and what it said. `said` comes first: the reader, whose messages go into it, is destroyed before it.
struct ReaderUnderTest {
    Collected               said;
    std::unique_ptr<Reader> reader;

    explicit ReaderUnderTest(Setup setup = {})
      : reader{makeReader(said,
                          std::move(setup))} {}
};

// ImageSource::announcedReset of an image with an announced-reset block: the fake's
inline std::optional<std::uint32_t> announceBlock() {
    return FakeTransport::Shared::announceAddress;
}

// Target RAM at ramBase (`bytes` of it, zeroed) with an RTT control block at its start: the up rings given, no down
// ring. Built before the case's first reader (nothing reads the RAM yet).
struct RamTarget {
    static constexpr std::uint32_t block = FakeTransport::Shared::ramBase;

    FakeMemory   memory;
    FakeRttBlock rtt;

    RamTarget(FakeTransport::Shared& shared UC_LOG_LIFETIMEBOUND,
              std::size_t                   bytes,
              std::span<RingSpec const>     up)
      : memory{cleared(shared.ram,
                       bytes),
               block}
      , rtt{memory,
            block,
            up} {}

private:
    static std::span<std::byte> cleared(std::vector<std::byte>& ram UC_LOG_LIFETIMEBOUND,
                                        std::size_t                 bytes) {
        ram.assign(bytes, std::byte{});
        return ram;
    }
};

// the ring most cases read: 1 KiB at ramBase + 0x100
inline constexpr std::array<RingSpec, 1> oneKiBRing{
  RingSpec{.buffer = FakeTransport::Shared::ramBase + 0x100, .size = 1024}
};

// the stall cases' word (a QMI-like busy flag) and their reader: the signature and the package's recovery writes
inline constexpr std::uint32_t stallWord = FakeTransport::Shared::ramBase + 0x700;

inline std::unique_ptr<Reader>
stallReader(FakeTransport::Shared&                                        shared,
            Collected&                                                    quiet,
            std::uint32_t                                                 wordValue,
            std::uint32_t                                                 secondValue,
            int                                                           firstStep = 1,
            std::function<bool(uc_log::printer::DirectMemoryRead const&)> gate      = {}) {
    constexpr std::uint32_t word = stallWord;
    {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.putRam(word, wordValue);
        shared.putRam(word + 4, secondValue);
        shared.ramReadDelay = 0ms;
        shared.writes.clear();
    }
    Reader::StallSignature stall{
      {                            {word, 0x3, 0x3},         {word + 4, 0xffff'ffff, 0x0007'0000}},
      {JLink::MemoryWrite{0x4001'8008, 0x01ff'fffe}, JLink::MemoryWrite{0x400d'8000, 0x8000'0000}}
    };
    return makeReader(
      quiet,
      {
        .block = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
        .catalogText = "hello direct",
        .image       = {.decodeGate = std::move(gate)},
        .options     = {.stall = std::move(stall), .stallFirstStep = firstStep}
    });
}

// the flash-ready gate's target: a control block with one up ring whose name lies in flash (an image's .rodata)
inline constexpr std::array<RingSpec, 1> flashGateRing{
  RingSpec{.buffer = FakeTransport::Shared::ramBase + 0x100,
           .size   = 256,
           .name   = FakeTransport::Shared::flashBase + 0x200}
};

struct FlashGate {
    FakeTransport::Shared& shared;
    RamTarget              target;
    Collected              flashCheck;

    explicit FlashGate(FakeTransport::Shared& s UC_LOG_LIFETIMEBOUND)
      : shared{s}
      , target{s,
               0x800,
               flashGateRing} {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.blockGone    = false;
        shared.ramReadDelay = 0ms;
    }

    void put(std::uint32_t address,
             std::uint32_t v) {
        std::scoped_lock<std::mutex> const lock{shared.mutex};
        shared.putRam(address, v);
    }

    void ssi(std::uint32_t v) { put(FakeTransport::Shared::ssiWord, v); }

    std::unique_ptr<Reader> start(std::vector<std::byte>& got,
                                  std::atomic<int>&       results,
                                  std::atomic<bool>&      readOk) {
        auto const sessionStart = [&](uc_log::printer::DirectMemoryRead const& read) {
            std::vector<std::byte> b(16);
            readOk = read(FakeTransport::Shared::flashBase + 0x100, b);
            got    = std::move(b);
            ++results;
        };
        return makeReader(
          flashCheck,
          {.block
           = []() { return uc_log::printer::RttBlockInfo{FakeTransport::Shared::ramBase, 1}; },
           .catalogText = "hello direct",
           .image       = {.onSessionStart = sessionStart},
           .options     = {.flashReady = {{FakeTransport::Shared::ssiWord, 0x300, 0x300},
                                          {FakeTransport::Shared::ssiWord + 4, 0x1, 0x1}}}});
    }
};

}   // namespace uc_log_test_rtt_reader
