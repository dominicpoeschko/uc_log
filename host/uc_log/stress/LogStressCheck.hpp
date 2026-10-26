#pragma once

// The host half of uc_log's stress stream (uc_log/stress/Records.hpp): reads what the printer wrote - the .rttlog rows
// and its status messages - and proves, per producer and boot, that every record arrived exactly once with exactly the
// text the firmware meant, or lies inside a loss the target announced on that channel.
//
// What it calls an anomaly (each one counted and the first few described):
//   wrong text      a record whose text differs from the text its (producer, boot, number) gives
//   duplicate       a number that arrived before, in the same boot
//   reorder         a number below the producer's last one, in the same boot
//   stale boot      a record of a boot older than one already seen (old ring contents read again)
//   unannounced     on a channel, more records missing than lost-records had announced up to that point
//   unparsed        a row that starts like a stress record but is not one (a torn or merged frame)
//   accounting      at a TOTAL line: issued != received + lost, per channel, for a quiesced boot
//   boot gap        a BOOT line skipped a boot number
//   printer         a status message that means bytes were lost or misread on the way (resync, corrupted
//                   data, host overflow, bad format, ...); the target's own loss announcements are not
//
// Feed it rows in the order the printer wrote them (one .rttlog after the other) and the status lines.

#include "uc_log/printer/LostRecordLine.hpp"
#include "uc_log/stress/Records.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

template<>
struct fmt::formatter<uc_log::stress::Color> : fmt::formatter<std::string_view> {
    auto format(uc_log::stress::Color c,
                format_context&       ctx) const {
        return fmt::formatter<std::string_view>::format(
          uc_log::stress::colorNames[static_cast<std::size_t>(c)],
          ctx);
    }
};

namespace uc_log::stress {

// The text the stress firmware logs for this record.
inline std::string expectedText(unsigned      p,
                                std::uint32_t b,
                                std::uint32_t s) {
    auto const v = values(p, b, s);
    switch(s % numShapes) {
#define LS_EXPECT(shape, fmtString, ...)                    \
    case shape: return fmt::format(fmtString, __VA_ARGS__);
        UC_LOG_STRESS_SHAPES(LS_EXPECT)
#undef LS_EXPECT
    case 10: return fmt::format("LS|{}|{}|{}|m|@METRIC(ls::value[u]={})", p, b, s, metricValue(v));
    default: return {};
    }
}

struct Row {
    unsigned         channel;
    std::string_view message;
    std::string_view module;
    std::string_view function{};   // empty: not checked (a synthetic row)
    std::string_view level{};
};

inline constexpr std::array<std::string_view, 6> levelNames{"trace",
                                                            "debug",
                                                            "info",
                                                            "warn",
                                                            "error",
                                                            "crit"};

class Checker {
public:
    enum class Kind : std::size_t {
        wrongText,
        duplicate,
        reorder,
        staleBoot,
        unannounced,
        unparsed,
        accounting,
        bootGap,
        printer,
        count
    };

    // every kind's name: a switch over the enum, so a new kind without a name does not compile (-Wswitch)
    static constexpr std::string_view kindName(Kind k) {
        switch(k) {
        case Kind::wrongText:   return "wrong text";
        case Kind::duplicate:   return "duplicate";
        case Kind::reorder:     return "reorder";
        case Kind::staleBoot:   return "stale boot";
        case Kind::unannounced: return "unannounced loss";
        case Kind::unparsed:    return "unparsed";
        case Kind::accounting:  return "accounting";
        case Kind::bootGap:     return "boot gap";
        case Kind::printer:     return "printer";
        case Kind::count:       break;
        }
        return {};
    }

    std::size_t maxExamples = 20;

    // the levels variant (Records.hpp levelOf): the image's compile-time floor; shapes below it were never logged
    std::optional<unsigned> floor;

    // Records of the numbers in (from, to) that the image logs at all
    [[nodiscard]] std::uint64_t logged(std::uint32_t from,
                                       std::uint32_t to) const {
        if(to <= from + 1) { return 0; }
        if(!floor) { return to - from - 1; }
        auto const upTo = [this](std::uint64_t n) {   // numbers 0..n-1 that are logged
            std::uint64_t count = (n / numShapes) * shapesLogged();
            for(std::uint64_t k = 0; k < n % numShapes; ++k) {
                count += levelOf(static_cast<unsigned>(k)) >= *floor;
            }
            return count;
        };
        return upTo(to) - upTo(std::uint64_t{from} + 1);
    }

