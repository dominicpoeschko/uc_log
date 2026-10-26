#pragma once

// What the printer makes of LossPolicy::Counted (LossPolicy.hpp): the in-band lost-record a firmware
// writes in front of the next line that fits ("⚠ N log records lost on this channel"), and the ledger
// it keeps in RAM (`::lossLedger`), which the printer reads every few seconds - the one way to learn of
// a ring that went quiet after its loss. Pure: fed with log entries, raw bytes and times.

#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LostRecordLine.hpp"
#include "uc_log/printer/MapSymbols.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

// N, when `e` is the lost-record: a warn line of module uc_log written by RecordFrame<>::reportLost.
// By its site, not its text; the text only carries the number (LostRecordLine.hpp, either sign).
inline std::optional<std::uint64_t> lostRecordCount(LogEntry const& e) {
    if(!e.parsedOk || e.module != "uc_log" || e.logLevel != uc_log::LogLevel::warn) {
        return std::nullopt;
    }
    bool const site = e.functionName.contains("reportLost") || e.signature.contains("::reportLost");
    if(!site) { return std::nullopt; }
    return lostRecordNumber(e.logMsg);
}

// One ring of the ledger as the printer reads it: 16 bytes, `lost` and `reported` the first two
// little-endian words (LossLedger.hpp RingLoss).
struct RingLossView {
    std::uint32_t lost{};
    std::uint32_t reported{};
};

inline constexpr std::size_t ringLossBytes = 16;

// The ledger's rings, the no-ring slot last; empty for a size that is no whole number of slots.
inline std::vector<RingLossView> decodeLossLedger(std::span<std::byte const> raw) {
    std::vector<RingLossView> out;
    if(raw.empty() || raw.size() % ringLossBytes != 0) { return out; }
    for(std::size_t at = 0; at != raw.size(); at += ringLossBytes) {
        out.push_back({le32(raw, at), le32(raw, at + 4)});
    }
    return out;
}

// Where the ledger is, from the map. lld: the demangled symbol "...::lossLedger" with its size. GNU ld (its demangled
// names have spaces, and its symbol line shows no size): the input section ".bss._ZN...10lossLedgerE" with address
// and size. Several: nullopt and `count` says so.
struct LedgerPlace {
    std::uint32_t address{};
    std::uint32_t size{};
};

inline std::optional<LedgerPlace> findLossLedger(MapSymbols const& map,
                                                 std::size_t&      count) {
    static constexpr std::string_view demangled{"::lossLedger"};
    static constexpr std::string_view mangled{"10lossLedgerE"};
    auto const fits = [](std::uint32_t size) { return size != 0 && size % ringLossBytes == 0; };
    bool const lld  = map.fromLld();
    std::optional<LedgerPlace> found;
    count = 0;
    for(auto const& e : map.entries()) {
        bool const named
          = lld ? (!e.fromSection && e.name.ends_with(demangled))
                : (e.fromSection && e.name.starts_with(".bss") && e.name.contains(mangled));
        if(named && fits(e.size)) {
            ++count;
            found = LedgerPlace{e.address, e.size};
        }
    }
    if(count != 1) { return std::nullopt; }
    return found;
}

// Counts the lost-records per channel, and watches the ledger for a loss nobody announced.
class TargetLoss {
public:
    using Clock = uc_log::printer::Clock;

    // A ring may hold a pending count for a while: the next line on it reports it.
    static constexpr auto quietReportAfter = std::chrono::seconds{2};

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
    // quietReportAfter, and one per new loss in the no-ring slot).
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
            if(now - *w.since >= quietReportAfter && r.lost != w.toldLost) {
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

    [[nodiscard]] std::uint64_t total() const { return total_; }

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

}   // namespace uc_log::printer
