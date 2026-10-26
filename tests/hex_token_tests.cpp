// printer/HexToken.hpp: the one hex reader of the map parser and the command line.
#include "support/Check.hpp"
#include "uc_log/printer/HexToken.hpp"

#include <string_view>
#include <vector>

using namespace uc_log::printer;

namespace {

void values() {
    CHECK(hexValue("20000100") == 0x2000'0100U && hexValue("0x1F") == 0x1FU
            && hexValue("0XaB") == 0xABU,
          "with and without 0x, either case");
    CHECK(!hexValue("") && !hexValue("0x") && !hexValue("12zz") && !hexValue("-1")
            && !hexValue(" 1"),
          "only a whole token of hex digits");
    CHECK(hexValue("ffffffff") == 0xFFFF'FFFFU && !hexValue("100000000"), "32 bits, no wrap");
}

void tokens() {
    std::string_view text{"  0x20000100 \t 0x48 main.o"};
    CHECK(hexToken(text) == 0x2000'0100U && hexToken(text) == 0x48U,
          "blank-separated, spaces and tabs");
    CHECK(!hexToken(text) && text == " main.o",
          "a token that is no number leaves the text as it was");
    std::string_view empty{"   "};
    CHECK(!hexToken(empty), "nothing left");
    CHECK((hexTokens("20079780 20079780       30     1   x::lossLedger", 3)
           == std::vector<std::uint32_t>{0x2007'9780U, 0x2007'9780U, 0x30U}),
          "the first n columns of an lld line");
    CHECK((hexTokens("0x20079780 x.o 0x30", 3) == std::vector<std::uint32_t>{0x2007'9780U}),
          "stops at the first non-number");
}

void words() {
    CHECK((hexWords("40000000,ff,0x1", 4) == std::vector<std::uint32_t>{0x4000'0000U, 0xFFU, 1U}),
          "a comma list");
    CHECK((hexWords("1,2", 2) == std::vector<std::uint32_t>{1U, 2U}), "count words");
    CHECK(hexWords("1,2,3,4,5", 2).empty() && hexWords("1,2,", 4).empty()
            && hexWords("1", 0).empty(),
          "a word too many, a trailing comma: nothing");
    CHECK(hexWords("1,,2", 4).empty() && hexWords("1,x", 4).empty() && hexWords("1, 2", 4).empty(),
          "a malformed word: nothing");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"values", values},
                               {"tokens", tokens},
                               { "words",  words},
    });
}