    // called for every anomaly as it is found (the follow mode prints it at once)
    std::function<void(std::string_view)> onAnomaly;

    // called after each quiesced boot's accounting (the follow mode reports progress there)
    std::function<void()> onQuiesced;

    void row(Row const& r) {
        ++rows_;
        if(r.module == "uc_log") {
            // the printer's own row: boots that logged and were replaced unread (printer/BootLedger.hpp)
            if(auto const n = unreadBootCount(r.message)) {
                pendingUnreadBoots_ += *n;
                return;
            }
            // the printer's own download wrote over the target's RAM: the next boot number continues nothing
            if(r.message.contains("the boot count starts again")) {
                lastBootLine_.reset();
                pendingUnreadBoots_ = 0;
                return;
            }
            if(auto const n = printer::lostRecordNumber(r.message)) {
                // it belongs to the next record on this channel, whatever boot that is
                pendingAnnounced_[r.channel] += *n;
                announcedTotal_ += *n;
            }
            return;
        }
        if(!r.message.starts_with("LS|")) { return; }
        auto const fields = split(r.message);
        if(fields.size() >= 3 && fields[1] == "BOOT") {
            boot(fields);
            return;
        }
        if(fields.size() >= 5 && fields[1] == "TOTAL") {
            total(fields);
            return;
        }
        if(fields.size() >= 3 && fields[1] == "QUIET") { return; }
        if(fields.size() >= 3 && fields[1] == "PANIC") {
            return;
        }   // the panic ending's own line (128: KVASIR_PANIC)
        if(fields.size() >= 4 && fields[1] == "SYNC") {
            // the record each ring writes after its producers stopped: it carries that ring's last announcement
            if(number<std::uint32_t>(fields[3])) {
                channels_[{epoch_, r.channel}].announced += pendingAnnounced_[r.channel];
                pendingAnnounced_[r.channel] = 0;
            }
            return;
        }
        record(r, fields);
    }

    // a status message of the printer: [error] lines that mean bytes went missing or wrong on the way; a
    // [tool error] (the J-Link DLL's) is no anomaly by itself but is counted and the first ones kept
    void status(std::string_view line) {
        if(line.contains("[tool error]")) {
            if(toolErrors_++ < 5) { toolErrorExamples_.emplace_back(line); }
            return;
        }
        static constexpr std::array<std::string_view, 9> bad{"resync",
                                                             "corrupted data",
                                                             "host overflow",
                                                             "bad format",
                                                             "unparsed",
                                                             "malformed",
                                                             "not a valid",
                                                             "argument not found",
                                                             "OUT OF RANGE"};
        for(auto const b : bad) {
            if(line.contains(b)) {
                anomaly(Kind::printer, std::string{line});
                return;
            }
        }
    }

    // the producers' tails: a quiesced boot's TOTAL lines settle them; any other boot's last records may have gone
    // with the reset that ended it
    void finish() {}

    [[nodiscard]] std::size_t anomalies() const {
        std::size_t n = 0;
        for(auto const c : counts_) { n += c; }
        return n;
    }

    [[nodiscard]] std::size_t count(Kind k) const { return counts_[static_cast<std::size_t>(k)]; }

    [[nodiscard]] std::string summary() const {
        std::uint64_t received = 0;
        std::uint64_t missing  = 0;
        for(auto const& [key, pr] : producers_) {
            received += pr.received;
            missing += pr.missing;
        }
        return fmt::format(
          "rows {}, stress records {}, boots {}, missing {} (announced {}), quiesced {}, accounted "
          "exactly {}, anomalies {}, tool errors {}",
          rows_,
          received,
          bootsSeen_,
          missing,
          announcedTotal_,
          quiesced_,
          accountedBoots_,
          anomalies(),
          toolErrors_);
    }

