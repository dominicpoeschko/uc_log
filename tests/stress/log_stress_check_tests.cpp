// The stress stream's host side, without a board:
//   1. every shape of uc_log/stress/Records.hpp, serialized by remote_fmt's Printer and decoded by uc_log's RttChannel
//      (the printer's own frame extraction) in pieces of every size, is exactly the text the checker expects
//   2. the checker passes a clean stream with announced losses and catches each kind of anomaly it names
//   3. a measurement, not a pass/fail: how often one damaged frame decodes as a VALID frame with other text
#include "LogStressCheck.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"
#include "uc_log/detail/RttChannel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace sc::literals;

// CRC-8/AUTOSAR's check value (the catalogue of parametrised CRC algorithms): "123456789" -> 0xDF
static_assert([] {
    constexpr std::string_view check{"123456789"};
    std::uint8_t               crc = remote_fmt::protocol::CrcInit;
    for(char const c : check) {
        crc = remote_fmt::protocol::crcStep(crc, static_cast<std::byte>(c));
    }
    return remote_fmt::protocol::crcFinal(crc);
}() == 0xDF);

namespace {

int failures = 0;

#define CHECK(cond, ...)                             \
    do {                                             \
        if(!(cond)) {                                \
            std::printf("FAIL line %d: ", __LINE__); \
            std::printf(__VA_ARGS__);                \
            std::printf("\n");                       \
            ++failures;                              \
        }                                            \
    } while(0)

struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

std::unordered_map<std::uint16_t,
                   std::string> const&
emptyCatalog() {
    static auto const& catalog = *new std::unordered_map<std::uint16_t, std::string>{};
    return catalog;
}

// one record as the firmware's Printer writes it (inline format string: the host has no catalog here)
std::vector<std::byte> serialize(unsigned      p,
                                 std::uint32_t b,
                                 std::uint32_t s) {
    remote_fmt::Printer<VectorBackend> printer{};
    auto const                         v = uc_log::stress::values(p, b, s);
    switch(s % uc_log::stress::NumShapes) {
#define LS_SERIALIZE(shape, fmtString, ...)                        \
    case shape: printer.print(fmtString##_sc, __VA_ARGS__); break;
        UC_LOG_STRESS_SHAPES(LS_SERIALIZE)
#undef LS_SERIALIZE
    case 10:
        // the metric's marker is uc_log's (metric.hpp); on the wire it is part of the format string
        printer.print("LS|{}|{}|{}|m|@METRIC(ls::value[u]={})"_sc,
                      p,
                      b,
                      s,
                      uc_log::stress::metricValue(v));
        break;
    default: break;
    }
    return printer.get_com_backend().memory;
}

std::uint32_t rng = 0x1234'5678U;

std::uint32_t next() {
    rng ^= rng << 13U;
    rng ^= rng >> 17U;
    rng ^= rng << 5U;
    return rng;
}

// 1. the encoder, the printer's frame extraction and the checker agree on every shape
void roundTrip() {
    std::vector<std::byte>   stream;
    std::vector<std::string> expected;
    for(std::uint32_t s = 0; s < 3000; ++s) {
        unsigned const p = s % uc_log::stress::NumProducers;
        auto const     f = serialize(p, s % 3, s);
        stream.insert(stream.end(), f.begin(), f.end());
        expected.push_back(uc_log::stress::expectedText(p, s % 3, s));
    }

    uc_log::detail::RttChannel channel;
    channel.frameCheck = REMOTE_FMT_FRAME_CHECK != 0 ? remote_fmt::FrameCheck::required
                                                     : remote_fmt::FrameCheck::none;
    std::vector<std::string> got;
    std::vector<std::string> errors;
    std::size_t              pos = 0;
    while(pos < stream.size()) {
        std::size_t const piece = std::min<std::size_t>(1 + next() % 97, stream.size() - pos);
        channel.append(std::span{stream}.subspan(pos, piece));
        pos += piece;
        channel.drain([&](std::size_t, std::string const& m, auto) { got.push_back(m); },
                      0,
                      emptyCatalog(),
                      [&](std::string_view e) { errors.emplace_back(e); },
                      true);
    }
    CHECK(errors.empty(),
          "%zu decoder errors, first: %s",
          errors.size(),
          errors.empty() ? "" : errors[0].c_str());
    CHECK(got.size() == expected.size(), "%zu decoded, %zu written", got.size(), expected.size());
    std::size_t wrong = 0;
    for(std::size_t i = 0; i < std::min(got.size(), expected.size()); ++i) {
        if(got[i] != expected[i]) {
            if(wrong++ < 5) {
                CHECK(false,
                      "record %zu: got '%s' expected '%s'",
                      i,
                      got[i].c_str(),
                      expected[i].c_str());
            }
        }
    }
    CHECK(wrong == 0, "%zu records differ", wrong);
}

uc_log::stress::Row row(unsigned           channel,
                        std::string const& message,
                        std::string_view   module = "") {
    return {channel, message, module};
}

std::string lost(std::uint64_t n) {
    return fmt::format("⚠ {} log records lost on this channel (ring full, or no debugger)", n);
}

// 2. the checker: clean, then one anomaly each
void checker() {
    using K = uc_log::stress::Checker::Kind;
    {
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;   // the rows hold views
        auto                     feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        feed(0, "LS|BOOT|0|6 producers|12 shapes");
        for(std::uint32_t s = 0; s < 50; ++s) {
            for(unsigned p = 0; p < uc_log::stress::NumProducers; ++p) {
                if(p == 1 && s >= 10 && s < 15) { continue; }   // lost on channel 1 ...
                if(p == 1 && s == 15) {
                    feed(1, lost(5), "uc_log");
                }   // ... and announced before its next record
                feed(uc_log::stress::ChannelOf[p], uc_log::stress::expectedText(p, 0, s));
            }
        }
        feed(1, "LS|SYNC|1|0");
        for(unsigned p = 0; p < uc_log::stress::NumProducers; ++p) {
            feed(0, fmt::format("LS|TOTAL|{}|0|50", p));
        }
        c.status(
          "2000-01-01T00:00:00.000Z [error] channel 1: the target dropped 5 log records before "
          "0.130 s");
        CHECK(c.anomalies() == 0, "clean stream: %s", c.report().c_str());
        CHECK(c.report().find("accounted exactly 1") != std::string::npos,
              "accounting: %s",
              c.report().c_str());
    }

    auto one = [&](auto&& build, K kind, char const* what) {
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;
        keep.reserve(1000);
        auto feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        build(feed, c);
        CHECK(c.count(kind) >= 1, "%s not caught: %s", what, c.report().c_str());
    };

    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          auto t = uc_log::stress::expectedText(0, 0, 0);
          t[t.size() - 1] ^= 1;
          feed(0, t);
      },
      K::wrongText,
      "a changed character");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, uc_log::stress::expectedText(0, 0, 0));
          feed(0, uc_log::stress::expectedText(0, 0, 0));
      },
      K::duplicate,
      "a duplicate");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, uc_log::stress::expectedText(0, 0, 5));
          feed(0, uc_log::stress::expectedText(0, 0, 3));
      },
      K::reorder,
      "a reorder");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, uc_log::stress::expectedText(0, 0, 0));
          feed(0, uc_log::stress::expectedText(0, 0, 2));
      },
      K::unannounced,
      "an unannounced gap");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, "LS|BOOT|1|x");
          feed(0, uc_log::stress::expectedText(0, 0, 9));
      },
      K::staleBoot,
      "a record of an old boot");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, uc_log::stress::expectedText(0, 0, 0));
          for(unsigned p = 0; p < uc_log::stress::NumProducers; ++p) {
              feed(0, fmt::format("LS|TOTAL|{}|0|{}", p, p == 0 ? 2 : 0));
          }
      },
      K::accounting,
      "a tail lost without a word");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, "LS|BOOT|2|x");
      },
      K::bootGap,
      "a skipped boot");
    {   // the same number twice: said as such (a boot that logged and was not counted), not as a gap of 65 535
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;
        keep.reserve(4);
        auto feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        feed(0, "LS|BOOT|5|x");
        feed(0, "LS|BOOT|5|x");
        CHECK(c.count(K::bootGap) == 1 && c.report().find("boot 5 twice") != std::string::npos,
              "the same boot number twice: %s",
              c.report().c_str());
    }
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|5|x");
          feed(0, "LS|BOOT|0|x");
      },
      K::bootGap,
      "a count that went back to 0 (no flash exception: the count survives flashes)");
    {   // the printer's download wrote over RAM: the count starts again, the next number continues nothing
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;
        keep.reserve(4);
        auto feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        feed(0, "LS|BOOT|9723|x");
        feed(0xFFFF,
             "⚠ the boot count starts again: the printer's download wrote over the target's RAM",
             "uc_log");
        feed(0, "LS|BOOT|51938|x");
        feed(0, "LS|BOOT|51939|x");
        CHECK(c.count(K::bootGap) == 0, "a restart row: no gap across it, %s", c.report().c_str());
    }
    {   // across the count's 16-bit wrap
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;
        keep.reserve(4);
        auto feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        feed(0, "LS|BOOT|65535|x");
        feed(0, "LS|BOOT|0|x");
        CHECK(c.count(K::bootGap) == 0, "65535 then 0: the next boot, %s", c.report().c_str());
    }
    // the printer's count of boots that passed unread: a gap of exactly that many is a loss that was said
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|3|x");
          feed(0xFFFF, "⚠ 1 boot passed unread (boots that logged 7 -> 9)", "uc_log");
          feed(0, "LS|BOOT|6|x");
      },
      K::bootGap,
      "a gap of two where the printer said one");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|3|x");
          feed(0xFFFF, "⚠ 1 boot passed unread (boots that logged 7 -> 9)", "uc_log");
          feed(0, "LS|BOOT|4|x");
      },
      K::bootGap,
      "unread boots said where the boot numbers show none");
    {
        uc_log::stress::Checker  c;
        std::vector<std::string> keep;
        auto                     feed = [&](unsigned ch, std::string m, std::string_view mod = "") {
            keep.push_back(std::move(m));
            c.row(row(ch, keep.back(), mod));
        };
        keep.reserve(10);
        feed(0, "LS|BOOT|3|x");
        feed(0xFFFF, "⚠ 2 boots passed unread (boots that logged 7 -> 10)", "uc_log");
        feed(0, "LS|BOOT|6|x");
        CHECK(c.count(K::bootGap) == 0
                && c.report().find("said by the printer) 2") != std::string::npos,
              "a gap the printer counted exactly is no anomaly, and the report counts it: %s",
              c.report().c_str());
    }
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          auto t = uc_log::stress::expectedText(0, 0, 0);
          feed(0, t.substr(0, 9));
      },
      K::wrongText,
      "a torn record");
    one(
      [](auto& feed, auto&) {
          feed(0, "LS|BOOT|0|x");
          feed(0, "LS|zz|0|0|a|1");
      },
      K::unparsed,
      "a garbled record");
    one([](auto&,
           auto& c) { c.status("[error] channel 0 resync: dropped 12 bytes of 1 stuck frame"); },
        K::printer,
        "a printer resync");
}

