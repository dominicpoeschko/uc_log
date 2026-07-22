// Unit test for uc_log::detail::RttChannel: frame decode, partial frames, garbage
// resync (timeout + size cap), halt gating, single-compaction drain.
#include "remote_fmt/type_identifier.hpp"
#include "uc_log/detail/RttChannel.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using uc_log::detail::RttChannel;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

static constexpr std::byte Start{0x55};
static constexpr std::byte End{0xAA};

// one cataloged frame with no arguments: marker, type byte, catalog id, end marker
static std::vector<std::byte> makeFrame(std::uint8_t catalogId) {
    auto const typeByte = remote_fmt::detail::fmtStringTypeIdentifier<
      remote_fmt::detail::FmtStringType::cataloged_normal>(remote_fmt::detail::RangeSize::_1);
    return {Start, typeByte, std::byte{catalogId}, End};
}

struct Sink {
    std::vector<std::string> messages;
    std::vector<std::string> errors;

    auto printF() {
        return [this](std::size_t, std::string_view msg) { messages.emplace_back(msg); };
    }

    auto errorF() {
        return [this](std::string_view msg) { errors.emplace_back(msg); };
    }
};

static bool drain(RttChannel& ch,
                  Sink&       sink,
                  bool        haltedRecently = false) {
    std::unordered_map<std::uint16_t, std::string> const catalog{
      {0, "hello"},
      {1, "world"},
    };
    return ch
      .drain([]() { return false; }, sink.printF(), 0, catalog, sink.errorF(), haltedRecently);
}

int main() {
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
        ch.append(std::vector<std::byte>{End});
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
        // valid start, plausible type byte, but the end marker never arrives and no 0xAA
        // anywhere so the parser keeps waiting
        std::vector<std::byte> stuck{
          Start,
          remote_fmt::detail::fmtStringTypeIdentifier<
            remote_fmt::detail::FmtStringType::cataloged_normal>(remote_fmt::detail::RangeSize::_1),
          std::byte{0},
        };
        stuck.insert(stuck.end(), 100, std::byte{0x00});
        ch.append(stuck);
        drain(ch, sink);
        CHECK(!ch.buffer.empty(), "incomplete frame retained before timeout");

        std::this_thread::sleep_for(RttChannel::FrameTimeout + 20ms);
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
        RttChannel             ch;
        Sink                   sink;
        std::vector<std::byte> huge{
          Start,
          remote_fmt::detail::fmtStringTypeIdentifier<
            remote_fmt::detail::FmtStringType::cataloged_normal>(remote_fmt::detail::RangeSize::_1),
          std::byte{0},
        };
        huge.insert(huge.end(), RttChannel::MaxBufferSize + 1000, std::byte{0x00});
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

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
