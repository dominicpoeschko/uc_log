// Where a RAM image starts: the vector table from the linker map, its words from the hex file.

#include "uc_log/detail/RamImage.hpp"

#include <cstdio>
#include <sstream>
#include <string>

using namespace uc_log::detail;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

static std::expected<std::uint32_t,
                     std::string> tableOf(std::string const& map) {
    std::istringstream in{map};
    return vectorTableFromMap(in);
}

static std::vector<HexSegment> image(std::string const& hex) {
    std::istringstream in{hex};
    return parseIntelHex(in).value_or(std::vector<HexSegment>{});
}

static std::expected<JLink::RamImageStart,
                     std::string>
startOf(std::string const& map,
        std::string const& hex) {
    std::istringstream in{map};
    return ramImageStart(in, image(hex));
}

int main() {
    // lld (an RP2350 map), and the GNU ld shape
    std::string const lld
      = "     VMA      LMA     Size Align Out     In      Symbol\n"
        "20000000 20000000      124     4 .vectors\n"
        "20000000 20000000        0     1         . = ALIGN(4)\n"
        "20000000 20000000        0     1         _LINKER_vectors_start_ = .\n";
    std::string const gnu
      = " .vectors       0x20000100      0xc0\n"
        "                0x20000100                _LINKER_vectors_start_ = .\n";
    CHECK(tableOf(lld) == 0x2000'0000U, "lld map");
    CHECK(tableOf(gnu) == 0x2000'0100U, "GNU ld map");
    CHECK(!tableOf("20000000 20000000 0 1 PROVIDE(x_LINKER_vectors_start_ = .)\n").has_value(),
          "only the symbol itself");
    CHECK(!tableOf("20000000 20000000 0 1 _LINKER_vectors_start_end_ = .\n").has_value(),
          "not a longer name");
    CHECK(!tableOf("_LINKER_vectors_start_ = .\n").has_value(), "a line without an address");
    auto const none = tableOf("nothing here\n");
    CHECK(!none && none.error().find("_LINKER_vectors_start_") != std::string::npos,
          "a missing symbol is named");

    // SP 0x20082000, reset 0x20000199, at 0x20000000
    std::string const hex
      = ":020000042000DA\n"
        ":080000000020082099010020F6\n"
        ":00000001FF\n";
    auto const start = startOf(lld, hex);
    CHECK((start == JLink::RamImageStart{0x2000'0000U, 0x2008'2000U, 0x2000'0199U}),
          "the words at the table");
    if(!start) { std::printf("  %s\n", start.error().c_str()); }

    auto const elsewhere = startOf(gnu, hex);
    CHECK(!elsewhere
            && elsewhere.error().find("no vector table at 0x20000100") != std::string::npos,
          "a table the hex does not hold");

    // reset vector without the Thumb bit
    auto const arm = startOf(lld,
                             ":020000042000DA\n"
                             ":080000000020082098010020F7\n"
                             ":00000001FF\n");
    CHECK(!arm && arm.error().find("implausible") != std::string::npos, "an ARM reset vector");
    // SP not word-aligned
    auto const odd = startOf(lld,
                             ":020000042000DA\n"
                             ":080000000220082099010020F4\n"
                             ":00000001FF\n");
    CHECK(!odd && odd.error().find("implausible") != std::string::npos, "an unaligned SP");

    auto const files = ramImageStart(std::string{"/nonexistent.map"}, std::string{"x.hex"});
    CHECK(!files && files.error().find("cannot open") != std::string::npos, "a missing map file");

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
