#include "uc_log/detail/HexImage.hpp"

#include <cstdio>
#include <sstream>

using namespace uc_log::detail;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

int main() {
    // zlib's check value: crc32("123456789") = 0xCBF43926
    {
        std::string_view const text{"123456789"};
        CHECK(crc32(std::as_bytes(std::span{text})) == 0xCBF43926U, "crc32 is zlib's");
        auto const head = crc32(std::as_bytes(std::span{text.substr(0, 4)}));
        CHECK(crc32(std::as_bytes(std::span{text.substr(4)}), head) == 0xCBF43926U,
              "crc32 continues over pieces");
    }

    std::string const hex
      = ":020000041000EA\n"
        ":0400000001020304F2\n"
        ":0400040005060708DE\r\n"
        ":020000042000DA\n"
        ":02000000AABB99\n"
        ":0400000510000101E5\n"
        ":00000001FF\n";
    std::istringstream in{hex};
    auto const         image = parseIntelHex(in);
    CHECK(image.has_value(), "the file parses");
    if(image) {
        CHECK(image->size() == 2, "records that touch are one segment");
        CHECK((*image)[0].address == 0x10000000 && (*image)[0].data.size() == 8
                && (*image)[0].data[7] == std::byte{8},
              "linear base address and data");
        CHECK((*image)[1].address == 0x20000000 && (*image)[1].data.size() == 2, "second segment");

        auto const flash = [](std::uint32_t address, std::span<std::byte> out) {
            for(std::size_t i = 0; i != out.size(); ++i) {
                out[i] = static_cast<std::byte>(address - 0x10000000 + i + 1);
            }
            return true;
        };
        auto const same = compareImage(*image, flash);
        CHECK(same.result == ImageCheck::Result::match && same.comparedBytes == 8,
              "the flash part matches; the SRAM part is not looked at");
        auto const other = compareImage(*image, [&](std::uint32_t a, std::span<std::byte> out) {
            flash(a, out);
            out[5] = std::byte{0};
            return true;
        });
        CHECK(other.result == ImageCheck::Result::different && other.firstDifference == 0x10000005,
              "another firmware: where it differs first");
        auto const dead
          = compareImage(*image, [](std::uint32_t, std::span<std::byte>) { return false; });
        CHECK(dead.result == ImageCheck::Result::unreadable, "a target that cannot be read");
    }

    std::istringstream broken{":0400000001020304F3\n"};
    CHECK(!parseIntelHex(broken).has_value(), "a wrong checksum is an error");

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
