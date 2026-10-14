#pragma once

#include "remote_fmt/parser.hpp"
#include "uc_log/detail/Clock.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uc_log { namespace detail {

    // receive buffer and frame extraction for one RTT up channel. Decoupled from the
    // transport: the caller appends raw bytes, so the framing/resync logic is testable
    // without hardware.
    struct RttChannel {
        using Clock = uc_log::detail::Clock;

        // a corrupted stream must resync in bulk (not one byte per timeout) and the buffer
        // must never grow without bound when the end marker never arrives
        static constexpr std::size_t MaxBufferSize = 1024 * 1024;
        static constexpr auto        FrameTimeout  = std::chrono::milliseconds{100};

        std::vector<std::byte> buffer;
        Clock::time_point      lastValidRead{Clock::now()};
        // what the image's catalog says about its frames (remote_fmt::FrameCheck): set at each session start
        remote_fmt::FrameCheck frameCheck{remote_fmt::FrameCheck::either};

        void append(std::span<std::byte const> data) {
            buffer.insert(buffer.end(), data.begin(), data.end());
        }

        // drain all complete frames, one printF(channel, message, catalog id) call per decoded
        // message. haltedRecently gates the timeout-based resync (a halted target legitimately
        // pauses mid-frame), the size cap does not. Never cut short: what was read from the target
        // is decoded whole. Returns true when at least one message was decoded.
        template<typename PrintF,
                 typename ErrorMessageF>
        bool drain(PrintF&&                               printF,
                   std::size_t                            channelId,
                   std::unordered_map<std::uint16_t,
                                      std::string> const& stringConstantsMap,
                   ErrorMessageF&&                        errorMessagef,
                   bool                                   haltedRecently) {
            if(buffer.empty()) {
                lastValidRead = Clock::now();
                return false;
            }

            bool        gotMessage{};
            std::size_t unparsedTotal{};
            auto const  decode = [&] {
                std::span<std::byte const> remaining{buffer};
                while(true) {
                    auto const parsed = remote_fmt::parseMessage(remaining,
                                                                 stringConstantsMap,
                                                                 errorMessagef,
                                                                 frameCheck);
                    remaining         = parsed.remaining;
                    unparsedTotal += parsed.discarded;
                    if(parsed.message) {
                        lastValidRead = Clock::now();
                        gotMessage    = true;
                        printF(channelId, *parsed.message, parsed.catalogId);
                        continue;
                    }
                    break;
                }
                // single compaction per pass instead of one front-erase per message
                buffer.erase(buffer.begin(),
                             std::next(buffer.begin(),
                                       static_cast<std::make_signed_t<std::size_t>>(
                                         buffer.size() - remaining.size())));
            };
            decode();

            if(!buffer.empty()) {
                bool const oversized = buffer.size() > MaxBufferSize;
                bool const timedOut
                  = !haltedRecently && Clock::now() > lastValidRead + FrameTimeout;
                if(oversized || timedOut) {
                    // The head has been stuck for a whole timeout: what lies behind it arrived in
                    // the same stall, so a head that is not the buffer's last frame start is stuck
                    // too - resync through all of them now, not one per timeout (a target that
                    // restarts without a reset while the DLL reads can hand over a ring's worth of
                    // stale bytes: one frame per timeout took minutes for those). The last frame
                    // start may still be arriving; it gets its own timeout.
                    std::size_t dropped{};
                    std::size_t frames{};
                    do {
                        dropped += dropHeadFrame();
                        ++frames;
                        decode();
                    } while(hasLaterFrameStart());
                    lastValidRead = Clock::now();
                    errorMessagef(
                      fmt::format("channel {} resync: dropped {} byte{} of {} stuck frame{}",
                                  channelId,
                                  dropped,
                                  dropped == 1 ? "" : "s",
                                  frames,
                                  frames == 1 ? "" : "s"));
                }
            }
            if(unparsedTotal != 0) {
                errorMessagef(fmt::format("channel {} corrupted data removed {} byte{}",
                                          channelId,
                                          unparsedTotal,
                                          unparsedTotal == 1 ? "" : "s"));
            }
            return gotMessage;
        }

    private:
        // the frame at the head is stuck (incomplete or corrupt): drop it up to the next
        // start marker in one step; returns the bytes dropped
        std::size_t dropHeadFrame() {
            auto const next
              = std::find_if(std::next(buffer.begin()), buffer.end(), [this](std::byte b) {
                    return remote_fmt::isFrameStart(b, frameCheck);
                });
            auto const dropped = static_cast<std::size_t>(std::distance(buffer.begin(), next));
            buffer.erase(buffer.begin(), next);
            return dropped;
        }

        // a start marker behind the head's
        bool hasLaterFrameStart() const {
            return buffer.size() > 1
                && std::find_if(std::next(buffer.begin()), buffer.end(), [this](std::byte b) {
                       return remote_fmt::isFrameStart(b, frameCheck);
                   }) != buffer.end();
        }
    };

}}   // namespace uc_log::detail
