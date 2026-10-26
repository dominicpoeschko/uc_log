// printer/BootLedger.hpp: the boots nobody read, counted exactly by the image's count of boots that logged (16 bits) -
// also when starts that never logged (a park before main) move the RTT epoch; a count found at another address is
// unknown, never 0.
#include "support/Check.hpp"
#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/printer/BootLedger.hpp"

#include <cstdint>
#include <cstdio>
#include <variant>

namespace {

using uc_log::printer::BootLedger;

std::uint32_t unread(BootLedger::Verdict const& v) {
    auto const* u = std::get_if<BootLedger::Unread>(&v);
    return u != nullptr ? u->boots : 0U;
}

bool nothing(BootLedger::Verdict const& v) { return std::holds_alternative<std::monostate>(v); }

constexpr std::uint32_t address = 0x2000'0100;

void cases() {
    {   // (a) boot 7 read; boot 8 never logged (parked before main: the epoch moved, the count did not); a flash
        BootLedger l;
        l.start(address, 7);
        CHECK(nothing(l.bytesRead(7)), "(a) the boot read is no unread one");
        CHECK(nothing(l.replacing(address, 7)),
              "(a) a start that never logged, then a flash: nothing unread");
    }
    {   // (b) boot 7 read; boot 8 logged and nobody read it; boot 9 read
        BootLedger l;
        l.start(address, 7);
        static_cast<void>(l.bytesRead(7));
        auto const  v = l.bytesRead(9);
        auto const* u = std::get_if<BootLedger::Unread>(&v);
        CHECK(u != nullptr && u->boots == 1 && u->from == 7 && u->to == 8 && !u->replacing,
              "(b) exactly 1, 7 -> 8");
    }
    {   // (c) boot 7 read; one start parked before main, one that logged (count 8); a flash
        BootLedger l;
        l.start(address, 7);
        static_cast<void>(l.bytesRead(7));
        CHECK(unread(l.replacing(address, 8)) == 1, "(c) exactly 1 of the two starts");
        CHECK(nothing(l.replacing(address, 8)), "(c) settled: a second look counts nothing again");
    }
    {   // the printer's first session: boots before it are not its; the current boot counted before or after the start
        BootLedger l;
        l.start(address, 40);
        CHECK(nothing(l.bytesRead(41)),
              "a boot that logged after the session started is the read one");
        BootLedger m;
        m.start(address, 41);
        CHECK(nothing(m.bytesRead(41)),
              "a boot that logged before the session started is the read one");
    }
    {   // a firmware without a boot report that logs: its first record counted the boot; one boot lost in between
        BootLedger l;
        l.start(address, 20);
        static_cast<void>(l.bytesRead(20));
        CHECK(unread(l.bytesRead(22)) == 1,
              "a firmware with no boot report: one lost boot is exactly 1");
    }
    {   // a second session on the same boot (the printer restarted): the same epoch, nothing new
        BootLedger l;
        l.start(address, 7);
        static_cast<void>(l.bytesRead(7));
        CHECK(nothing(l.bytesRead(7)), "the same boot read again: nothing");
        CHECK(unread(l.replacing(address, 8)) == 1,
              "and the next boot that logged, unread, still counts");
    }
    {   // a count that went back, or jumped beyond counting: said, no number
        BootLedger l;
        l.start(address, 50);
        static_cast<void>(l.bytesRead(50));
        CHECK(std::holds_alternative<BootLedger::Uncountable>(l.bytesRead(3)),
              "a count that went back");
        CHECK(std::holds_alternative<BootLedger::Uncountable>(
                l.replacing(address,
                            static_cast<BootLedger::Count>(3 + BootLedger::maxCountableBoots + 2))),
              "a jump beyond counting");
    }
    {   // the count at another address (an image of another layout): unknown, never 0 - at a session start, and before a
        // flash whose files already describe the other image
        BootLedger l;
        l.start(address, 7);
        static_cast<void>(l.bytesRead(7));
        CHECK(std::holds_alternative<BootLedger::Uncountable>(l.start(address + 0x40, 3)),
              "a moved count at a start");
        BootLedger m;
        m.start(address, 7);
        static_cast<void>(m.bytesRead(7));
        CHECK(std::holds_alternative<BootLedger::Uncountable>(m.replacing(address + 0x40, 9)),
              "a moved count before a flash");
    }
    {   // a session read boot 9453's first bytes, then a probe failure dropped them - no line of it: not read after all,
        // and the printer's flash after more loop boots counts it
        BootLedger l;
        l.start(address, 9447);
        static_cast<void>(l.bytesRead(9447));
        CHECK(unread(l.replacing(address, 9452)) == 5, "boots 9448..9452 replaced unread");
        CHECK(nothing(l.bytesRead(9453)), "boot 9453's first bytes");
        l.notRead(9453);
        CHECK(unread(l.replacing(address, 9456)) == 4,
              "9453 not read after all: 9453..9456 unread, 4");
    }
    {   // the printer's own download wrote over RAM (the count read 9723 before, 51937 after): counting starts again - no
        // "unknown", no number, and the boots after it are counted from the new value
        BootLedger l;
        l.start(address, 9720);
        static_cast<void>(l.bytesRead(9720));
        CHECK(unread(l.replacing(address, 9723)) == 3, "before the flash: 3 unread");
        l.restart();
        CHECK(nothing(l.start(address, 51937)), "after the download: a new baseline, nothing said");
        CHECK(nothing(l.bytesRead(51938)), "the new image's first boot read");
        CHECK(unread(l.bytesRead(51941)) == 2, "and counted on from there");
    }
    {   // notRead of another boot than the one settled as read changes nothing
        BootLedger l;
        l.start(address, 7);
        static_cast<void>(l.bytesRead(8));
        l.notRead(5);
        CHECK(unread(l.bytesRead(10)) == 1, "only boot 9 unread");
    }
    {   // across the counter's wrap
        BootLedger l;
        l.start(address, 0xFFFEU);
        static_cast<void>(l.bytesRead(0xFFFEU));
        CHECK(unread(l.bytesRead(1)) == 2, "two boots across the wrap");
    }
}

// The image's side (detail/RttEpoch.hpp) on the host: its two words, a boot (the epoch bump, .bss zeroed), a record.
namespace image {
    namespace d = uc_log::detail;

