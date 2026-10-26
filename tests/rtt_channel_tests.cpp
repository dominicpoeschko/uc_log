// Unit test for uc_log::printer::RttChannel: frame decode, partial frames, garbage
// resync (timeout + size cap), halt gating, single-compaction drain.
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/wire.hpp"
#include "support/Check.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/RttChannel.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using uc_log::printer::RttChannel;

// fmt's locale and unicode settings must match the compiled fmt library (see uc_log's CMakeLists.txt)
static_assert(FMT_USE_LOCALE == 1,
              "FMT_USE_LOCALE must match the compiled fmt library");
static_assert(FMT_UNICODE == 1,
              "FMT_UNICODE must match the compiled fmt library");

static constexpr std::byte start = remote_fmt::protocol::startMarker;   // an unchecked frame
static constexpr std::byte end   = remote_fmt::protocol::endMarker;

namespace {

// one cataloged frame with no arguments: marker, the 2-byte catalog id, end marker
std::vector<std::byte> makeFrame(std::uint8_t catalogId) {
    return {start, std::byte{catalogId}, std::byte{0}, end};
}

// The start of a frame of catalog id 2 ("stuck {}" with one string argument) whose string says it is `length` bytes
// long: the parser waits for them.
std::vector<std::byte> stuckStart(std::uint8_t length) {
    return {start, std::byte{2}, std::byte{0}, std::byte{length}};
}

struct Sink {
    std::vector<std::string> messages;
    std::vector<std::string> errors;

    auto printF() UC_LOG_LIFETIMEBOUND {
        return [this](std::size_t, std::string_view msg, std::optional<remote_fmt::catalog_id>) {
            messages.emplace_back(msg);
        };
    }

    auto errorF() UC_LOG_LIFETIMEBOUND {
        return [this](std::string_view msg) { errors.emplace_back(msg); };
    }
};

bool drain(RttChannel& ch,
           Sink&       sink,
           bool        haltedRecently = false) {
    std::unordered_map<std::uint16_t, std::string> const catalog{
      {0,     "hello\x1e"},
      {1,     "world\x1e"},
      {2, "stuck {}\x1es"},
    };
    return ch.drain(sink.printF(), 0, catalog, sink.errorF(), haltedRecently);
}

}   // namespace

