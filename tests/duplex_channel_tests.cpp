// The duplex channels of the RTT backend (detail/RttBackendCore.hpp DuplexChannel<I>): host -> target bytes through
// read(), in order across the down buffer's wrap and into a buffer smaller than what is there; target -> host bytes
// through write() on the channel's own up buffer, a full buffer giving back the remainder (trim mode), nothing written
// while no debugger is attached; and FanOutBackend handing on the primary's channels. The test plays the probe: it
// finds the control block in its own writable memory by the SEGGER id, as a J-Link finds it in the target's RAM, and
// moves the offsets of the buffers it owns (the down buffers' write offset, the up buffers' read offset). Built -m32:
// the control block's layout needs 4-byte pointers.
#include "rtt/rtt.hpp"
#include "support/Check.hpp"
#include "uc_log/AlwaysAttached.hpp"
#include "uc_log/FanOutBackend.hpp"
#include "uc_log/RttLog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <link.h>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef __clang__
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {

static_assert(sizeof(void*) == 4,
              "the RTT control block's layout needs 4-byte pointers: build -m32");

struct NeverAttached {
    [[maybe_unused]] bool operator()() const { return false; }
};

// two channels: "shell" 32 up / 16 down, "raw" 16 up / 8 down, both trim mode (the default)
using Duplex = uc_log::RttLog<uc_log::AlwaysAttached,
                              uc_log::Rings<64>,
                              uc_log::DuplexChannels<uc_log::DuplexChannelConfig<"shell", 32, 16>,
                                                     uc_log::DuplexChannelConfig<"raw", 16, 8>>>;
using Detached
  = uc_log::RttLog<NeverAttached,
                   uc_log::Rings<64>,
                   uc_log::DuplexChannels<uc_log::DuplexChannelConfig<"dshell", 32, 16>>>;

using Shell = Duplex::DuplexChannel<0>;
using Raw   = Duplex::DuplexChannel<1>;

static_assert(Duplex::numDuplexChannels == 2 && Shell::name == "shell" && Raw::name == "raw");

struct OneCore {
    static constexpr std::size_t cores = 1;

    [[maybe_unused]] static uc_log::Context context() { return uc_log::Context::thread; }

    [[maybe_unused]] static std::size_t core() { return 0; }
};

struct Other {
    [[maybe_unused]] static void write(std::span<std::byte const>) {}
};

using Fan = uc_log::FanOutBackendWith<OneCore, Duplex, Other>;
static_assert(Fan::numDuplexChannels == 2
                && std::is_same_v<Fan::DuplexChannel<0>,
                                  Shell>
                && std::is_same_v<Fan::DuplexChannel<1>,
                                  Raw>,
              "the fan-out hands on the primary's channels");

// The SEGGER layout as the probe reads it: the id, the buffer counts, then the up and the down buffers' blocks.
struct Header {
    std::array<char, 16> id;
    std::uint32_t        numUp;
    std::uint32_t        numDown;
};

rtt::detail::BufferControlBlock* blocksOf(Header* h) {
    return reinterpret_cast<rtt::detail::BufferControlBlock*>(reinterpret_cast<std::byte*>(h)
                                                              + sizeof(Header));
}

struct Found {
    std::vector<Header*> blocks;
};

// every 16-byte SEGGER id in a writable segment of this executable
int scanSegments(dl_phdr_info* info,
                 std::size_t,
                 void* data) {
    if(info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }   // the executable only
    auto& found = *static_cast<Found*>(data);
    for(std::size_t i = 0; i < info->dlpi_phnum; ++i) {
        auto const& ph = info->dlpi_phdr[i];
        if(ph.p_type != PT_LOAD || (ph.p_flags & PF_W) == 0) { continue; }
        auto* const       begin = reinterpret_cast<std::byte*>(info->dlpi_addr + ph.p_vaddr);
        std::size_t const size  = ph.p_memsz;
        for(std::size_t off = 0; off + sizeof(Header) <= size; off += 4) {
            if(std::memcmp(begin + off,
                           rtt::defaultControlBlockId.data(),
                           rtt::defaultControlBlockId.size())
               == 0)
            {
                found.blocks.push_back(reinterpret_cast<Header*>(begin + off));
            }
        }
    }
    return 0;
}

// The duplex channels' up buffers come after the log rings: one up and one down buffer per channel
std::size_t firstDuplexUp(Header const* h) { return h->numUp - h->numDown; }

// The control block whose first duplex up buffer is named `name`
Header* block(std::string_view name) {
    Found found{};
    dl_iterate_phdr(scanSegments, &found);
    Header* hit = nullptr;
    int     n   = 0;
    for(auto* h : found.blocks) {
        auto const& first = blocksOf(h)[firstDuplexUp(h)];
        if(h->numUp > h->numDown && h->numDown > 0 && first.name != nullptr
           && std::string_view{first.name} == name)
        {
            hit = h;
            ++n;
        }
    }
    CHECKF(n == 1,
           "one control block with duplex channel 0 '%.*s', found %d",
           static_cast<int>(name.size()),
           name.data(),
           n);
    return hit;
}

// up buffer i of the duplex channels (0: the first channel), and log ring i
rtt::detail::BufferControlBlock& up(Header*     h,
                                    std::size_t i) {
    return blocksOf(h)[firstDuplexUp(h) + i];
}

rtt::detail::BufferControlBlock& logRing(Header*     h,
                                         std::size_t i) {
    return blocksOf(h)[i];
}

rtt::detail::BufferControlBlock& down(Header*     h,
                                      std::size_t i) {
    return blocksOf(h)[h->numUp + i];
}

// the probe writes into a down buffer: what fits (one slot stays free), the write offset published after the bytes
std::size_t hostWrite(rtt::detail::BufferControlBlock& b,
                      std::string_view                 text) {
    std::uint32_t const rd
      = std::atomic_ref<std::uint32_t>{b.readPosition}.load(std::memory_order_acquire);
    std::uint32_t     wr   = b.writePosition;
    std::size_t const free = (rd + b.bufferSize - wr - 1U) % b.bufferSize;
    std::size_t const n    = std::min(free, text.size());
    for(std::size_t i = 0; i < n; ++i) {
        b.buffer[wr] = static_cast<std::byte>(text[i]);
        wr           = (wr + 1U) % b.bufferSize;
    }
    std::atomic_ref<std::uint32_t>{b.writePosition}.store(wr, std::memory_order_release);
    return n;
}

// the probe reads an up buffer: everything published, the read offset given back
std::string hostRead(rtt::detail::BufferControlBlock& b) {
    std::uint32_t const wr
      = std::atomic_ref<std::uint32_t>{b.writePosition}.load(std::memory_order_acquire);
    std::uint32_t rd = b.readPosition;
    std::string   s;
    while(rd != wr) {
        s.push_back(static_cast<char>(b.buffer[rd]));
        rd = (rd + 1U) % b.bufferSize;
    }
    std::atomic_ref<std::uint32_t>{b.readPosition}.store(rd, std::memory_order_release);
    return s;
}

template<typename Channel>
std::string targetRead(std::size_t bufferSize) {
    std::vector<std::byte> buf(bufferSize);
    auto const             got = Channel::read(std::span{buf});
    std::string            s;
    for(auto const c : got) { s.push_back(static_cast<char>(c)); }
    return s;
}

template<typename Channel>
std::string targetWrite(std::string_view text) {
    auto const  rest = Channel::write(std::as_bytes(std::span{text}));
    std::string s;
    for(auto const c : rest) { s.push_back(static_cast<char>(c)); }
    return s;
}

void layout() {
    Header* const h = block("shell");
    if(h == nullptr) { return; }
    CHECK(h->numUp == 4 && h->numDown == 2,
          "the two log rings of Rings<64> + two duplex up buffers, two down buffers");
    CHECK(up(h, 0).bufferSize == 32 && up(h, 1).bufferSize == 16, "up sizes 32 and 16");
    CHECK(std::string_view{up(h, 1).name} == "raw",
          "the second channel's up buffer carries its name");
    CHECK(down(h, 0).bufferSize == 16 && down(h, 1).bufferSize == 8, "down sizes 16 and 8");
    CHECK(std::string_view{down(h, 0).name} == "shell"
            && std::string_view{down(h, 1).name} == "raw",
          "the down buffers carry the channel names");
}

void hostToTarget() {
    Header* const h = block("shell");
    if(h == nullptr) { return; }
    CHECK(targetRead<Shell>(32).empty(), "nothing sent: read() returns nothing");
    CHECK(hostWrite(down(h, 0), "hello") == 5, "the probe writes 5 bytes");
    CHECK(targetRead<Raw>(32).empty(), "the other channel's read sees none of them");
    CHECK(targetRead<Shell>(32) == "hello", "read() returns them");
    CHECK(targetRead<Shell>(32).empty(), "and only once");
}

void hostToTargetWrapAndShortBuffer() {
    Header* const h = block("shell");
    if(h == nullptr) { return; }
    // move the offsets near the end, so the next text wraps
    CHECK(hostWrite(down(h, 0), "0123456789") == 10, "10 bytes");
    CHECK(targetRead<Shell>(32) == "0123456789", "10 back");
    CHECK(hostWrite(down(h, 0), "abcdefghijkl") == 12, "12 bytes across the wrap");
    std::string got;
    for(int i = 0; i < 4; ++i) { got += targetRead<Shell>(32); }
    CHECK(got == "abcdefghijkl", "in order across the wrap");
    CHECK(hostWrite(down(h, 0), "ABCDEFGHIJ") == 10, "10 bytes");
    CHECK(targetRead<Shell>(4) == "ABCD", "a 4-byte buffer takes 4");
    CHECK(targetRead<Shell>(4) == "EFGH", "the next 4");
    CHECK(targetRead<Shell>(4) == "IJ", "the rest");
    CHECK(hostWrite(down(h, 0), std::string(20, 'x')) == 15,
          "a full down buffer takes 15 of 16 (one slot free)");
    CHECK(targetRead<Shell>(32).size() == 15, "and gives 15");
}

void targetToHost() {
    Header* const h = block("shell");
    if(h == nullptr) { return; }
    CHECK(targetWrite<Shell>("abc").empty(), "write() takes 3 bytes, no remainder");
    CHECK(targetWrite<Raw>("xy").empty(), "the other channel");
    CHECK(hostRead(up(h, 0)) == "abc", "the shell's up buffer has them");
    CHECK(hostRead(up(h, 1)) == "xy", "the raw channel's its own");
    CHECK(hostRead(logRing(h, 0)).empty() && hostRead(logRing(h, 1)).empty(),
          "nothing in the log rings");
    std::string const forty(40, 'q');
    auto const        rest = targetWrite<Shell>(forty);
    CHECK(rest.size() == 9, "a full up buffer (31 of 32 usable): the remainder comes back");
    CHECK(hostRead(up(h, 0)) == std::string(31, 'q'), "what fitted");
    CHECK(targetWrite<Shell>(rest).empty(), "after the host read, the rest goes in");
    CHECK(hostRead(up(h, 0)) == std::string(9, 'q'), "and arrives");
}

void detachedWritesNothing() {
    Header* const h = block("dshell");
    if(h == nullptr) { return; }
    CHECK(targetWrite<Detached::DuplexChannel<0>>("abc") == "abc",
          "no debugger: write() gives everything back");
    CHECK(hostRead(up(h, 0)).empty(), "and nothing is in the up buffer");
    CHECK(hostWrite(down(h, 0), "in") == 2, "the probe can still write");
    CHECK(targetRead<Detached::DuplexChannel<0>>(8) == "in",
          "read() does not depend on the debugger");
}

void throughTheFanOut() {
    Header* const h = block("shell");
    if(h == nullptr) { return; }
    CHECK(targetWrite<Fan::DuplexChannel<0>>("fan").empty(), "write through the fan-out");
    CHECK(hostRead(up(h, 0)) == "fan", "lands in the primary's up buffer");
    CHECK(hostWrite(down(h, 1), "rx") == 2, "the probe writes the raw channel");
    CHECK(targetRead<Fan::DuplexChannel<1>>(8) == "rx", "read through the fan-out");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {                        "layout",                         layout},
                               {                  "hostToTarget",                   hostToTarget},
                               {"hostToTargetWrapAndShortBuffer", hostToTargetWrapAndShortBuffer},
                               {                  "targetToHost",                   targetToHost},
                               {         "detachedWritesNothing",          detachedWritesNothing},
                               {              "throughTheFanOut",               throughTheFanOut},
    });
}
