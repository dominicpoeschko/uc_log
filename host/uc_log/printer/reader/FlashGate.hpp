#pragma once

// The reader's reads of the image in flash (the firmware check, the decode gate, a control-socket read in the flash
// window) and its looks at DHCSR.S_RESET_ST, which those reads share. Included only by JLinkRttReaderImpl.hpp.
//
// Only while the flash is mapped: while a boot ROM drives the flash interface by hand, a read through the XIP window
// collides with its transfer - the RP2040's ROM, reading boot2 in plain SPI mode, then waits for good, and the XIP
// cache fills with junk. The chip package names the register words of a mapped flash (ReaderOptions::flashReady,
// --flash_ready_signature); the first read of a reader waits up to readyWait for them, every later one reads them
// first. Not mapped: the read fails (the check says "not checked"), and a boot that never maps it is the stall
// check's. A core-0-only reset (SYSRESETREQ on the RP2040) leaves the SSI as boot2 set it, so the words still say
// "mapped" while the boot ROM takes the flash over: S_RESET_ST is read before and after every read - a reset seen, the
// read is dropped and no further one is made through that reader (the bit goes on to resetSinceLastLook).

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/JLinkRttReader.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/TargetHealth.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uc_log::printer::reader {

template<typename TransportT>
class FlashGate {
    using Clock = uc_log::printer::Clock;

public:
    using Condition = uc_log::printer::StallSignature::Condition;
    /// Before any waiting for the flash, the health checks once: a chip the DLL parked in its boot ROM (the RP2040's)
    /// is reset first, as before the gate existed, not polled for seconds. True: the reader dealt with the target (a
    /// reset or a recovery follows), the read gives up.
    using HealthLook = std::function<bool(TransportT&)>;

    static constexpr auto readyWait = std::chrono::milliseconds{3000};
    // few probe accesses while a boot ROM may run: fast polling of the flash interface can wedge an RP2040's boot ROM
    static constexpr auto readyPoll = std::chrono::milliseconds{50};

    FlashGate(
      MessageLog& messages                    UC_LOG_LIFETIMEBOUND,
      std::vector<Condition>                  ready,
      std::optional<std::pair<std::uint32_t,
                              std::uint32_t>> window,
      // a std::function by value: clang 23's -Wlifetime-safety-intra-tu-constructor-suggestions
      // takes it for a view and asks for [[clang::lifetimebound]] (an error in this build)
      // NOLINTNEXTLINE(modernize-pass-by-value) // see above
      HealthLook const& healthLook)
      : messages_{messages}
      , ready_{std::move(ready)}
      , window_{std::move(window)}
      , healthLook_{healthLook} {}

    /// True when the core was reset since the last look (reading clears the bit - a look a flash read took counts); a
    /// failed read says no.
    bool resetSinceLastLook(TransportT& jlink) {
        bool const aside = std::exchange(resetSeenAside_, false);
        auto const dhcsr = uc_log::printer::tryReadWord(jlink, uc_log::printer::cortex_m::dhcsr);
        return aside || (dhcsr && (*dhcsr & uc_log::printer::cortex_m::dhcsrResetSt) != 0);
    }

    /// A reader of the flash through the gate, for one connection.
    uc_log::printer::DirectMemoryRead
    read(TransportT& jlink UC_LOG_LIFETIMEBOUND) UC_LOG_LIFETIMEBOUND {
        return [this, &jlink, first = true, stopped = false](std::uint32_t        address,
                                                             std::span<std::byte> out) mutable {
            if(stopped) { return false; }
            auto const wait
              = std::exchange(first, false) ? readyWait : std::chrono::milliseconds{0};
            if(!mapped(jlink, wait) || resetDuringRead(jlink)) {
                stopped = stopped || resetSeenAside_;
                return false;
            }
            try {
                jlink.readMemory(address, out);
            } catch(std::exception const&) { return false; }
            if(resetDuringRead(jlink)) {
                stopped = true;
                return false;
            }
            return true;
        };
    }

    /// [address, address + size) touches the chip's flash window (ReaderOptions::flashWindow).
    [[nodiscard]] bool inWindow(std::uint32_t address,
                                std::uint32_t size) const {
        if(!window_) { return false; }
        auto const [base, length] = *window_;
        return std::uint64_t{address} + size > base && address < std::uint64_t{base} + length;
    }

private:
    // A flash read's look at S_RESET_ST: set -> the bit is kept for resetSinceLastLook, and true.
    bool resetDuringRead(TransportT& jlink) {
        auto const dhcsr = uc_log::printer::tryReadWord(jlink, uc_log::printer::cortex_m::dhcsr);
        if(!dhcsr || (*dhcsr & uc_log::printer::cortex_m::dhcsrResetSt) == 0) { return false; }
        resetSeenAside_ = true;
        return true;
    }

    // every condition holds on one read, within `wait`
    bool mapped(TransportT&               jlink,
                std::chrono::milliseconds wait) {
        auto const deadline = Clock::now() + wait;
        bool       checked  = wait.count() == 0;
        while(true) {
            bool const isMapped = std::ranges::all_of(ready_, [&jlink](Condition const& c) {
                auto const word = uc_log::printer::tryReadWord(jlink, c.address);
                return word && (*word & c.mask) == c.value;
            });
            if(isMapped) { return true; }
            if(!std::exchange(checked, true) && healthLook_(jlink)) { return false; }
            if(Clock::now() >= deadline) { break; }
            Clock::sleep_for(readyPoll);
        }
        if(wait.count() != 0) {
            messages_.tool(
              fmt::format("the flash is not mapped after {} ms (the boot ROM drives it?): the "
                          "firmware check does not read it",
                          wait.count()));
        }
        return false;
    }

    MessageLog&                                            messages_;
    std::vector<Condition>                                 ready_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> window_;
    HealthLook                                             healthLook_;
    bool resetSeenAside_{};   // S_RESET_ST a flash read saw, for the next resetSinceLastLook
};

}   // namespace uc_log::printer::reader
