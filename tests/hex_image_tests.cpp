#include "support/Check.hpp"
#include "uc_log/printer/HexImage.hpp"

#include <cstdio>
#include <sstream>

using namespace uc_log::printer;

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
        // the id the printer names a build by; Kvasir_SDK tools/test_kvasir_bench.py holds kvasir_bench's
        // hex_build_id of this same file to the same literal
        CHECK(imageCrc(*image) == 0x1c6c'3b4bU, "the build id");

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

        // a flash behind a cache, its uncached alias 0x03000000 above: one cache line that is not the flash's
        static constexpr std::uint32_t alias     = 0x0300'0000U;
        auto const                     staleLine = [&](std::uint32_t a, std::span<std::byte> out) {
            flash(a >= 0x1300'0000U ? a - alias : a, out);
            if(a <= 0x1000'0004U && a + out.size() > 0x1000'0004U) {
                out[0x1000'0004U - a] = std::byte{0xEE};
            }
            return true;
        };
        auto const stale = checkFlash(*image, staleLine, alias);
        CHECK(
          stale.flash.result == ImageCheck::Result::match && stale.cacheDiffers()
            && stale.cached.firstDifference == 0x10000004,
          "the flash matches through the alias, the cached view does not: said as a cache line");
        auto const clean = checkFlash(
          *image,
          [&](std::uint32_t a, std::span<std::byte> out) {
              return flash(a >= 0x1300'0000U ? a - alias : a, out);
          },
          alias);
        CHECK(clean.flash.result == ImageCheck::Result::match && !clean.cacheDiffers(),
              "flash and cache agree");
        auto const otherFlash = checkFlash(
          *image,
          [&](std::uint32_t a, std::span<std::byte> out) {
              flash(a >= 0x1300'0000U ? a - alias : a, out);
              if(a >= 0x1300'0000U) { out[1] = std::byte{0}; }
              return true;
          },
          alias);
        CHECK(otherFlash.flash.result == ImageCheck::Result::different
                && otherFlash.flash.firstDifference == 0x10000001 && !otherFlash.cacheDiffers(),
              "another firmware in the flash is judged by the flash, at the image's own address");
        auto const noAlias = checkFlash(*image, staleLine, 0);
        CHECK(noAlias.flash.result == ImageCheck::Result::different,
              "without an alias the cached view is all there is");
    }

    std::istringstream broken{":0400000001020304F3\n"};
    CHECK(!parseIntelHex(broken).has_value(), "a wrong checksum is an error");

    return uc_log::test::report();
}
