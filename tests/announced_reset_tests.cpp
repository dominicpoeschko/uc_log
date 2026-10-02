// The announced-reset block: found in both linkers' maps, decoded, and when it asks for an ack.
#include "uc_log/detail/AnnouncedReset.hpp"

#include <array>
#include <cstdio>
#include <sstream>
#include <string>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

using namespace uc_log::detail::announced_reset;

static std::expected<std::uint32_t,
                     std::string>
fromMap(std::string const& text) {
    std::istringstream in{text};
    return addressFromMap(in);
}

int main() {
    // lld: the input-section line and the symbol line both start with the address
    CHECK(fromMap(
            "     VMA      LMA     Size Align Out     In      Symbol\n"
            "20002cfc 20002cfc       10     4         x.elf.lto.o:(.data.kvasir_announced_reset)\n"
            "20002cfc 20002cfc       10     1                 kvasir_announced_reset\n")
            == 0x20002cfcU,
          "lld map");
    // GNU ld: section line with the numbers on the next line, then the symbol line; a copy
    // --gc-sections dropped is listed first, at 0
    CHECK(fromMap("Discarded input sections\n"
                  "\n"
                  " .data.kvasir_announced_reset\n"
                  "                0x00000000       0x10 other.o\n"
                  "\n"
                  "Memory Configuration\n"
                  "Linker script and memory map\n"
                  " .data.kvasir_announced_reset\n"
                  "                0x20000400       0x10 main.o\n"
                  "                0x20000400                kvasir_announced_reset\n")
            == 0x20000400U,
          "GNU ld map, discarded copy skipped");
    // GNU ld with LTO: only a section line, the numbers on it
    CHECK(fromMap(" .data.kvasir_announced_reset 0x20000410 0x10 /tmp/ccX.ltrans0.ltrans.o\n")
            == 0x20000410U,
          "GNU ld map, section line only");
    CHECK(!fromMap("20000000 20000000 10 1 kvasir_announced_reset_other\n"
                   "20000000 20000000 10 1 my_kvasir_announced_reset\n"),
          "a longer name is not the block");
    CHECK(!fromMap("20000000 20000000 10 1 rttControlBlock\n"), "no block: the feature is off");

    std::array<std::byte, BlockSize> raw{};
    auto const                       put = [&](std::size_t word, std::uint32_t v) {
        for(std::size_t i = 0; i != 4; ++i) {
            raw[4 * word + i] = static_cast<std::byte>(v >> (8 * i));
        }
    };
    put(0, Magic);
    put(1, HostArmed);
    put(2, 3);
    put(3, 2);
    auto const b = decode(raw);
    CHECK(b.valid() && b.armed == HostArmed && b.request == 3 && b.ack == 2, "decoded");
    CHECK(b.pending(), "request != ack: the firmware waits");
    put(3, 3);
    CHECK(!decode(raw).pending(), "acked: nothing to do");
    put(2, 0);
    put(3, 0);
    CHECK(!decode(raw).pending(), "a fresh boot's block asks nothing");
    put(0, 0x12345678);
    put(2, 5);
    CHECK(!decode(raw).pending(), "no magic: not the block");
    // the firmware's bytes: "ATSR", "HARM"
    CHECK(Magic == 0x5253'5441U && HostArmed == 0x4D52'4148U,
          "the numbers of Kvasir_SDK Util/AnnouncedReset.hpp");

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
