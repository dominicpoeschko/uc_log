#pragma once

#include "remote_fmt/parser.hpp"

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
        using Clock = std::chrono::steady_clock;

        // a corrupted stream must resync in bulk (not one byte per timeout) and the buffer
        // must never grow without bound when the end marker never arrives
        static constexpr std::size_t MaxBufferSize = 1024 * 1024;
        static constexpr auto        FrameTimeout  = std::chrono::milliseconds{100};

        std::vector<std::byte> buffer;
        Clock::time_point      lastValidRead{Clock::now()};

        void append(std::span<std::byte const> data) {
            buffer.insert(buffer.end(), data.begin(), data.end());
        }

        // drain all complete frames, one printF call per decoded message. haltedRecently
        // gates the timeout-based resync (a halted target legitimately pauses mid-frame),
        // the size cap does not. Returns true when at least one message was decoded.
        template<typename StopRequestedF,
                 typename PrintF,
                 typename ErrorMessageF>
        bool drain(StopRequestedF&&                       stopRequested,
                   PrintF&&                               printF,
                   std::size_t                            channelId,
                   std::unordered_map<std::uint16_t,
                                      std::string> const& stringConstantsMap,
                   ErrorMessageF&&                        errorMessagef,
                   bool                                   haltedRecently) {
            if(buffer.empty()) {
                lastValidRead = Clock::now();
                return false;
            }

            bool                       gotMessage{};
            std::size_t                unparsedTotal{};
            std::span<std::byte const> remaining{buffer};
            while(!stopRequested()) {
                auto const [output_stream, subrange, unparsed_bytes]
                  = remote_fmt::parse(remaining, stringConstantsMap, errorMessagef);
                remaining = subrange;
                unparsedTotal += unparsed_bytes;
                if(output_stream) {
                    lastValidRead = Clock::now();
                    gotMessage    = true;
                    printF(channelId, *output_stream);
                    continue;
                }
                break;
            }
            // single compaction per drain instead of one front-erase per message
            buffer.erase(buffer.begin(),
                         std::next(buffer.begin(),
                                   static_cast<std::make_signed_t<std::size_t>>(
                                     buffer.size() - remaining.size())));
            if(unparsedTotal != 0) {
                errorMessagef(fmt::format("channel {} corrupted data removed {} byte{}",
                                          channelId,
                                          unparsedTotal,
                                          unparsedTotal == 1 ? "" : "s"));
            }

            if(!buffer.empty()) {
                bool const oversized = buffer.size() > MaxBufferSize;
                bool const timedOut
                  = !haltedRecently && Clock::now() > lastValidRead + FrameTimeout;
                if(oversized || timedOut) { resyncToNextFrame(channelId, errorMessagef); }
            }
            return gotMessage;
        }

    private:
        // the frame at the head is stuck (incomplete or corrupt): drop it up to the next
        // start marker in one step
        template<typename ErrorMessageF>
        void resyncToNextFrame(std::size_t     channelId,
                               ErrorMessageF&& errorMessagef) {
            auto const next    = std::find(std::next(buffer.begin()),
                                           buffer.end(),
                                           remote_fmt::protocol::Start_marker);
            auto const dropped = static_cast<std::size_t>(std::distance(buffer.begin(), next));
            buffer.erase(buffer.begin(), next);
            lastValidRead = Clock::now();
            errorMessagef(fmt::format("channel {} resync: dropped {} byte{} of stuck frame",
                                      channelId,
                                      dropped,
                                      dropped == 1 ? "" : "s"));
        }
    };

}}   // namespace uc_log::detail
