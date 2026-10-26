#pragma once

// The bookkeeping of LossPolicy::Counted (LossPolicy.hpp): what each log ring dropped, and the
// state of a panic's wait for room. Pure, so the host tests run it against a real rtt ring.
//
// A record is many write() calls. Each one is staged (rtt's Buffer::stage: written past the
// published offset, invisible to the host) and the record is published at its end - or, when one
// piece did not fit, the whole record is dropped: the rest of it is not even tried (admits() says
// no) and its staged pieces are never published. A ring never holds a frame with a hole, and the
// count is of records, not pieces. (Without the staging, skip mode left holes: a piece that did not
// fit after pieces that did, or a later piece that did fit after the host had read.)

#include <array>
#include <cstddef>
#include <cstdint>

/// A free-running microsecond counter, if the chip package has one - the one Kvasir::AnnouncedReset times its waits
/// with (Kvasir_SDK Runtime/AnnouncedReset.hpp; chip_rp_common: the TIMER's raw low word, TIMERAWL). Not defined: null,
/// and a panic's wait counts polls.
// Also declared in Kvasir_SDK Runtime/AnnouncedReset.hpp; the guard avoids gcc's -Wredundant-decls.
#ifndef KVASIR_ANNOUNCED_RESET_TIME_US_DECLARED
    #define KVASIR_ANNOUNCED_RESET_TIME_US_DECLARED
extern "C" [[gnu::weak]] std::uint32_t kvasir_announced_reset_time_us();
#endif

namespace uc_log::detail {

// Per log ring, written only by that ring's producer (IsrPolicy.hpp): plain loads and stores, no
// atomics - fine on the Cortex-M0+. NMI and HardFault may cut into a record on the first ISR ring:
// its staged bytes are lost, and a count may be off by one in a fault. 16 bytes; the printer reads
// the first two as little-endian words.
struct RingLoss {
    std::uint32_t lost;       // records dropped since boot (settle() takes back its own)
    std::uint32_t reported;   // how many of them a lost-record has announced
    std::uint32_t staged;     // the write offset of the record being written, not published yet
    std::uint8_t  dropping;   // a piece of the current record was dropped: drop the rest of it too
    std::uint8_t  reserved[3];
};

static_assert(sizeof(RingLoss) == 16,
              "the printer reads 16 bytes a ring");

template<std::size_t NumRings>
struct LossLedger {
    // the last slot: records of an ISR level that has no ring (a SilentLevels promise broken),
    // or of any ring index past the rings
    static constexpr std::size_t noRing = NumRings;

    std::array<RingLoss, NumRings + 1> rings;   // .bss: a boot starts at 0

    static constexpr std::size_t slot(std::size_t ring) { return ring < NumRings ? ring : noRing; }

    // a record starts at the ring's published write offset
    void recordBegin(std::size_t   ring,
                     std::uint32_t published) {
        rings[slot(ring)].staged = published;
    }

    // where the record's next piece goes
    std::uint32_t& staged(std::size_t ring) { return rings[slot(ring)].staged; }

    [[nodiscard]] bool admits(std::size_t ring) const { return rings[slot(ring)].dropping == 0; }

    void dropped(std::size_t ring) { rings[slot(ring)].dropping = 1; }

    // true: publish the staged record; false: it was dropped (and is counted)
    bool recordEnd(std::size_t ring) {
        auto& r = rings[slot(ring)];
        if(r.dropping != 0) {
            r.dropping = 0;
            r.lost     = r.lost + 1;
            return false;
        }
        return true;
    }

    struct Mark {
        std::size_t   ring;
        std::uint32_t lostBefore;
        std::uint32_t
          pending;   // dropped, not announced yet; 0 for the no-ring slot (nowhere to say it)
    };

    [[nodiscard]] Mark mark(std::size_t ring) const {
        auto const& r = rings[slot(ring)];
        return {ring, r.lost, slot(ring) == noRing ? 0U : r.lost - r.reported};
    }

    // After the lost-record went out, or did not: its own drop is not a lost line (taken back), and
    // then the count stays pending. The record it was written in front of is dropped as well (and
    // counted): a shorter record may fit where the lost-record did not, and would then arrive after
    // its gap with nothing in front of it - the announcement one line late.
    void settle(Mark const m) {
        auto& r = rings[slot(m.ring)];
        if(r.lost != m.lostBefore) {
            r.lost     = m.lostBefore;
            r.dropping = 1;
        } else {
            r.reported = r.reported + m.pending;
        }
    }
};

// A panic's wait for room (LossPolicy::Counted with panicStallPolls != 0).
template<std::size_t NumRings>
struct PanicWait {
    bool on;
    // per ring: the host's read offset + 1 when a wait last ran out, 0 while the host reads; a
    // later wait with the offset still there gives up at once (a fault dump of 20 lines with nobody
    // reading costs one stall, not twenty)
    std::array<std::uint32_t, NumRings> goneAtPlus1;
};

// The waiter rtt's writeWaiting (and a drain) asks: yes while the host's read offset moved within the stall time. Timed
// by the microsecond counter `now` while it runs: a poll costs what the loop around it costs (~100-160 cycles on an
// M0+ in a sanitize image), so a stall counted in polls at the fewest cycles a poll can take waited ~40 times too
// long - 10 s instead of 250 ms without a printer. `polls` only where there is no counter, or while it stands still (a
// tick not set up yet).
struct StallWaiter {
    std::uint32_t
      polls;   // the stall in polls at the fastest clock; 0: no wait (outside panic mode)
    std::uint32_t& goneAtPlus1;
    std::uint32_t  stallUs{};   // the stall by the counter (0: polls only)
    std::uint32_t (*now)(){};   // the counter; null: none
    std::uint32_t last{};
    std::uint32_t idle{};    // polls since the host last moved
    std::uint32_t since{};   // the counter when the host last moved
    bool          started{};
    bool          counterMoves{};

    bool operator()(std::uint32_t const host) {
        if(polls == 0) { return false; }   // not in panic mode
        if(!started) {
            started = true;
            if(goneAtPlus1 == host + 1U) { return false; }
            goneAtPlus1 = 0;
            hostMoved(host);
            return true;
        }
        if(host != last) {
            hostMoved(host);
            return true;
        }
        ++idle;
        if(stalled()) {
            goneAtPlus1 = host + 1U;
            return false;
        }
        return true;
    }

    void hostMoved(std::uint32_t host) {
        last  = host;
        idle  = 0;
        since = now != nullptr ? now() : 0U;
    }

    bool stalled() {
        if(now != nullptr && stallUs != 0) {
            auto const elapsed = now() - since;   // modulo 2^32: a wrap of the counter is fine
            counterMoves       = counterMoves || elapsed != 0;
            if(counterMoves) { return elapsed >= stallUs; }
        }
        return idle >= polls;   // no counter, or one that stands still (its tick not set up)
    }
};

struct NoLossState {};

}   // namespace uc_log::detail
