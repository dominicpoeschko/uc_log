#pragma once

// What the printer makes of LossPolicy::Counted (LossPolicy.hpp): the in-band lost-record a firmware
// writes in front of the next line that fits ("⚠ N log records lost on this channel"), and the ledger
// it keeps in RAM (`::lossLedger`), which the printer reads every few seconds - the one way to learn of
// a ring that went quiet after its loss. Pure: fed with log entries, raw bytes and times.

#include "LogEntry.hpp"
#include "uc_log/detail/Le32.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <istream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::detail {

// N, when `e` is the lost-record: a warn line of module uc_log written by RecordFrame<>::reportLost.
// By its site, not its text; the text only carries the number.
inline std::optional<std::uint64_t> lostRecordCount(LogEntry const& e) {
    if(!e.parsedOk || e.module != "uc_log" || e.logLevel != uc_log::LogLevel::warn) {
        return std::nullopt;
    }
    bool const site = e.functionName.contains("reportLost") || e.signature.contains("::reportLost");
    if(!site) { return std::nullopt; }
    std::string_view                  msg{e.logMsg};
    static constexpr std::string_view Warning{"⚠ "};
    if(!msg.starts_with(Warning)) { return std::nullopt; }
    msg.remove_prefix(Warning.size());
    std::uint64_t n{};
    auto const [ptr, ec] = std::from_chars(msg.begin(), msg.end(), n);
    if(ec != std::errc{} || ptr == msg.begin()) { return std::nullopt; }
    return n;
}

// One ring of the ledger as the printer reads it: 16 bytes, `lost` and `reported` the first two
// little-endian words (LossLedger.hpp RingLoss).
struct RingLossView {
    std::uint32_t lost{};
    std::uint32_t reported{};
};

inline constexpr std::size_t RingLossBytes = 16;

// The ledger's rings, the no-ring slot last; empty for a size that is no whole number of slots.
inline std::vector<RingLossView> decodeLossLedger(std::span<std::byte const> raw) {
    std::vector<RingLossView> out;
    if(raw.empty() || raw.size() % RingLossBytes != 0) { return out; }
    for(std::size_t at = 0; at != raw.size(); at += RingLossBytes) {
        out.push_back({le32(raw, at), le32(raw, at + 4)});
    }
    return out;
}

// Where the ledger is, from the map. lld: "<vma> <lma> <size> <align> ...::lossLedger" (demangled). GNU ld (its
// demangled names have spaces, so the symbol line alone says nothing about the size): the input-section line
// ".bss._ZN...10lossLedgerE", address and size on it or on the next line. Several: nullopt and `count` says so.
struct LedgerPlace {
    std::uint32_t address{};
    std::uint32_t size{};
};

inline std::optional<LedgerPlace> findLossLedger(std::istream& map,
                                                 std::size_t&  count) {
    static constexpr std::string_view Demangled{"::lossLedger"};
    static constexpr std::string_view Mangled{"10lossLedgerE"};
    auto const                        hexes = [](std::string_view text, std::size_t n) {
        std::vector<std::uint32_t> out;
        while(out.size() != n) {
            auto const first = text.find_first_not_of(' ');
            if(first == std::string_view::npos) { break; }
            text.remove_prefix(first);
            if(text.starts_with("0x")) { text.remove_prefix(2); }
            std::uint32_t v{};
            auto const [ptr, ec] = std::from_chars(text.begin(), text.end(), v, 16);
            if(ec != std::errc{} || ptr == text.begin()) { break; }
            out.push_back(v);
            text.remove_prefix(static_cast<std::size_t>(ptr - text.begin()));
        }
        return out;
    };
    auto const fits = [](std::uint32_t size) { return size != 0 && size % RingLossBytes == 0; };
    std::optional<LedgerPlace> found;
    count = 0;
    std::string line;
    bool        pending = false;   // a GNU section line without its numbers
    while(std::getline(map, line)) {
        std::string_view const text{line};
        if(pending) {
            pending       = false;
            auto const nr = hexes(text, 2);
            if(nr.size() == 2 && fits(nr[1])) {
                ++count;
                found = LedgerPlace{nr[0], nr[1]};
            }
            continue;
        }
        if(text.ends_with(Demangled)) {
            auto const nr = hexes(text, 3);   // lld: vma, lma, size
            if(nr.size() == 3 && fits(nr[2])) {
                ++count;
                found = LedgerPlace{nr[0], nr[2]};
            }
            continue;
        }
        auto const at = text.find(Mangled);
        if(at != std::string_view::npos && text.find_first_not_of(' ') != std::string_view::npos
           && text.substr(text.find_first_not_of(' ')).starts_with(".bss"))
        {
            auto const nr = hexes(text.substr(at + Mangled.size()), 2);
            if(nr.size() == 2) {
                if(fits(nr[1])) {
                    ++count;
                    found = LedgerPlace{nr[0], nr[1]};
                }
            } else {
                pending = true;
            }
        }
    }
    if(count != 1) { return std::nullopt; }
    return found;
}