// 3. one damaged frame in a stream of good ones: does the decoder hand out a VALID frame with other text? (no
//    checksum in the protocol: the frame's own structure is the only check)
void damagedFrames() {
    std::size_t tries = 0, acceptedWrong = 0, dropped = 0;
    for(std::uint32_t t = 0; t < 20000; ++t) {
        unsigned const    p    = t % uc_log::stress::NumProducers;
        auto              f    = serialize(p, 0, t);
        std::string const want = uc_log::stress::expectedText(p, 0, t);
        // damage: one byte flipped, or a run of 1..8 bytes cut out (a torn piece), never the markers' positions alone
        std::size_t const at = 1 + next() % (f.size() - 2);
        if(t % 2 == 0) {
            f[at] ^= static_cast<std::byte>(1U << (next() % 8U));
        } else {
            std::size_t const n = std::min<std::size_t>(1 + next() % 8, f.size() - 1 - at);
            f.erase(f.begin() + static_cast<std::ptrdiff_t>(at),
                    f.begin() + static_cast<std::ptrdiff_t>(at + n));
        }
        // a good frame behind it: the damaged one may swallow it
        auto const next_ = serialize((p + 1) % uc_log::stress::NumProducers, 0, t + 1);
        f.insert(f.end(), next_.begin(), next_.end());
        uc_log::detail::RttChannel channel;
        channel.append(f);
        std::vector<std::string> got;
        channel.drain([&](std::size_t, std::string const& m, auto) { got.push_back(m); },
                      0,
                      emptyCatalog(),
                      [](std::string_view) {},
                      true);
        ++tries;
        std::string const wantNext
          = uc_log::stress::expectedText((p + 1) % uc_log::stress::NumProducers, 0, t + 1);
        for(auto const& g : got) {
            if(g != want && g != wantNext) { ++acceptedWrong; }
        }
        if(got.empty() || got.back() != wantNext) { ++dropped; }
    }
    if constexpr(REMOTE_FMT_FRAME_CHECK != 0) {
        // what is left is the CRC-8's 1 in 256 of the damage the frame's structure let through
        CHECK(acceptedWrong * 200 < tries,
              "with the frame check, %zu of %zu damaged frames still decoded",
              acceptedWrong,
              tries);
    }
    std::printf(
      "damaged frames (%s): %zu tries, %zu decoded as a valid frame with wrong text (%.2f %%), the "
      "good frame "
      "behind lost or late in %zu\n",
      REMOTE_FMT_FRAME_CHECK != 0 ? "frame check" : "no frame check",
      tries,
      acceptedWrong,
      100.0 * static_cast<double>(acceptedWrong) / static_cast<double>(tries),
      dropped);
}

