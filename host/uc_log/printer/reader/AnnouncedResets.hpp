#pragma once

// The reader's side of the firmware's announced resets for one session (Kvasir_SDK Runtime/AnnouncedReset.hpp has the
// firmware's side and the why, printer/AnnouncedReset.hpp the block): armed at the session start, armed again while the
// block was not there yet, polled by the read passes; a request is answered with the ack as the probe's last access,
// and the probe then stays away for the pause the firmware asked for. Included only by JLinkRttReaderImpl.hpp.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/AnnouncedReset.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/MessageLog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <optional>
#include <stop_token>

namespace uc_log::printer::reader {

template<typename TransportT>
class AnnouncedResets {
    using Clock = uc_log::printer::Clock;

public:
    using Block = uc_log::printer::announced_reset::Block;

    // how often the armed block is read (the firmware waits >= 0.5 s for the ack)
    static constexpr auto poll = std::chrono::milliseconds{10};
    // how often a session that found no block looks for it again, and how long a session start waits for it (a boot
    // copies .data a few ms after the reset)
    static constexpr auto armRetry = std::chrono::milliseconds{20};
    static constexpr auto armWait  = std::chrono::milliseconds{100};
    // how long the target gets to put its last lines into the rings before they are drained
    static constexpr auto drain = std::chrono::milliseconds{20};

    /// `address`: the block's (the map's), nullopt: the image has none. Armed now - only after the session start's
    /// flash reads: the firmware's AnnouncedReset::waitForPrinter returns once the printer listens, and a reset it then
    /// makes must not land in an XIP read (the RP2040's boot ROM hangs on it). Right after a reset .data is not copied
    /// yet: polled quietly for up to armWait, said once if the block is still not there (the read passes keep looking
    /// every armRetry).
    AnnouncedResets(MessageLog& messages         UC_LOG_LIFETIMEBOUND,
                    TransportT&                  jlink,
                    std::optional<std::uint32_t> address,
                    std::stop_token const&       stoken)
      : messages_{messages}
      , address_{address} {
        if(!address) { return; }
        auto const at  = *address;
        auto const end = Clock::now() + armWait;
        while(true) {
            if(auto const armed = arm(jlink, at, true); armed) {
                armedAt_ = armed;
                break;
            }
            if(stoken.stop_requested() || Clock::now() >= end) {
                armedAt_ = arm(jlink, at, false);   // says why
                break;
            }
            Clock::sleep_for(std::chrono::milliseconds{1});
        }
        nextArm_ = Clock::now() + armRetry;
    }

    /// A request the firmware waits on, read every Poll from the armed block; armed again every armRetry while it is
    /// not (the block was not there yet).
    std::optional<Block> pendingRequest(TransportT& jlink) {
        if(address_ && !armedAt_ && Clock::now() >= nextArm_) {
            auto const at = *address_;
            nextArm_      = Clock::now() + armRetry;
            armedAt_      = arm(jlink, at, true);
        }
        if(!armedAt_ || Clock::now() < nextPoll_) { return std::nullopt; }
        nextPoll_        = Clock::now() + poll;
        auto const block = readBlock(jlink, *armedAt_);
        if(!block || !block->pending()) { return std::nullopt; }
        return block;
    }

    /// The firmware waits a bounded time (~0.5 s). A printer slow to see the request - busy with a halt it caught, a
    /// slow probe - finds it reset already: writing then would clear the id of the new boot's control block and ack
    /// into its block. So the request must still wait.
    bool stillPending(TransportT&  jlink,
                      Block const& asked) const {
        if(!armedAt_) { return false; }
        auto const still = readBlock(jlink, *armedAt_);
        return still && still->pending() && still->request == asked.request;
    }

    /// The pause `asked` gets (the firmware may ask for longer: a watchdog it lets run out), kept as pause().
    std::chrono::milliseconds takePause(Block const& asked) {
        pause_ = std::clamp(std::chrono::milliseconds{asked.stayAwayMs()},
                            uc_log::printer::announced_reset::printerPause,
                            uc_log::printer::announced_reset::printerPauseMax);
        return pause_;
    }

    [[nodiscard]] std::chrono::milliseconds pause() const { return pause_; }

    /// The answer, the last access before the probe stays away.
    void ack(TransportT&  jlink,
             Block const& asked) const {
        if(!armedAt_) { return; }
        jlink.writeWord(*armedAt_ + uc_log::printer::announced_reset::ackOffset, asked.request);
    }

    /// A printer that goes disarms the block: a later reset of the firmware must not wait for it.
    void disarm(TransportT& jlink) const noexcept {
        if(!armedAt_) { return; }
        try {
            jlink.writeWord(*armedAt_ + uc_log::printer::announced_reset::armedOffset, 0);
        } catch(
          std::
            exception const&) { /* ignored: a probe gone with the printer has nothing to disarm */
        }
    }

private:
    static std::optional<Block> readBlock(TransportT&   jlink,
                                          std::uint32_t address) {
        std::array<std::byte, uc_log::printer::announced_reset::blockSize> raw{};
        try {
            jlink.readMemory(address, raw);
        } catch(std::exception const&) { return std::nullopt; }
        return uc_log::printer::announced_reset::decode(raw);
    }

    // The block's address once armed (the firmware then waits for our ack before resetting itself); nullopt: not
    // armed now. `retry`: no message unless it succeeds.
    std::optional<std::uint32_t> arm(TransportT&   jlink,
                                     std::uint32_t address,
                                     bool          retry) {
        auto const block = readBlock(jlink, address);
        if(!block || !block->valid()) {
            if(!retry) {
                messages_.tool(fmt::format(
                  "announced resets: no block at {:#010x} yet ({} not set up or not readable) - "
                  "looking again every {} ms",
                  address,
                  uc_log::printer::announced_reset::symbol,
                  armRetry.count()));
            }
            return std::nullopt;
        }
        try {
            jlink.writeWord(address + uc_log::printer::announced_reset::armedOffset,
                            uc_log::printer::announced_reset::hostArmed);
        } catch(std::exception const& e) {
            if(!retry) {
                messages_.toolError(
                  fmt::format("announced resets: arming the block at {:#010x} failed: {}",
                              address,
                              e.what()));
            }
            return std::nullopt;
        }
        messages_.tool(fmt::format("announced resets: listening at {:#010x} (request {}, ack {})",
                                   address,
                                   block->request,
                                   block->ack));
        return address;
    }

    MessageLog&                  messages_;
    std::optional<std::uint32_t> address_;
    std::optional<std::uint32_t> armedAt_;
    Clock::time_point            nextPoll_{};
    Clock::time_point            nextArm_{};
    std::chrono::milliseconds    pause_{uc_log::printer::announced_reset::printerPause};
};

}   // namespace uc_log::printer::reader