    void boot() {
        d::epochBlockStale();
        d::epochBlockReady();
        d::bootLogged = false;
    }

    BootLedger::Count logged() {
        return static_cast<BootLedger::Count>(uc_log_boot_words[1] >> 16U);
    }
}   // namespace image

void imageSide() {
    namespace d          = uc_log::detail;
    uc_log_boot_words[0] = 0x1234'5678U;   // what a power-on left
    uc_log_boot_words[1] = 0x9abc'def0U;
    {   // an image whose main never calls logBoot: its first record counts the boot, one lost boot is 1
        BootLedger l;
        image::boot();
        d::noteRecord();
        l.start(address, image::logged());
        CHECK(nothing(l.bytesRead(image::logged())), "the boot read");
        image::boot();   // logs, nobody reads it
        d::noteRecord();
        d::noteRecord();
        image::boot();   // parked before main: never logs
        image::boot();   // read
        d::noteRecord();
        CHECK(unread(l.bytesRead(image::logged())) == 1,
              "one boot that logged lost: 1, the one that never logged: 0");
    }
    {   // a boot counts once, however many records
        image::boot();
        auto const before = image::logged();
        d::noteRecord();
        d::noteRecord();
        d::countLoggedBoot();   // the other core's first record of the same boot
        CHECK(image::logged() == static_cast<BootLedger::Count>(before + 1U),
              "one boot, one count");
        CHECK(d::loggedBootCount() == image::logged(),
              "uc_log::bootsThatLogged()'s word: the count the printer reads");
    }
    {   // a count whose complement does not match (the RAM was not kept): it starts again, never counted on
        uc_log_boot_words[0] = 0x30;
        uc_log_boot_words[1] = 0xcae2'1bc8U;   // junk
        uc_log_boot_words[2] = 0x1234'5678U;
        d::bootLogged        = false;
        d::noteRecord();
        CHECK(image::logged() == 1
                && uc_log_boot_words[2] == (~uc_log_boot_words[1] ^ d::restartMark),
              "junk: the count starts at 1, marked as restarted");
        image::boot();
        d::noteRecord();
        CHECK(image::logged() == 2 && uc_log_boot_words[2] == ~uc_log_boot_words[1],
              "the next boot counts on, unmarked");
        CHECK(d::keptLogged(0x0007'0003U, ~0x0007'0003U) == 0x0007'0003U
                && d::keptLogged(0x0007'0003U, ~0x0007'0003U ^ d::restartMark) == 0x0007'0003U
                && !d::keptLogged(0x0007'0003U, 0),
              "keptLogged: the word with its complement, plain or marked, nothing else");
    }
    {   // two cores both see the boot uncounted and both write: the same value
        std::uint32_t const word  = 0x0007'0003U;
        auto const          first = d::countedBoot(word, 4U);
        CHECK(first == 0x0008'0004U, "counted: count+1, the epoch's low half as mark");
        CHECK(d::countedBoot(first, 4U) == first, "the other core, after: no second count");
        CHECK(d::countedBoot(word, 0x1'0004U) == first, "only the epoch's low half marks the boot");
        CHECK(d::countedBoot(0x0007'0004U, 0x0104U) == 0x0008'0104U,
              "the whole low half: epoch 0x104 is not epoch 4");
        CHECK(d::countedBoot(0xFFFF'0003U, 4U) == 0x0000'0004U, "the count wraps modulo 2^16");
    }
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {    "cases",     cases},
                               {"imageSide", imageSide},
    });
}