// the other kind of frame than the catalog says: refused, said, never decoded
void wrongKindRefused() {
    std::vector<std::byte> stream;
    for(std::uint32_t s = 0; s < 50; ++s) {
        auto f = serialize(s % uc_log::stress::NumProducers, 0, s);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    for(auto const expect : {remote_fmt::FrameCheck::required, remote_fmt::FrameCheck::none}) {
        bool const mine
          = (expect == remote_fmt::FrameCheck::required) == (REMOTE_FMT_FRAME_CHECK != 0);
        uc_log::detail::RttChannel channel;
        channel.frameCheck = expect;
        channel.append(stream);
        std::size_t              decoded = 0;
        std::vector<std::string> errors;
        channel.drain([&](std::size_t, std::string const&, auto) { ++decoded; },
                      0,
                      emptyCatalog(),
                      [&](std::string_view e) { errors.emplace_back(e); },
                      false);
        if(mine) {
            CHECK(decoded == 50 && errors.empty(),
                  "the image's own kind: all 50 decoded (%zu, %zu errors)",
                  decoded,
                  errors.size());
        } else {
            bool const said = std::ranges::any_of(errors, [](auto const& e) {
                return e.find("refused") != std::string::npos;
            });
            CHECK(decoded == 0, "the other kind: none decoded (%zu)", decoded);
            CHECK(said, "the other kind: refused with a word");
        }
    }
}
}   // namespace

int main() {
    wrongKindRefused();
    roundTrip();
    checker();
    damagedFrames();
    std::printf(failures == 0 ? "all checks passed\n" : "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