// Counts the lost-records per channel, and watches the ledger for a loss nobody announced.
class TargetLoss {
public:
    using Clock = std::chrono::steady_clock;

    // A ring may hold a pending count for a while: the next line on it reports it.
    static constexpr auto QuietReportAfter = std::chrono::seconds{2};

    // the status message for a lost-record on `channel`
    std::string lostRecord(std::size_t              channel,
                           std::uint64_t            n,
                           std::chrono::nanoseconds before) {
        perChannel_[channel] += n;
        total_ += n;
        return fmt::format(
          "channel {}: the target dropped {} log record{} before {:.3f} s (ring full, or "
          "no debugger); {} on this channel since the printer started",
          channel,
          n,
          n == 1 ? "" : "s",
          std::chrono::duration<double>(before).count(),
          perChannel_[channel]);
    }

    // A ledger read: the messages it gives (one per ring whose count stayed unannounced for
    // QuietReportAfter, and one per new loss in the no-ring slot).
    std::vector<std::string> ledger(std::span<RingLossView const> rings,
                                    Clock::time_point             now) {
        std::vector<std::string> out;
        if(rings.empty()) { return out; }
        watched_.resize(rings.size());
        auto const noRing = rings.size() - 1;
        for(std::size_t i = 0; i != rings.size(); ++i) {
            auto&      w = watched_[i];
            auto const r = rings[i];
            if(i == noRing) {
                if(r.lost > w.toldNoRing) {
                    out.push_back(
                      fmt::format("{} log record{} from an interrupt level that has no RTT ring (a "
                                  "SilentLevels "
                                  "promise broken): dropped, counted in the ledger",
                                  r.lost - w.toldNoRing,
                                  r.lost - w.toldNoRing == 1 ? "" : "s"));
                    w.toldNoRing = r.lost;
                }
                continue;
            }
            std::uint32_t const pending = r.lost - r.reported;
            if(pending == 0 || r.lost < w.toldLost) {   // reported, or the target rebooted
                w = {};
                continue;
            }
            if(!w.since || w.pending != pending) {
                w.since   = now;
                w.pending = pending;
                continue;
            }
            if(now - *w.since >= QuietReportAfter && r.lost != w.toldLost) {
                out.push_back(
                  fmt::format("ring {}: {} log record{} dropped, not yet reported in-band - the "
                              "ring has written nothing since",
                              i,
                              pending,
                              pending == 1 ? "" : "s"));
                w.toldLost = r.lost;
            }
        }
        return out;
    }

    std::uint64_t total() const { return total_; }

    void clearWatch() { watched_.clear(); }

private:
    struct Watch {
        std::optional<Clock::time_point> since;
        std::uint32_t                    pending{};
        std::uint32_t                    toldLost{};
        std::uint32_t                    toldNoRing{};
    };

    std::map<std::size_t, std::uint64_t> perChannel_;
    std::uint64_t                        total_{};
    std::vector<Watch>                   watched_;
};

}   // namespace uc_log::detail