int main() {
    // the frame timeout on the virtual clock: it passes only where a test sleeps
    uc_log::printer::VirtualTime const virtualTime;
    // 0: a locale-specific spec formats (the decode path of a replacement field such as {:LF})
    CHECK(fmt::format(fmt::runtime("{:LF}"), 1.5) == "1.500000", "a {:L} spec formats");

    // 1: single frame decodes, buffer fully consumed
    {
        RttChannel ch;
        Sink       sink;
        ch.append(makeFrame(0));
        CHECK(drain(ch, sink), "frame decoded");
        CHECK(sink.messages.size() == 1 && sink.messages[0] == "hello", "message content");
        CHECK(ch.buffer.empty(), "buffer consumed");
    }

    // 2: two frames + partial third in one drain; completing the third decodes it
    {
        RttChannel ch;
        Sink       sink;
        auto       data = makeFrame(0);
        auto const f2   = makeFrame(1);
        data.insert(data.end(), f2.begin(), f2.end());
        auto const f3 = makeFrame(0);
        data.insert(data.end(), f3.begin(), f3.end() - 1);   // partial
        ch.append(data);
        drain(ch, sink);
        CHECK(sink.messages.size() == 2, "two complete frames decoded");
        CHECK(ch.buffer.size() == f3.size() - 1, "partial frame retained");
        ch.append(std::vector<std::byte>{end});
        drain(ch, sink);
        CHECK(sink.messages.size() == 3 && sink.messages[2] == "hello", "completed frame decoded");
    }

    // 3: byte-by-byte feeding
    {
        RttChannel ch;
        Sink       sink;
        for(auto const b : makeFrame(1)) {
            ch.append(std::vector<std::byte>{b});
            drain(ch, sink);
        }
        CHECK(sink.messages.size() == 1 && sink.messages[0] == "world", "byte-wise decode");
    }

    // 4: pure garbage without markers is discarded and reported
    {
        RttChannel ch;
        Sink       sink;
        ch.append(std::vector<std::byte>(1000, std::byte{0x11}));
        drain(ch, sink);
        CHECK(sink.messages.empty(), "no message from garbage");
        CHECK(ch.buffer.empty(), "garbage discarded");
        CHECK(!sink.errors.empty(), "garbage reported");
    }

    // 5: stuck frame resyncs after the timeout, but not while halted recently
    {
        RttChannel ch;
        Sink       sink;
        // a valid start and a known id, but the string's bytes never all arrive, so the parser keeps waiting
        auto stuck = stuckStart(120);
        stuck.insert(stuck.end(), 100, std::byte{0x00});
        ch.append(stuck);
        drain(ch, sink);
        CHECK(!ch.buffer.empty(), "incomplete frame retained before timeout");

        uc_log::printer::Clock::sleep_for(RttChannel::frameTimeout + 20ms);
        drain(ch, sink, true);   // halted: no resync
        CHECK(!ch.buffer.empty(), "halt gate blocks timeout resync");

        drain(ch, sink, false);
        CHECK(ch.buffer.empty(), "timeout resync dropped the stuck frame");
        auto valid = makeFrame(1);
        ch.append(valid);
        drain(ch, sink);
        CHECK(!sink.messages.empty() && sink.messages.back() == "world",
              "decodes normally after resync");
    }

    // 6: size cap resyncs immediately, even right after a halt
    {
        RttChannel ch;
        Sink       sink;
        // a string length of 2^32 - 1 (LEB128): longer than the cap
        std::vector<std::byte> huge{start,
                                    std::byte{2},
                                    std::byte{0},
                                    std::byte{0xFF},
                                    std::byte{0xFF},
                                    std::byte{0xFF},
                                    std::byte{0xFF},
                                    std::byte{0x0F}};
        huge.insert(huge.end(), RttChannel::maxBufferSize + 1000, std::byte{0x00});
        // valid frame after the garbage: the resync must land on its start marker
        auto const valid = makeFrame(0);
        huge.insert(huge.end(), valid.begin(), valid.end());
        ch.append(huge);
        drain(ch, sink, true);   // even a recent halt must not defer the cap
        drain(ch, sink, true);
        CHECK(sink.messages.size() == 1 && sink.messages[0] == "hello",
              "cap resync recovered the trailing valid frame");
        CHECK(ch.buffer.empty(), "buffer drained after cap resync");
    }

    // 7: a ring's worth of stale stuck frames (a target that restarted without a reset while the
    // DLL read) is dropped in one resync after one timeout, not one frame per timeout
    {
        RttChannel             ch;
        Sink                   sink;
        std::vector<std::byte> stale;
        for(int i = 0; i != 200; ++i) {
            auto const stuck = stuckStart(50);
            stale.insert(stale.end(), stuck.begin(), stuck.end());
            stale.insert(stale.end(), 8, std::byte{0x00});
        }
        auto const valid = makeFrame(1);
        stale.insert(stale.end(), valid.begin(), valid.end());
        ch.append(stale);
        drain(ch, sink);
        CHECK(sink.messages.empty(), "stale frames block before the timeout");
        uc_log::printer::Clock::sleep_for(RttChannel::frameTimeout + 20ms);
        drain(ch, sink);
        CHECK(sink.messages.size() == 1 && sink.messages[0] == "world",
              "one timeout resyncs through all stale frames to the valid one");
        CHECK(ch.buffer.empty(), "nothing left after the bulk resync");
        std::size_t resyncs{};
        for(auto const& e : sink.errors) {
            if(e.contains("resync")) { ++resyncs; }
        }
        CHECK(resyncs == 1, "one resync message for the whole stall");

        // the last frame start may still be arriving: kept for its own timeout
        RttChannel             tail;
        Sink                   tailSink;
        std::vector<std::byte> two{stale.begin(), std::next(stale.begin(), 24)};
        tail.append(two);
        drain(tail, tailSink);
        uc_log::printer::Clock::sleep_for(RttChannel::frameTimeout + 20ms);
        drain(tail, tailSink);
        CHECK(tail.buffer.size() == 12, "the last frame start is kept after a bulk resync");
    }

    // A line lost to its format and a damaged stream, told apart by the kind (remote_fmt::ParseError) - never by the
    // words: a time value fmt cannot take ("time value out of range", no "bad format" prefix) and a spec fmt refuses
    // are both format errors, each frame consumed whole; garbage before them is a stream error.
    {
        std::unordered_map<std::uint16_t, std::string> const catalog{
          {3,
           "t {:%H}\x1e"
           "d1/1f8"      },
          {4,
           "v {:Z}\x1e"
           "u4"          },
          {0, "hello\x1e"},
        };
        std::vector<std::byte> stream{std::byte{0x01},
                                      std::byte{0x02}};   // no frame start: skipped
        auto const             huge = std::bit_cast<std::array<std::byte, 8>>(1e300);
        stream.insert(stream.end(), {start, std::byte{3}, std::byte{0}});
        stream.insert(stream.end(), huge.begin(), huge.end());
        stream.push_back(end);
        stream.insert(stream.end(), {start, std::byte{4}, std::byte{0}, std::byte{5}, end});
        auto const hello = makeFrame(0);
        stream.insert(stream.end(), hello.begin(), hello.end());

        RttChannel                                                  ch;
        std::vector<std::string>                                    messages;
        std::vector<std::pair<remote_fmt::ParseError, std::string>> errors;
        ch.append(stream);
        ch.drain(
          [&](std::size_t, std::string_view msg, auto) { messages.emplace_back(msg); },
          0,
          catalog,
          [&](std::string_view m, remote_fmt::ParseError kind) { errors.emplace_back(kind, m); },
          false);
        CHECK(messages == std::vector<std::string>{"hello"} && ch.buffer.empty(),
              "both refused frames consumed, the next one decoded");
        std::size_t formats{};
        std::size_t streams{};
        for(auto const& [kind, text] : errors) {
            if(kind == remote_fmt::ParseError::format) {
                ++formats;
            } else {
                ++streams;
                CHECK(text.contains("corrupted data removed 2 bytes"), "the garbage, said once");
            }
        }
        CHECK(formats == 2 && streams == 1, "two format errors, one stream error");
        if(formats != 2 || streams != 1) {
            for(auto const& [kind, text] : errors) {
                std::printf("  %s: %s\n",
                            kind == remote_fmt::ParseError::format ? "format" : "stream",
                            text.c_str());
            }
        }
        CHECK(!errors.empty() && errors.front().first == remote_fmt::ParseError::format
                && errors.front().second.starts_with("time value out of range"),
              "the time value is a format error too, whatever its words");
    }

    return uc_log::test::report();
}
