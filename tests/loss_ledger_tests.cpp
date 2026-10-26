// LossPolicy::Counted's bookkeeping (detail/LossLedger.hpp) against a real rtt ring and a host
// stand-in for the reader: records of several pieces written the way a backend writes them, frames
// parsed back, and the accounting identity checked. Also the case it exists for: in skip mode a
// record longer than one write can lose a MIDDLE piece - piece 1 does not fit, the host reads,
// piece 2 does - and the ring then holds a frame with a hole. Built -m32 (rtt needs 4-byte
// pointers).
#include "rtt/rtt.hpp"
#include "support/Check.hpp"
#include "uc_log/ChannelRouter.hpp"
#include "uc_log/LossPolicy.hpp"
#include "uc_log/RttLog.hpp"
#include "uc_log/detail/LevelBoundBackend.hpp"
#include "uc_log/detail/LossLedger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __clang__
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {

struct Name {
    constexpr operator std::string_view() const { return "uc_log0"; }
};

constexpr std::size_t ringSize = 64;
using Buffer                   = rtt::detail::Buffer<rtt::BufferMode::skip, ringSize, Name>;
using Ledger                   = uc_log::detail::LossLedger<1>;
// the CPU clock ceiling the policies below are given (LossPolicy::Counted has no default)
constexpr std::uint64_t cpuHz = 200'000'000;

// A frame as remote_fmt's is shaped: start marker, a kind, a length, the payload, end marker.
constexpr std::byte start{0x58};
constexpr std::byte end{0xAA};
constexpr std::byte dataKind{'D'};
constexpr std::byte lostKind{'L'};

struct Ring {
    std::unique_ptr<std::byte[]>    storage = std::make_unique<std::byte[]>(ringSize);
    rtt::detail::BufferControlBlock block   = Buffer::make(storage.get());
};

std::vector<std::byte> frame(std::byte     kind,
                             std::uint32_t value,
                             std::size_t   payload) {
    std::vector<std::byte> f{start, kind, std::byte{static_cast<std::uint8_t>(payload)}};
    for(std::size_t i = 0; i != payload; ++i) {
        f.push_back(std::byte{static_cast<std::uint8_t>(value >> (8U * (i % 4U)))});
    }
    f.push_back(end);
    return f;
}

// What a reader makes of the bytes: whole frames, and the bytes it had to throw away.
struct Parsed {
    std::vector<std::uint32_t> data;   // the value of each whole data frame
    std::vector<std::uint32_t> lost;   // the count of each whole lost-record
    std::size_t                discarded{};
};

struct Reader {
    std::vector<std::byte> pending;
    Parsed                 parsed;

    void take(Ring& ring) {
        std::array<std::byte, ringSize> out{};
        auto const                      got = Buffer::read(ring.block, out);
        pending.insert(pending.end(), got.begin(), got.end());
        parse();
    }

    void parse() {
        std::size_t i = 0;
        while(i < pending.size()) {
            if(pending[i] != start) {
                ++parsed.discarded;
                ++i;
                continue;
            }
            if(i + 3 > pending.size()) { break; }
            auto const n = std::to_integer<std::size_t>(pending[i + 2]);
            if(i + 3 + n + 1 > pending.size()) { break; }
            if(pending[i + 3 + n] != end
               || (pending[i + 1] != dataKind && pending[i + 1] != lostKind))
            {
                ++parsed.discarded;
                ++i;
                continue;
            }
            std::uint32_t v{};
            for(std::size_t k = 0; k != std::min<std::size_t>(n, 4); ++k) {
                v |= std::to_integer<std::uint32_t>(pending[i + 3 + k]) << (8U * k);
            }
            (pending[i + 1] == dataKind ? parsed.data : parsed.lost).push_back(v);
            i += 3 + n + 1;
        }
        pending.erase(pending.begin(), std::next(pending.begin(), static_cast<std::ptrdiff_t>(i)));
    }
};

// A record as the backend writes it: in pieces of at most `piece` bytes (the Printer's staging).
// `between` runs between two pieces (the host reading at that moment).
template<typename Between>
void writePiecesToday(Ring&                         ring,
                      std::vector<std::byte> const& f,
                      std::size_t                   piece,
                      Between&&                     between) {
    for(std::size_t at = 0; at < f.size(); at += piece) {
        auto const n = std::min(piece, f.size() - at);
        static_cast<void>(Buffer::write(ring.block, std::span{f}.subspan(at, n)));
        between();
    }
}

template<typename Between>
void writePiecesCounted(Ring&                         ring,
                        Ledger&                       ledger,
                        bool                          present,
                        std::vector<std::byte> const& f,
                        std::size_t                   piece,
                        Between&&                     between) {
    ledger.recordBegin(0, ring.block.writePosition);
    for(std::size_t at = 0; at < f.size(); at += piece) {
        auto const n = std::min(piece, f.size() - at);
        if(ledger.admits(0)) {
            if(!present
               || !Buffer::stage(ring.block, std::span{f}.subspan(at, n), ledger.staged(0)))
            {
                ledger.dropped(0);
            }
        }
        between();
    }
    if(ledger.recordEnd(0)) { Buffer::publish(ring.block, ledger.staged(0)); }
}

// RecordFrame's order: the lost-record first if one is pending, settled, then the record.
template<typename Between>
void recordCounted(Ring&         ring,
                   Ledger&       ledger,
                   bool          present,
                   std::uint32_t value,
                   std::size_t   payload,
                   std::size_t   piece,
                   Between&&     between) {
    if(auto const m = ledger.mark(0); m.pending != 0) {
        writePiecesCounted(ring, ledger, present, frame(lostKind, m.pending, 4), piece, between);
        ledger.settle(m);
    }
    writePiecesCounted(ring, ledger, present, frame(dataKind, value, payload), piece, between);
}

void middlePieceHole() {
    // 50 bytes unread: 13 free. A 30-byte record in pieces of 12: piece 1 fits (1 left), piece 2
    // does not, the host reads everything, piece 3 fits - after a frame start whose middle is gone.
    {
        Ring   ring;
        Reader reader;
        auto   filler = frame(dataKind, 1, 46);   // 50 bytes
        static_cast<void>(Buffer::write(ring.block, filler));
        int pieces = 0;
        writePiecesToday(ring, frame(dataKind, 2, 26), 12, [&] {
            if(++pieces == 2) { reader.take(ring); }
        });
        // the reader is inside a frame that claims 26 more bytes: later records complete it
        for(std::uint32_t v = 3; v != 6; ++v) {
            writePiecesToday(ring, frame(dataKind, v, 4), 12, [] {});
            reader.take(ring);
        }
        CHECK(reader.parsed.discarded > 0,
              "today: a dropped middle piece leaves a frame with a hole");
        CHECK(reader.parsed.data.size() < 5,
              "today: a later record is swallowed by the broken frame");
        std::printf(
          "today's skip mode: %zu bytes discarded by the reader after a dropped middle piece\n",
          reader.parsed.discarded);
    }
    {
        Ring   ring;
        Reader reader;
        Ledger ledger{};
        static_cast<void>(Buffer::write(ring.block, frame(dataKind, 1, 46)));
        int pieces = 0;
        recordCounted(ring, ledger, true, 2, 26, 12, [&] {
            if(++pieces == 2) { reader.take(ring); }
        });
        reader.take(ring);
        CHECK(reader.parsed.discarded == 0, "counted: the rest of a dropped record is dropped too");
        CHECK(ledger.rings[0].lost == 1, "counted: one record lost");
        recordCounted(ring, ledger, true, 3, 4, 12, [] {});
        reader.take(ring);
        CHECK(reader.parsed.lost.size() == 1 && reader.parsed.lost[0] == 1,
              "the next record is preceded by a lost-record with the count");
        CHECK(reader.parsed.data.size() == 2 && reader.parsed.data.back() == 3, "and then arrives");
        CHECK(ledger.mark(0).pending == 0, "nothing pending after the report");
    }
}

void lostRecordThatDoesNotFit() {
    Ring   ring;
    Reader reader;
    Ledger ledger{};
    // fill the ring, lose two records, still no room: the lost-record itself is dropped
    static_cast<void>(Buffer::write(ring.block, frame(dataKind, 1, 58)));
    recordCounted(ring, ledger, true, 2, 8, 12, [] {});
    recordCounted(ring, ledger, true, 3, 8, 12, [] {});
    // the second call tried a lost-record first: dropped, and taken back by settle()
    CHECK(ledger.rings[0].lost == 2, "two records lost, the lost-record's own drop not among them");
    CHECK(ledger.mark(0).pending == 2,
          "the lost-record's own drop is not counted, both still pending");
    reader.take(ring);
    recordCounted(ring, ledger, true, 4, 4, 12, [] {});
    reader.take(ring);
    CHECK(reader.parsed.lost.size() == 1 && reader.parsed.lost[0] == 2, "reported once, with 2");
    CHECK(reader.parsed.discarded == 0, "only whole frames");
}

void shortRecordAfterAFailedLostRecord() {
    Ring   ring;
    Reader reader;
    Ledger ledger{};
    // 7 bytes free: a 12-byte record is lost; the next record is 4 bytes, its lost-record 8
    static_cast<void>(Buffer::write(ring.block, frame(dataKind, 1, 52)));
    recordCounted(ring, ledger, true, 2, 8, 12, [] {});
    recordCounted(ring, ledger, true, 3, 0, 12, [] {});
    CHECK(ledger.rings[0].lost == 2,
          "the record behind a lost-record that did not fit is dropped too");
    reader.take(ring);
    recordCounted(ring, ledger, true, 4, 4, 12, [] {});
    reader.take(ring);
    CHECK(reader.parsed.data.size() == 2 && reader.parsed.data[0] == 1
            && reader.parsed.data[1] == 4,
          "no record arrives between its gap and the lost-record");
    CHECK(reader.parsed.lost.size() == 1 && reader.parsed.lost[0] == 2,
          "the gap is announced with 2");
    CHECK(reader.parsed.discarded == 0, "only whole frames");
}

void debuggerAbsent() {
    Ring   ring;
    Ledger ledger{};
    for(std::uint32_t i = 0; i != 5; ++i) {
        recordCounted(ring, ledger, false, i, 4, 12, [] {});
    }
    CHECK(ledger.rings[0].lost == 5, "records refused by DebuggerPresentFunction are counted");
    Reader reader;
    recordCounted(ring, ledger, true, 9, 4, 12, [] {});
    reader.take(ring);
    CHECK(reader.parsed.lost.size() == 1 && reader.parsed.lost[0] == 5,
          "and reported when it says yes");
}

void noRingSlot() {
    uc_log::detail::LossLedger<2> ledger{};
    constexpr auto                noLogRing = ~std::size_t{0};
    ledger.dropped(noLogRing);
    ledger.recordEnd(noLogRing);
    ledger.dropped(7);
    ledger.recordEnd(7);
    CHECK(ledger.rings[2].lost == 2,
          "a level with no ring and an index past the rings count in the last slot");
    CHECK(ledger.mark(noLogRing).pending == 0, "nothing to report in-band for it");
    CHECK(ledger.rings[0].lost == 0 && ledger.rings[1].lost == 0, "the rings are untouched");
}

// Random record sizes and a random reader: per ring, decoded + announced + pending == issued.
void model() {
    Ring          ring;
    Reader        reader;
    Ledger        ledger{};
    std::mt19937  random{42};
    std::uint32_t issued = 0;
    for(int i = 0; i != 100'000; ++i) {
        auto const payload = 4 + random() % 40;   // at least the 4 bytes of the value
        bool const present = random() % 50 != 0;
        recordCounted(ring, ledger, present, issued, payload, 12, [&] {
            if(random() % 6 == 0) { reader.take(ring); }
        });
        ++issued;
    }
    reader.take(ring);
    std::uint64_t announced = 0;
    for(auto n : reader.parsed.lost) { announced += n; }
    CHECK(reader.parsed.discarded == 0, "model: only whole frames");
    CHECK(reader.parsed.data.size() + announced + ledger.mark(0).pending == issued,
          "model: decoded + announced + pending == issued");
    bool ordered = std::ranges::is_sorted(reader.parsed.data);
    CHECK(ordered, "model: data frames in order");
    std::printf("model: %u issued, %zu decoded, %llu announced lost, %u pending\n",
                issued,
                reader.parsed.data.size(),
                static_cast<unsigned long long>(announced),
                ledger.mark(0).pending);
}

// Every place a record can meet the ring's end: each start offset of the 64-byte ring x each frame size 4..67 (over
// the ring: never fits) x each piece size x three reader habits (never during a record / between every piece / after
// each record). The lost-record crossing the wrap comes with it: after a drop, the next record's lost-record starts
// wherever the ring stands. Invariants every time: only whole frames, decoded + announced + pending == issued, in
// order, and a record that fits a drained ring is never lost while the reader keeps up.
void wrapSweep() {
    std::size_t cases            = 0;
    std::size_t bad              = 0;
    std::size_t lostWhileDrained = 0;
    for(std::uint32_t origin = 0; origin != ringSize; ++origin) {
        for(std::size_t payload = 0; payload != 64; ++payload) {
            for(std::size_t const piece :
                {std::size_t{1}, std::size_t{5}, std::size_t{12}, std::size_t{32}})
            {
                for(int habit = 0; habit != 3; ++habit) {
                    Ring   ring;
                    Reader reader;
                    Ledger ledger{};
                    ring.block.writePosition = origin;
                    ring.block.readPosition  = origin;
                    std::uint32_t issued     = 0;
                    for(; issued != 6; ++issued) {
                        recordCounted(ring, ledger, true, issued, payload, piece, [&] {
                            if(habit == 1) { reader.take(ring); }
                        });
                        if(habit != 0) { reader.take(ring); }
                    }
                    reader.take(ring);
                    // the last pending loss goes out in front of one more record that fits
                    if(ledger.mark(0).pending != 0) {
                        recordCounted(ring, ledger, true, issued, 4, piece, [] {});
                        ++issued;
                        reader.take(ring);
                    }
                    std::uint64_t announced = 0;
                    for(auto n : reader.parsed.lost) { announced += n; }
                    ++cases;
                    bool const ok
                      = reader.parsed.discarded == 0
                     && reader.parsed.data.size() + announced + ledger.mark(0).pending == issued
                     && (payload < 4 || std::ranges::is_sorted(reader.parsed.data));
                    if(!ok && bad++ < 3) {
                        std::printf(
                          "  wrap: start %u payload %zu piece %zu habit %d: %zu decoded, %llu "
                          "announced, "
                          "%u pending, %zu discarded of %u issued, sorted %d, data",
                          origin,
                          payload,
                          piece,
                          habit,
                          reader.parsed.data.size(),
                          static_cast<unsigned long long>(announced),
                          ledger.mark(0).pending,
                          reader.parsed.discarded,
                          issued,
                          std::ranges::is_sorted(reader.parsed.data) ? 1 : 0);
                        for(auto v : reader.parsed.data) { std::printf(" %u", v); }
                        std::printf("\n");
                    }
                    // a frame of payload + 4 bytes fits a drained 64-byte ring (63 usable) when <= 63
                    if(habit == 2 && payload + 4 <= ringSize - 1 && announced != 0) {
                        ++lostWhileDrained;
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "wrap sweep: every case whole, counted and in order");
    CHECK(lostWhileDrained == 0, "wrap sweep: a record that fits a drained ring is never lost");
    std::printf("wrap sweep: %zu cases\n", cases);
}

// writeWaiting with the panic's waiter: waits while the reader moves, gives up `polls` after it
// stops, then the latch ends the next wait at once until the host reads again. The host reads at a
// pace counted in the writer's polls (one read every hostEvery polls), not as a thread that may or
// may not get the CPU: every outcome below is the same on every run.
void panicWait() {
    Ring                         ring;
    uc_log::detail::PanicWait<1> wait{};
    constexpr std::uint32_t      polls = 200'000;
    constexpr std::uint32_t      hostEvery
      = 50;   // well inside `polls`: a reading host never stalls the writer
    std::size_t readBytes = 0;
    auto const  hostRead  = [&] {
        std::array<std::byte, 8> out{};
        readBytes += Buffer::read(ring.block, out).size();
    };

    // the waiter as the writer asks it, with the host's reads between its polls
    struct HostPaced {
        uc_log::detail::StallWaiter& waiter;
        decltype(hostRead) const&    read;
        bool                         reading;
        std::uint32_t                asked{};

        bool operator()(std::uint32_t host) {
            ++asked;
            if(reading && asked % hostEvery == 0) { read(); }
            return waiter(host);
        }
    };

    static_cast<void>(Buffer::write(ring.block, frame(dataKind, 1, 58)));   // full
    auto const big = frame(dataKind, 2, 40);
    // one staged record that waits with the panic's waiter: whole or nothing
    auto const writeWaiting = [&](HostPaced& waiter) {
        std::uint32_t staged = ring.block.writePosition;
        bool const    ok     = Buffer::stage(ring.block, big, staged, waiter);
        if(ok) { Buffer::publish(ring.block, staged); }
        return ok;
    };
    {
        uc_log::detail::StallWaiter waiter{polls, wait.goneAtPlus1[0]};
        HostPaced                   paced{waiter, hostRead, true};
        CHECK(writeWaiting(paced), "waits while the host reads, then lands whole");
        CHECK(wait.goneAtPlus1[0] == 0, "no latch");
    }
    while(Buffer::unread(ring.block) < ringSize - 2) {   // the host has stopped reading
        static_cast<void>(Buffer::write(ring.block, std::array<std::byte, 1>{std::byte{0}}));
    }
    {
        uc_log::detail::StallWaiter waiter{polls, wait.goneAtPlus1[0]};
        HostPaced                   paced{waiter, hostRead, false};
        CHECK(!writeWaiting(paced), "gives up when the host stops reading");
        CHECK(paced.asked > polls, "after the stall's polls");
        CHECK(wait.goneAtPlus1[0] != 0, "and latches");
    }
    {
        uc_log::detail::StallWaiter waiter{polls, wait.goneAtPlus1[0]};
        HostPaced                   paced{waiter, hostRead, false};
        CHECK(!writeWaiting(paced) && paced.asked == 1, "the latch ends the next wait at once");
    }
    hostRead();   // the host reads again
    {
        uc_log::detail::StallWaiter waiter{polls, wait.goneAtPlus1[0]};
        HostPaced                   paced{waiter, hostRead, true};
        CHECK(writeWaiting(paced),
              "the host read again: the latch is gone, the write waits and lands");
    }
    while(Buffer::unread(ring.block) != 0) { hostRead(); }
    CHECK(Buffer::unread(ring.block) == 0, "unread() reaches 0 once the host drained the ring");
    CHECK(readBytes != 0, "the host read");
}

// The panic's wait is timed by the chip's microsecond counter, not by polls: however long a poll takes, a host that does
// not read ends the wait after the stall time of counter time (it waited ~40x too long on an M0+ when polls were
// counted at 4 cycles each). A counter that stands still falls back to the polls.
std::uint32_t fakeUs   = 0;
std::uint32_t fakeStep = 0;   // microseconds one poll costs

std::uint32_t fakeClock() { return fakeUs += fakeStep; }

std::uint32_t pollsUntilGiveUp(std::uint32_t polls,
                               std::uint32_t stallUs,
                               std::uint32_t (*now)()) {
    std::uint32_t               gone = 0;
    uc_log::detail::StallWaiter waiter{polls, gone, stallUs, now};
    std::uint32_t               count = 0;
    while(waiter(7) && count != 100'000'000) { ++count; }   // the host's read offset never moves
    return count;
}

void timedStall() {
    constexpr std::uint32_t stallUs = 250'000;
    constexpr std::uint32_t pollLimit
      = 12'500'000;   // LossPolicy::Counted<CpuHz>'s: 250 ms at 200 MHz, 4 cycles a poll
    for(std::uint32_t const step : {1U, 10U, 1000U}) {
        fakeUs            = 0xFFFF'0000U;   // across the counter's wrap
        fakeStep          = step;
        auto const polls  = pollsUntilGiveUp(pollLimit, stallUs, &fakeClock);
        auto const waited = polls * step;
        CHECK(waited >= stallUs - step && waited <= stallUs + 2 * step,
              "the stall ends after its time on the counter, however long a poll takes");
    }
    fakeStep         = 0;   // a counter that stands still (its tick not set up)
    auto const polls = pollsUntilGiveUp(pollLimit, stallUs, &fakeClock);
    CHECK(polls == pollLimit, "a counter that stands still: the polls decide");
    CHECK(pollsUntilGiveUp(1000, stallUs, nullptr) == 1000, "no counter: the polls decide");
    CHECK(pollsUntilGiveUp(0, stallUs, &fakeClock) == 0, "no wait outside panic mode");
}

void unreadEdges() {
    Ring ring;
    CHECK(Buffer::unread(ring.block) == 0, "empty");
    static_cast<void>(Buffer::write(ring.block, frame(dataKind, 1, 20)));
    CHECK(Buffer::unread(ring.block) == 24, "24 written");
    std::array<std::byte, 10> out{};
    static_cast<void>(Buffer::read(ring.block, out));
    CHECK(Buffer::unread(ring.block) == 14, "10 read");
    ring.block.readPosition = ringSize + 3;   // a corrupt host offset
    CHECK(Buffer::unread(ring.block) == 0, "a host offset out of range: nothing to wait for");
}

// What a backend offers per policy (the three backends declare the same members; one is enough on the host:
// two backends' .noInit storage in one host image is a gcc section type conflict): Silent nothing new (no lossMark, so RecordFrame and LevelBoundBackend make
// no code), Counted the record hooks, the panic wait only with a stall time.
template<typename B>
concept HasLossHooks = requires { B::lossMark(); } && requires { B::initTransfer(); };
template<typename B>
concept HasPanicWait = requires { B::enterPanicMode(); } && requires { B::drain(); };

template<typename Loss>
using Default = uc_log::RttLog<uc_log::AlwaysAttached, uc_log::Rings<64>, Loss>;
static_assert(!HasLossHooks<Default<uc_log::LossPolicy::Silent>>);
static_assert(!HasPanicWait<Default<uc_log::LossPolicy::Silent>>);
static_assert(HasLossHooks<Default<uc_log::LossPolicy::Counted<cpuHz>>>);
static_assert(HasPanicWait<Default<uc_log::LossPolicy::Counted<cpuHz>>>);
static_assert(HasLossHooks<Default<uc_log::LossPolicy::Counted<cpuHz,
                                                               0>>>);
static_assert(!HasPanicWait<Default<uc_log::LossPolicy::Counted<cpuHz,
                                                                0>>>);

// RttLog: the options in any order, the missing ones defaulted (Silent, skip, SingleLevel, no duplex, one core and
// channel), and Rings repeated per group - all naming the one canonical RttComBackend
struct CoreOne {
    [[maybe_unused]] std::uint32_t operator()() const { return 1; }
};

using Canonical
  = uc_log::RttComBackend<uc_log::AlwaysAttached,
                          uc_log::RingLayout<uc_log::SingleCore, uc_log::SingleChannelRouter>,
                          uc_log::ChannelSizes<64, 64>,
                          rtt::BufferMode::skip,
                          uc_log::DuplexChannels<>,
                          uc_log::IsrPolicy::SingleLevel<>,
                          uc_log::LossPolicy::Silent>;
static_assert(std::is_same_v<uc_log::RttLog<uc_log::AlwaysAttached,
                                            uc_log::Rings<64>>,
                             Canonical>);
static_assert(std::is_same_v<uc_log::RttLog<uc_log::AlwaysAttached,
                                            uc_log::ChannelSizes<64,
                                                                 64>>,
                             Canonical>);
static_assert(std::is_same_v<uc_log::RttLog<uc_log::AlwaysAttached,
                                            uc_log::LossPolicy::Silent,
                                            uc_log::Mode<rtt::BufferMode::skip>,
                                            uc_log::Rings<64>,
                                            uc_log::IsrPolicy::SingleLevel<>>,
                             Canonical>);
static_assert(std::is_same_v<uc_log::RttLog<uc_log::AlwaysAttached,
                                            uc_log::LossPolicy::Counted<cpuHz>,
                                            uc_log::IsrPolicy::MaskedRecord,
                                            uc_log::Rings<1024,
                                                          256>,
                                            uc_log::RingLayout<uc_log::TwoCores<CoreOne>>>,
                             uc_log::RttComBackend<uc_log::AlwaysAttached,
                                                   uc_log::RingLayout<uc_log::TwoCores<CoreOne>>,
                                                   uc_log::ChannelSizes<1024,
                                                                        256,
                                                                        1024,
                                                                        256>,
                                                   rtt::BufferMode::skip,
                                                   uc_log::DuplexChannels<>,
                                                   uc_log::IsrPolicy::MaskedRecord,
                                                   uc_log::LossPolicy::Counted<cpuHz>>>,
              "Rings repeated per core");
// two cores x two channels: Rings fills four groups (the backend checks the count)
using Split2 = uc_log::RttLog<
  uc_log::AlwaysAttached,
  uc_log::Rings<16, 8>,
  uc_log::RingLayout<uc_log::TwoCores<CoreOne>, uc_log::LevelSplitRouter<uc_log::LogLevel::warn>>>;
static_assert(sizeof(Split2) > 0 && Split2::perCoreIsrContexts);
// RecordFrame asks LevelBoundBackend
template<typename B>
concept ForwardsLossMark = requires { B::lossMark(); };
static_assert(
  !ForwardsLossMark<uc_log::detail::LevelBoundBackend<Default<uc_log::LossPolicy::Silent>,
                                                      uc_log::LogLevel::info>>);
static_assert(
  ForwardsLossMark<uc_log::detail::LevelBoundBackend<Default<uc_log::LossPolicy::Counted<cpuHz>>,
                                                     uc_log::LogLevel::info>>);

static_assert(uc_log::LossPolicy::Policy<uc_log::LossPolicy::Silent>);
static_assert(uc_log::LossPolicy::Policy<uc_log::LossPolicy::Counted<cpuHz>>);
static_assert(!uc_log::LossPolicy::Policy<int>);
static_assert(uc_log::LossPolicy::Counted<cpuHz,
                                          0>::panicStallPolls
              == 0);
static_assert(uc_log::LossPolicy::Counted<cpuHz>::panicStallPolls == 12'500'000);
// the ceiling counts: a 48 MHz part waits a quarter of the polls, a 266 MHz one a third more
static_assert(uc_log::LossPolicy::Counted<48'000'000>::panicStallPolls == 3'000'000);
static_assert(uc_log::LossPolicy::Counted<266'000'000>::panicStallPolls == 16'625'000);
static_assert(uc_log::LossPolicy::Counted<cpuHz>::cpuHzCeiling == cpuHz);
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(
      argc,
      argv,
      {
        {                  "middlePieceHole",                   middlePieceHole},
        {         "lostRecordThatDoesNotFit",          lostRecordThatDoesNotFit},
        {"shortRecordAfterAFailedLostRecord", shortRecordAfterAFailedLostRecord},
        {                   "debuggerAbsent",                    debuggerAbsent},
        {                       "noRingSlot",                        noRingSlot},
        {                            "model",                             model},
        {                        "wrapSweep",                         wrapSweep},
        {                        "panicWait",                         panicWait},
        {                       "timedStall",                        timedStall},
        {                      "unreadEdges",                       unreadEdges},
    });
}