    [[nodiscard]] std::string report() const {
        std::string   out;
        std::uint64_t received = 0;
        std::uint64_t missing  = 0;
        for(auto const& [key, pr] : producers_) {
            received += pr.received;
            missing += pr.missing;
        }
        out += fmt::format(
          "rows {}, stress records {}, boots {}, missing {} (announced {}), quiesced {}, "
          "accounted exactly {}, boots passed unread (said by the printer) {}\n",
          rows_,
          received,
          bootsSeen_,
          missing,
          announcedTotal_,
          quiesced_,
          accountedBoots_,
          unreadBootsSaid_);
        for(std::size_t k = 0; k < counts_.size(); ++k) {
            if(counts_[k] != 0) {
                out += fmt::format("  {}: {}\n", kindName(static_cast<Kind>(k)), counts_[k]);
            }
        }
        for(auto const& e : examples_) { out += fmt::format("    {}\n", e); }
        if(toolErrors_ != 0) {
            out
              += fmt::format("  J-Link tool errors (not counted as anomalies): {}\n", toolErrors_);
            for(auto const& e : toolErrorExamples_) { out += fmt::format("    {}\n", e); }
        }
        out += anomalies() == 0 ? "PASS\n" : "FAIL\n";
        return out;
    }

private:
    struct Producer {
        std::optional<unsigned>      channel;
        std::optional<std::uint32_t> last;
        std::uint64_t                received{};
        std::uint64_t                missing{};
    };

    struct Channel {
        std::uint64_t announced{};
        std::uint64_t missing{};
    };

    // of the numbers 0..n-1, those the image logs
    [[nodiscard]] std::uint64_t issuedLogged(std::uint32_t n) const {
        return n == 0 ? 0 : logged(0, n) + (included(0) ? 1 : 0);
    }

    [[nodiscard]] bool included(std::uint32_t seq) const {
        return !floor || levelOf(seq % numShapes) >= *floor;
    }

    [[nodiscard]] std::uint64_t shapesLogged() const {
        std::uint64_t n = 0;
        for(unsigned k = 0; k < numShapes; ++k) { n += included(k) ? 1U : 0U; }
        return n;
    }

    std::map<std::pair<std::uint32_t, unsigned>, Producer>                          producers_;
    std::map<std::pair<std::uint32_t, unsigned>, Channel>                           channels_;
    std::map<std::uint32_t, std::array<std::optional<std::uint32_t>, numProducers>> totals_;

    std::array<std::size_t, static_cast<std::size_t>(Kind::count)> counts_{};
    std::vector<std::string>                                       examples_;
    std::map<unsigned, std::uint64_t>                              pendingAnnounced_;
    std::uint64_t                                                  toolErrors_{};
    std::vector<std::string>                                       toolErrorExamples_;
    std::uint64_t                                                  rows_{};
    std::uint64_t                                                  announcedTotal_{};
    // An epoch is the stretch from one BOOT line to the next: the boot number alone is not unique (a flash
    // overwrites the RAM the firmware counts boots in, and the count starts at 0 again).
    std::uint32_t                epoch_{};
    std::optional<std::uint32_t> epochBoot_;
    bool                         epochComplete_{true};
    std::optional<std::uint32_t> lastBootLine_;
    std::uint64_t                bootsSeen_{};
    std::uint64_t                quiesced_{};
    std::uint64_t                accountedBoots_{};
    // the printer's "N boots passed unread" since the last BOOT line, and the skipped boots they matched
    std::uint64_t pendingUnreadBoots_{};
    std::uint64_t unreadBootsSaid_{};

    void anomaly(Kind        k,
                 std::string what) {
        ++counts_[static_cast<std::size_t>(k)];
        if(onAnomaly) { onAnomaly(fmt::format("{}: {}", kindName(k), what)); }
        if(examples_.size() < maxExamples) {
            examples_.push_back(fmt::format("{}: {}", kindName(k), std::move(what)));
        }
    }

    static std::vector<std::string_view> split(std::string_view s) {
        std::vector<std::string_view> out;
        while(true) {
            auto const pos = s.find('|');
            out.push_back(s.substr(0, pos));
            if(pos == std::string_view::npos) { break; }
            s.remove_prefix(pos + 1);
        }
        return out;
    }

