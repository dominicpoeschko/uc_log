// JLink's pre-reset writes, against the fake DLL.

#include "jlink/JLink.hpp"

#include "jlink/JLinkFake.h"

#include <cstdio>
#include <stdexcept>
#include <string_view>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

static bool throws(std::string_view line) {
    try {
        JLink::parseCommand(line);
    } catch(std::runtime_error const&) { return true; }
    return false;
}

int main() {
    CHECK((JLink::parseCommand("w4 0x4001A004 0x01000000")
           == JLink::MemoryWrite{0x4001A004, 0x01000000}),
          "w4 in hex");
    CHECK((JLink::parseCommand("  W4\t16  0X10 ") == JLink::MemoryWrite{16, 16}),
          "decimal, upper case, extra blanks");
    CHECK(throws("mem32 0x0 1"), "another command is refused");
    CHECK(throws("w4 0x0"), "a missing value is refused");
    CHECK(throws("w4 0x0 1 2"), "an extra word is refused");
    CHECK(throws("w4 0x100000000 1"), "more than 32 bits is refused");
    CHECK(throws("w4 0xZZ 1"), "not a number is refused");
    CHECK(throws("w4 0x 1"), "an empty number is refused");

    {
        JLink jlink{"fake",
                    4000,
                    JLink::Connection{},
                    [](std::string_view) {},
                    [](std::string_view) {}};
        fakeJLinkClearWrites();
        jlink.resetTarget();
        CHECK(fakeJLinkWrites().empty(), "no commands, no writes");

        jlink.setPreResetCommands({
          JLink::MemoryWrite{0x4001A004, 0x01000000},
          JLink::MemoryWrite{0x4001B004, 0x01000000}
        });
        jlink.resetTarget();
        auto const writes = fakeJLinkWrites();
        CHECK(writes.size() == 2 && writes[0].first == 0x4001A004 && writes[1].first == 0x4001B004
                && writes[1].second == 0x01000000,
              "resetTarget writes the commands in order");
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
