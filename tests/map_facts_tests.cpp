// detail/MapFacts.hpp: the RTT control block from the three map shapes (its address, and its buffer count from its
// size), the other facts from one read, and a map that changes on disk read again.
#include "uc_log/detail/MapFacts.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

bool blockIs(std::string const& map,
             std::uint32_t      address,
             std::uint32_t      buffers) {
    auto const b = uc_log::detail::controlBlockFromMap(map, "x.map");
    return b && b->address == address && b->totalBuffers == buffers;
}

void shapes() {
    // lld: vma, lma, size, align on the symbol line; 24 + 3 * 24 bytes = 3 buffers
    CHECK(
      blockIs(
        "20000100 20000100       60     4         x.o:(.bss._ZN6uc_log6detail15rttControlBlockE)\n"
        "20000100 20000100       60     1                 uc_log::detail::rttControlBlock\n",
        0x2000'0100U,
        3),
      "lld map");
    // GNU ld: the size on the input-section line before, the symbol line with the same address
    CHECK(blockIs(" .bss._ZN6uc_log6detail15rttControlBlockE\n"
                  "                0x20000200       0x48 main.o\n"
                  "                0x20000200                uc_log::detail::rttControlBlock\n",
                  0x2000'0200U,
                  2),
          "GNU ld map");
    // GNU ld, the block in a section of another name: address and size on that section's line, the symbol line after
    CHECK(blockIs(" .bss.rtt\n"
                  "                0x20000280       0x60 main.o\n"
                  "                0x20000280                uc_log::detail::rttControlBlock\n",
                  0x2000'0280U,
                  3),
          "GNU ld map, size from the section line before the symbol");
    // GNU ld with LTO: the symbol localised, only the input-section line (numbers on it or on the next one)
    CHECK(blockIs(
            " .bss._ZN6uc_log6detail15rttControlBlockE 0x20000300 0x30 /tmp/cc.ltrans0.ltrans.o\n",
            0x2000'0300U,
            1),
          "GNU ld with LTO, one line");
    CHECK(blockIs(" .bss._ZN6uc_log6detail15rttControlBlockE\n                0x20000310       "
                  "0x30 /tmp/cc.ltrans0.o\n",
                  0x2000'0310U,
                  1),
          "GNU ld with LTO, numbers on the next line");
    CHECK(!uc_log::detail::controlBlockFromMap("20000100 20000100 10 1 something_else\n", "x.map"),
          "no block: said");
    CHECK(
      !blockIs("20000100 20000100       61     1                 uc_log::detail::rttControlBlock\n",
               0x2000'0100U,
               3),
      "a size that is no header plus whole descriptors is not taken");
}

void cachedAndReread() {
    auto const file
      = std::filesystem::temp_directory_path() / fmt::format("map_facts_{}.map", ::getpid());
    {
        std::ofstream out{file};
        out << "20000100 20000100       30     1                 uc_log::detail::rttControlBlock\n"
               "20002cfc 20002cfc       10     1                 kvasir_announced_reset\n"
               "20000000 20000000        8     4                 uc_log_boot_words\n"
               "20079780 20079780       30     1                 uc_log::detail::LossMembers<B, L, "
               "2u>::lossLedger\n";
    }
    uc_log::detail::MapFacts facts{file};
    auto const               first = facts.get();
    CHECK(first->block && first->block->address == 0x2000'0100U && first->block->totalBuffers == 1,
          "the block");
    CHECK(first->announce == 0x2000'2cfcU && first->epoch == 0x2000'0000U,
          "the announced-reset block and the epoch");
    CHECK(first->loggedBoots == 0x2000'0004U, "the logged count: the word after the epoch");
    CHECK(first->ledger && first->ledger->address == 0x2007'9780U && first->ledgers == 1,
          "the ledger");
    CHECK(facts.get() == first, "an unchanged file is not read again");
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    {
        std::ofstream out{file};
        out << "20000400 20000400       48     1                 uc_log::detail::rttControlBlock\n";
    }
    std::filesystem::last_write_time(file,
                                     std::filesystem::last_write_time(file)
                                       + std::chrono::seconds{1});
    auto const second = facts.get();
    CHECK(second != first && second->block && second->block->address == 0x2000'0400U
            && !second->announce && !second->epoch && !second->loggedBoots && !second->ledger,
          "a rebuilt map is read again");
    std::filesystem::remove(file);
    CHECK(!facts.get()->block, "a map that went: said, not the old one");
}
}   // namespace

int main() {
    shapes();
    cachedAndReread();
    std::printf(failures == 0 ? "ALL OK\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