    template<typename T>
    static std::optional<T> number(std::string_view s) {
        T value{};
        auto const [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if(ec != std::errc{} || ptr != s.data() + s.size()) { return std::nullopt; }
        return value;
    }

    // the printer's row "⚠ N boot(s) passed unread (boots that logged a -> b)"
    static std::optional<std::uint64_t> unreadBootCount(std::string_view m) {
        auto const prefix = std::string_view{"⚠ "};
        if(!m.starts_with(prefix)) { return std::nullopt; }
        m.remove_prefix(prefix.size());
        auto const space = m.find(' ');
        if(space == std::string_view::npos || !m.substr(space).contains("passed unread")) {
            return std::nullopt;
        }
        return number<std::uint64_t>(m.substr(0, space));
    }

    void boot(std::vector<std::string_view> const& f) {
        auto const b = number<std::uint32_t>(f[2]);
        if(!b) {
            anomaly(Kind::unparsed, fmt::format("BOOT line {}", fmt::join(f, "|")));
            return;
        }
        // The boot number is uc_log's count of boots that logged (16 bits, kept across resets and flashes of the same
        // board), read after the boot's first record - the same count the printer says unread boots by, so like is
        // compared with like: a jump skipped boots that logged - a loss unless the printer said exactly that many
        // passed unread (and a claim of unread boots with no gap is a mismatch too); the same number twice is a boot
        // that logged and was not counted.
        if(lastBootLine_) {
            auto const skipped = static_cast<std::uint16_t>(*b - *lastBootLine_ - 1U);
            if(*b == *lastBootLine_) {
                anomaly(Kind::bootGap,
                        fmt::format("boot {} twice: a boot that logged was not counted", *b));
            } else if(skipped != 0 || pendingUnreadBoots_ != 0) {
                if(skipped == pendingUnreadBoots_) {
                    unreadBootsSaid_ += skipped;
                } else {
                    anomaly(Kind::bootGap,
                            fmt::format("boot {} after boot {} (the printer said {} passed unread)",
                                        *b,
                                        *lastBootLine_,
                                        pendingUnreadBoots_));
                }
            }
        }
        pendingUnreadBoots_ = 0;
        lastBootLine_       = b;
        // a lost-record the old boot wrote last (its next record went with the reset) belongs to the old boot
        for(auto& [ch, n] : pendingAnnounced_) {
            channels_[{epoch_, ch}].announced += n;
            n = 0;
        }
        if(epochBoot_ || rows_ > 1) { ++epoch_; }
        epochBoot_     = b;
        epochComplete_ = true;
        ++bootsSeen_;
    }

    void total(std::vector<std::string_view> const& f) {
        auto const p = number<unsigned>(f[2]);
        auto const b = number<std::uint32_t>(f[3]);
        auto const n = number<std::uint32_t>(f[4]);
        if(!p || !b || !n || *p >= numProducers) {
            anomaly(Kind::unparsed, fmt::format("TOTAL line {}", fmt::join(f, "|")));
            return;
        }
        if(epochBoot_ && *b != *epochBoot_) {
            anomaly(Kind::staleBoot,
                    fmt::format("TOTAL of boot {} while boot {} runs", *b, *epochBoot_));
            return;
        }
        auto& t = totals_[epoch_];
        t[*p]   = n;
        if(*p + 1 != numProducers) { return; }
        // the last producer's TOTAL: hold every channel of this boot to what its producers issued (not a boot the
        // files begin in the middle of: its early records were never seen)
        ++quiesced_;
        if(!epochComplete_) {
            if(onQuiesced) { onQuiesced(); }
            return;
        }
        // each producer's channel as its records showed it (it depends on the backend and the ISR policy: the
        // default multicore one puts the three ISR producers on one ring, PerLevel on three); a producer none of
        // whose records arrived is held to the boot's total instead
        bool                              ok = true;
        std::map<unsigned, std::uint64_t> issuedOn;
        std::map<unsigned, std::uint64_t> receivedOn;
        std::uint64_t                     unplaced = 0;
        for(unsigned q = 0; q < numProducers; ++q) {
            auto const total = t[q];
            if(!total) { continue; }
            auto const                    it   = producers_.find({epoch_, q});
            auto const                    logs = issuedLogged(*total);
            std::optional<unsigned> const channel
              = it == producers_.end() ? std::nullopt : it->second.channel;
            if(!channel) {
                unplaced += logs;
                continue;
            }
            issuedOn[*channel] += logs;
            receivedOn[*channel] += it->second.received;
        }
        std::uint64_t announcedAll = 0;
        for(auto const& [key, c] : channels_) {
            if(key.first != epoch_) { continue; }
            announcedAll += c.announced;
            issuedOn[key.second] += 0;
        }
        auto const check = [&](std::string const& what,
                               std::uint64_t      issued,
                               std::uint64_t      received,
                               std::uint64_t      announced) {
            if(issued != received + announced) {
                ok = false;
                anomaly(Kind::accounting,
                        fmt::format("boot {} {}: issued {} != received {} + announced lost {}",
                                    *b,
                                    what,
                                    issued,
                                    received,
                                    announced));
            }
        };
        if(unplaced == 0) {
            for(auto const& [ch, issued] : issuedOn) {
                check(fmt::format("channel {}", ch),
                      issued,
                      receivedOn[ch],
                      channels_[{epoch_, ch}].announced);
            }
        } else {
            std::uint64_t issued   = unplaced;
            std::uint64_t received = 0;
            for(auto const& [ch, count] : issuedOn) { issued += count; }
            for(auto const& [ch, count] : receivedOn) { received += count; }
            check("all channels", issued, received, announcedAll);
        }
        if(ok) { ++accountedBoots_; }
        if(onQuiesced) { onQuiesced(); }
        // a quiet phase resets nothing: the next flood's numbers go on from here
    }

    void record(Row const&                           r,
                std::vector<std::string_view> const& f) {
        if(f.size() < 5) {
            anomaly(Kind::unparsed, std::string{r.message});
            return;
        }
        auto const p = number<unsigned>(f[1]);
        auto const b = number<std::uint32_t>(f[2]);
        auto const s = number<std::uint32_t>(f[3]);
        if(!p || !b || !s || *p >= numProducers) {
            anomaly(Kind::unparsed, std::string{r.message});
            return;
        }
        if(!epochBoot_) {
            epochBoot_
              = b;   // the files start in the middle of a boot: what came before is unknown
            epochComplete_ = false;
        } else if(*b != *epochBoot_) {
            anomaly(Kind::staleBoot,
                    fmt::format("a record of boot {} while boot {} runs: {}",
                                *b,
                                *epochBoot_,
                                r.message));
            return;
        }
        {
            auto& placed = producers_[{epoch_, *p}].channel;
            if(!placed) {
                placed = r.channel;
            } else if(*placed != r.channel) {
                anomaly(Kind::wrongText,
                        fmt::format("producer {} on channel {}, before on {}: {}",
                                    *p,
                                    r.channel,
                                    *placed,
                                    r.message));
            }
        }
        auto const expected = expectedText(*p, *b, *s);
        if(r.message != expected) {
            anomaly(Kind::wrongText, fmt::format("got {:?} expected {:?}", r.message, expected));
        }
        if(floor) {
            auto const level = levelOf(*s % numShapes);
            if(level < *floor) {
                anomaly(Kind::wrongText,
                        fmt::format("a record below the floor {} arrived: {}",
                                    levelNames[*floor],
                                    r.message));
            } else if(!r.level.empty() && r.level != levelNames[level]) {
                anomaly(Kind::wrongText,
                        fmt::format("level {} where the image logs {}: {}",
                                    r.level,
                                    levelNames[level],
                                    r.message));
            }
        }
        if(!r.function.empty()) {
            bool const deep = deepSite(*p, *s);
            bool const ok   = deep
                              ? r.function.contains("DeepSite<") && r.function.ends_with("::emit")
                              : r.function == "emit";
            if(!ok) {
                anomaly(Kind::wrongText,
                        fmt::format("function {:?} for a record {}from DeepSite: {}",
                                    r.function,
                                    deep ? "" : "not ",
                                    r.message));
            }
        }

        auto& pr = producers_[{epoch_, *p}];
        auto& ch = channels_[{epoch_, r.channel}];
        ch.announced += pendingAnnounced_[r.channel];
        pendingAnnounced_[r.channel] = 0;
        if(pr.last) {
            if(*s == *pr.last) {
                anomaly(Kind::duplicate, std::string{r.message});
                return;
            }
            if(*s < *pr.last) {
                anomaly(Kind::reorder, fmt::format("{} after {}: {}", *s, *pr.last, r.message));
                return;
            }
            auto const gap = logged(*pr.last, *s);
            pr.missing += gap;
            ch.missing += gap;
        } else if(*s != 0 && epochComplete_) {
            // the boot's first records went before the printer read them: missing too
            auto const gap = *s == 0 ? 0 : logged(0, *s) + (included(0) ? 1 : 0);
            pr.missing += gap;
            ch.missing += gap;
        }
        pr.last = s;
        ++pr.received;
        if(ch.missing > ch.announced) {
            anomaly(Kind::unannounced,
                    fmt::format("boot {} channel {}: {} missing, {} announced, at {}",
                                *b,
                                r.channel,
                                ch.missing,
                                ch.announced,
                                r.message));
            // count it once: the channel's books are level again from here
            ch.announced = ch.missing;
        }
    }
};

}   // namespace uc_log::stress

#ifdef __clang__
    #pragma clang diagnostic pop
#endif
