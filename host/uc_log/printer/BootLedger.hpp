#pragma once

// Boots nobody read, counted exactly. The image counts its boots that logged (detail/RttEpoch.hpp: uc_log's first
// record of a boot moves the count, 16 bits, in the word after the RTT epoch). The RTT epoch cannot be used for this:
// it moves at every start, also one that never logs (the J-Link DLL parks an RP2040 before main). Between two looks of the printer, the boots that logged and were not read are the counter's delta
// minus the boot it read. Exact while the image stays and RAM keeps the word; a power-on (RAM undefined) or a download
// over that RAM gives a delta that cannot be counted from - said as such.
//
// When a boot is settled:
// - start(): a session begins - the first look sets where counting starts (boots before the printer are not its);
// - bytesRead(): the session reads its boot's first bytes - every boot before it is settled, and it is the read one;
//   notRead() takes that back when the session ended without a line of it (a probe failure dropped the bytes);
// - restart(): the printer's own download wrote over the target's RAM (J-Link's flash loader runs there and does not
//   always put back what it found) - no count from before it compares with one after it: counting starts again;
// - replacing(): the printer's own flash or reset replaces the boot - every boot up to now is settled.
// The count read with a session's first bytes is that boot's own: its first record counted it before its bytes were
// in the ring. A second session on the same boot reads the same count and settles nothing.

#include <cstdint>
#include <optional>
#include <variant>

namespace uc_log::printer {

class BootLedger {
public:
    /// The count is 16 bits (it shares its word with the epoch's low half); a delta beyond this, or one that went back,
    /// is not counted.
    using Count                                      = std::uint16_t;
    static constexpr std::uint32_t maxCountableBoots = 30'000;

    /// `boots` boots that logged ran unread between counter values `from` and `to`; `replacing`: settled by the
    /// printer's own flash or reset.
    struct Unread {
        std::uint32_t boots;
        std::uint32_t from;
        std::uint32_t to;
        bool          replacing;
    };

    /// The counter moved from `from` to `to` by more than can be counted.
    struct Uncountable {
        std::uint32_t from;
        std::uint32_t to;
    };

    using Verdict = std::variant<std::monostate, Unread, Uncountable>;

    /// A session begins on the image whose count is at `address` and reads `logged` now. The count's place is the
    /// same in every image of a board (its own section, first in the RAM); a count found elsewhere than before (an
    /// image of another layout) cannot be compared with the one before: how many boots passed is said as unknown,
    /// never as 0.
    Verdict start(std::uint32_t address,
                  Count         logged) {
        if(settled_ && address_ == address) { return {}; }
        Verdict verdict;
        if(settled_) { verdict = Uncountable{*settled_, logged}; }
        address_ = address;
        settled_ = logged;
        return verdict;
    }

    /// The session's first bytes: the count reads `logged`, the read boot's own.
    Verdict bytesRead(Count logged) {
        if(!settled_) { return {}; }
        return settle(*settled_, static_cast<Count>(logged - 1U), false, logged);
    }

    /// The boot bytesRead(`logged`) settled as read left no line (its session ended on a probe failure, the bytes
    /// dropped): it counts as unread again, at the next settlement.
    void notRead(Count logged) {
        if(settled_ == logged) { settled_ = static_cast<Count>(logged - 1U); }
    }

    /// The printer's own download is done: counting starts again at the next session (replacing() settled what came
    /// before it).
    void restart() { settled_.reset(); }

    /// The printer's own flash or reset replaces the running boot; the count at `address` reads `logged`.
    Verdict replacing(std::uint32_t address,
                      Count         logged) {
        if(!settled_) { return {}; }
        if(
          address
          != address_) {   // the files already describe another image: this one's count is not there
            Verdict const verdict = Uncountable{*settled_, logged};
            settled_.reset();
            return verdict;
        }
        return settle(*settled_, logged, true, logged);
    }

private:
    // every boot in (from, upTo] that logged is unread (from: settled_); settled_ goes to `next`
    Verdict settle(Count from,
                   Count upTo,
                   bool  isReplacing,
                   Count next) {
        auto const delta
          = static_cast<std::int16_t>(static_cast<Count>(upTo - from));   // modulo 2^16
        Verdict verdict;
        if(delta > static_cast<std::int32_t>(maxCountableBoots) || delta < -1) {
            verdict = Uncountable{from, upTo};
        } else if(delta > 0) {
            verdict = Unread{static_cast<std::uint32_t>(delta), from, upTo, isReplacing};
        }
        // -1: the boot read is the one settled already - nothing to settle
        if(delta >= 0 || delta < -1) { settled_ = next; }
        return verdict;
    }

    std::optional<std::uint32_t> address_;
    std::optional<Count>         settled_;   // every boot up to this count is accounted for
};

}   // namespace uc_log::printer
