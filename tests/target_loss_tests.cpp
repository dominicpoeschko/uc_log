// The printer's side of LossPolicy::Counted (detail/TargetLoss.hpp): the lost-record found by its site and
// counted per channel, an old catalog that does not know the site, the ledger decoded from raw bytes, a ring that
// stays quiet with an unannounced count reported once after 2 s, and the no-ring slot.
#include "uc_log/detail/TargetLoss.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

using uc_log::detail::LogEntry;
using uc_log::detail::SignatureInfo;

constexpr std::string_view LostLine{
  R"(("uc_log.hpp", 356, 3, 2086536[1/1000000]s, "uc_log", """""")⚠ 2106 log records lost on this channel (ring full, or no debugger))"};

// a function-local static that is never destroyed (no exit-time destructor in a test)
SignatureInfo const& lostSite() {
    static SignatureInfo const& site = *new SignatureInfo{
      "uc_log.record_frame",
      "RecordFrame<…>::reportLost<…>",
      "void "
      "uc_log::detail::RecordFrame<uc_log::detail::LevelBoundBackend<uc_log::ComBackend<uc_log::"
      "Tag::User>, "
      "(uc_log::LogLevel)2>, "
      "uc_log::Tag::User>::reportLost<uc_log::detail::LossLedger<2u>::Mark>(uc_log::detail::"
      "LossLedger<2u>::Mark)"};
    return site;
}

SignatureInfo const& otherSite() {
    static SignatureInfo const& site = *new SignatureInfo{"app", "main", "int main()"};
    return site;
}

void lostRecord() {
    LogEntry const entry{1, LostLine, &lostSite()};
    CHECK(entry.parsedOk, "the lost-record's header parses");
    auto const n = uc_log::detail::lostRecordCount(entry);
    CHECK(n && *n == 2106, "the lost-record is found by its site and carries its count");

    LogEntry const notLost{1, LostLine, &otherSite()};
    CHECK(!uc_log::detail::lostRecordCount(notLost),
          "the same text from another site is just a line");

    LogEntry const oldCatalog{1, LostLine, nullptr};
    CHECK(!uc_log::detail::lostRecordCount(oldCatalog),
          "a catalog without the site: nothing recognised");

    constexpr std::string_view Info{
      R"(("main.cpp", 12, 2, 5[1/1000]s, """""")⚠ 3 log records lost on this channel)"};
    LogEntry const plain{0, Info, &lostSite()};
    CHECK(!uc_log::detail::lostRecordCount(plain), "not a warn line of module uc_log");

    uc_log::detail::TargetLoss loss;
    auto const                 text = loss.lostRecord(1, *n, entry.ucTime.time);
    CHECK(text.contains("channel 1") && text.contains("2106 log records")
            && text.contains("2.087 s"),
          "the status message names the channel, the count and the time");
    static_cast<void>(loss.lostRecord(1, 4, entry.ucTime.time));
    static_cast<void>(loss.lostRecord(0, 1, entry.ucTime.time));
    CHECK(loss.total() == 2111, "counted over every channel");
}

std::vector<std::byte> ledgerBytes(std::vector<std::array<std::uint32_t,
                                                          2>> const& rings) {
    std::vector<std::byte> raw;
    for(auto const& r : rings) {
        for(auto const w : {r[0], r[1], 0x1234U, 0U}) {
            for(std::size_t i = 0; i != 4; ++i) {
                raw.push_back(std::byte{static_cast<std::uint8_t>(w >> (8U * i))});
            }
        }
    }
    return raw;
}

void ledger() {
    using Clock        = uc_log::detail::TargetLoss::Clock;
    auto const decoded = uc_log::detail::decodeLossLedger(ledgerBytes({
      { 7, 7},
      {12, 2},
      { 0, 0}
    }));
    CHECK(decoded.size() == 3 && decoded[1].lost == 12 && decoded[1].reported == 2,
          "decoded from raw bytes");
    CHECK(uc_log::detail::decodeLossLedger(std::vector<std::byte>(20)).empty(),
          "a size that is no whole slot");

    uc_log::detail::TargetLoss loss;
    auto const                 t0 = Clock::now();
    CHECK(loss.ledger(decoded, t0).empty(), "a pending count is not reported at once");
    CHECK(loss.ledger(decoded, t0 + std::chrono::seconds{1}).empty(), "nor after 1 s");
    auto const late = loss.ledger(decoded, t0 + std::chrono::seconds{3});
    CHECK(late.size() == 1 && late[0].contains("ring 1") && late[0].contains("10 log records"),
          "a ring quiet with 10 unannounced records: one message after 2 s");
    CHECK(loss.ledger(decoded, t0 + std::chrono::seconds{6}).empty(), "and only once");

    auto const reported = uc_log::detail::decodeLossLedger(ledgerBytes({
      { 7,  7},
      {12, 12},
      { 0,  0}
    }));
    CHECK(loss.ledger(reported, t0 + std::chrono::seconds{7}).empty(),
          "reported in-band: nothing to say");

    auto const noRing = uc_log::detail::decodeLossLedger(ledgerBytes({
      { 7,  7},
      {12, 12},
      { 3,  0}
    }));
    auto const told   = loss.ledger(noRing, t0 + std::chrono::seconds{8});
    CHECK(told.size() == 1 && told[0].contains("3 log records") && told[0].contains("no RTT ring"),
          "the no-ring slot is told at once");
    CHECK(loss.ledger(noRing, t0 + std::chrono::seconds{9}).empty(), "once");
}

void maps() {
    // lld: the input section, then the demangled symbol with vma, lma, size (shortened)
    std::istringstream lld{
      "20079780 20079780       30     4         "
      "x.elf.lto.o:(.bss._ZN6uc_log6detail11LossMembersIS2_Lj2EE10lossLedgerE)\n"
      "20079780 20079780       30     1                 uc_log::detail::LossMembers<B, L, "
      "2u>::lossLedger\n"};
    std::size_t count = 0;
    auto const  a     = uc_log::detail::findLossLedger(lld, count);
    CHECK(a && a->address == 0x2007'9780U && a->size == 0x30 && count == 1, "lld map");

    // GNU ld: the input section with its numbers on the next line, then the demangled symbol (spaces in it)
    std::istringstream gnu{
      " .bss._ZN6uc_log6detail11LossMembersINS_20DefaultRttComBackendIS2_Lj2EE10lossLedgerE\n"
      "                0x20079780       0x30 /tmp/ccuMybFE.ltrans0.ltrans.o\n"
      "                0x20079780                uc_log::detail::LossMembers<B, L, "
      "2u>::lossLedger\n"};
    auto const b = uc_log::detail::findLossLedger(gnu, count);
    CHECK(b && b->address == 0x2007'9780U && b->size == 0x30 && count == 1, "GNU ld map");

    std::istringstream two{
      "20079780 20079780       30     1                 uc_log::detail::LossMembers<A, L, "
      "2u>::lossLedger\n"
      "200797b0 200797b0       30     1                 uc_log::detail::LossMembers<B, L, "
      "2u>::lossLedger\n"};
    CHECK(!uc_log::detail::findLossLedger(two, count) && count == 2,
          "two ledgers: none read, both counted");

    std::istringstream none{
      "20079440 20079440       48     4                 uc_log::X::rttControlBlock\n"};
    CHECK(!uc_log::detail::findLossLedger(none, count) && count == 0,
          "a firmware that does not count");
}
}   // namespace

int main() {
    lostRecord();
    ledger();
    maps();
    if(failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all target loss tests passed\n");
    return 0;
}
