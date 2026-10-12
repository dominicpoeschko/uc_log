// libFuzzer: the printer's frame extraction (detail/RttChannel.hpp + remote_fmt's parser) over an arbitrary byte
// stream arriving in arbitrary pieces - what a torn, duplicated, truncated or stale RTT read hands it. Every message
// it decodes is a well-formed string; it never crashes, never hangs (libFuzzer's -timeout), keeps its buffer within
// MaxBufferSize, and consumes everything once the stream has ended and the stuck head is dropped.
//
// Input: byte 0 seeds the piece sizes, the rest is the stream. Half the inputs are prefixed with a run of valid
// frames of the stress shapes (uc_log/stress/Records.hpp) so the fuzzer mutates real frames, not only noise.
#include "remote_fmt/remote_fmt.hpp"
#include "uc_log/detail/RttChannel.hpp"
#include "uc_log/stress/Records.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace sc::literals;

namespace {
struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

std::unordered_map<std::uint16_t,
                   std::string> const&
catalog() {
    static auto const& c = *new std::unordered_map<std::uint16_t, std::string>{
      {1, "cataloged {}"},
      {2,            "x"}
    };
    return c;
}

std::vector<std::byte> const& validFrames() {
    static auto const& frames = *new std::vector<std::byte>{[] {
        std::vector<std::byte> out;
        for(std::uint32_t s = 0; s < 24; ++s) {
            remote_fmt::Printer<VectorBackend> printer{};
            unsigned const                     p = s % uc_log::stress::NumProducers;
            std::uint32_t const                b = 0;
            auto const                         v = uc_log::stress::values(p, b, s);
            switch(s % uc_log::stress::NumShapes) {
#define LS_SERIALIZE(shape, fmtString, ...)                        \
    case shape: printer.print(fmtString##_sc, __VA_ARGS__); break;
                UC_LOG_STRESS_SHAPES(LS_SERIALIZE)
#undef LS_SERIALIZE
            default:
                printer.print("LS|{}|{}|{}|m|{}"_sc, p, b, s, uc_log::stress::metricValue(v));
                break;
            }
            auto const& m = printer.get_com_backend().memory;
            out.insert(out.end(), m.begin(), m.end());
        }
        return out;
    }()};
    return frames;
}
}   // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size);

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
    if(size == 0) { return 0; }
    std::uint32_t          seed = data[0];
    std::vector<std::byte> stream;
    if((seed & 1U) != 0U) {
        auto const& v = validFrames();
        stream.insert(stream.end(), v.begin(), v.end());
    }
    std::string_view const input{reinterpret_cast<char const*>(data), size};
    for(char const c : input.substr(1)) { stream.push_back(static_cast<std::byte>(c)); }

    uc_log::detail::RttChannel channel;
    std::size_t                pos  = 0;
    auto                       next = [&] {
        seed = seed * 1'103'515'245U + 12'345U;
        return seed >> 16U;
    };
    auto drain = [&](bool halted) {
        channel.drain(
          [](std::size_t, std::string const& m, auto) {
              // a decoded message is a string the .rttlog and the socket can carry
              if(m.size() > (1U << 20U)) { std::abort(); }
          },
          0,
          catalog(),
          [](std::string_view) {},
          halted);
        if(channel.buffer.size() > uc_log::detail::RttChannel::MaxBufferSize + 4096U) {
            std::abort();
        }
    };
    while(pos < stream.size()) {
        std::size_t const piece = std::min<std::size_t>(1 + next() % 300, stream.size() - pos);
        channel.append(std::span{stream}.subspan(pos, piece));
        pos += piece;
        drain((next() & 7U) != 0);
    }
    // the stream has ended: a timed-out resync must eat every stuck frame start, never loop
    channel.lastValidRead = uc_log::detail::RttChannel::Clock::now() - std::chrono::seconds{1};
    drain(false);
    if(channel.buffer.size() > 1U
       && std::find_if(std::next(channel.buffer.begin()),
                       channel.buffer.end(),
                       remote_fmt::protocol::isStartMarker)
            != channel.buffer.end())
    {
        std::abort();   // a frame start behind the head survived the resync
    }
    return 0;
}
