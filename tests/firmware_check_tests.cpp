// printer/FirmwareCheck.hpp on the virtual clock: a check at the session start says match / different / RAM image,
// the decode gate primes on its first call, re-checks a changed hex file and re-checks "different" every 2 s.
#include "support/Check.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/FirmwareCheck.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using State = uc_log::printer::control::FirmwareState;
using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

// 4 bytes 01 02 03 04 at 0x10000000, then a build-id note (detail/BuildId.hpp) with the hash A0 A1 ... B3
constexpr char const* hexWithNote
  = ":020000041000EA\n"
    ":1000000001020304040000001400000003000000CB\n"
    ":10001000474E5500A0A1A2A3A4A5A6A7A8A9AAAB34\n"
    ":08002000ACADAEAFB0B1B2B35C\n"
    ":00000001FF\n";
// the same with a note of another type (1, not NT_GNU_BUILD_ID 3): no build id
constexpr char const* hexWithOtherNote
  = ":020000041000EA\n"
    ":1000000001020304040000001400000001000000CD\n"
    ":10001000474E5500A0A1A2A3A4A5A6A7A8A9AAAB34\n"
    ":08002000ACADAEAFB0B1B2B35C\n"
    ":00000001FF\n";
constexpr std::uint32_t                 ramBase = 0x2000'0000;
constexpr uc_log::printer::BuildIdPlace place{.bootWords = ramBase, .note = 0x1000'0004};

// 4 bytes 01 02 03 04 at 0x10000000
constexpr char const* hexImage
  = ":020000041000EA\n"
    ":0400000001020304F2\n"
    ":00000001FF\n";

struct Said {
    std::vector<State>       states;
    std::vector<std::string> texts;
    std::vector<bool>        errors;
};

struct Fixture {
    std::filesystem::path hex;
    // the target's flash from 0x10000000: the image's 4 bytes, then erased-looking zeros (a note-sized hex file reads
    // them in the flash compare)
    std::array<std::byte, 64> flash{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 20>
         ram{};     // uc_log's boot words at ramBase: [0] epoch, [3] [4] the id ^ epoch
    int  reads{};   // of the flash
    int  ramReads{};
    Said said;
    uc_log::printer::FirmwareCheck        check;
    uc_log::printer::FirmwareCheck::EmitF emit = [this](State s, std::string const& t, bool e) {
        said.states.push_back(s);
        said.texts.push_back(t);
        said.errors.push_back(e);
    };

    explicit Fixture(char const* image = hexImage)
      : hex{std::filesystem::temp_directory_path()
            / ("uc_log_firmware_check_" + std::to_string(::getpid()) + ".hex")} {
        std::ofstream{hex} << image;
    }

    ~Fixture() { std::filesystem::remove(hex); }

    Fixture(Fixture const&)            = delete;
    Fixture& operator=(Fixture const&) = delete;

    // the boot words a boot of `epoch` leaves: the hash A0 A1 ... A7 (`last`: its last byte), each word XOR the epoch
    void bootWords(std::uint32_t epoch,
                   std::byte     last = std::byte{0xA7}) {
        std::array<std::byte, 8> id{};
        for(std::size_t i = 0; i != id.size(); ++i) { id[i] = static_cast<std::byte>(0xA0 + i); }
        id[7] = last;
        for(std::size_t i = 0; i != 4; ++i) {
            ram[i] = static_cast<std::byte>((epoch >> (8 * i)) & 0xFFU);
        }
        for(std::size_t i = 0; i != id.size(); ++i) { ram[12 + i] = id[i] ^ ram[i % 4]; }
    }

    // a probe round trip (a zero-time fake would hang a loop on the virtual clock)
    auto reader() UC_LOG_LIFETIMEBOUND {
        return [this](std::uint32_t address, std::span<std::byte> out) {
            uc_log::printer::Clock::sleep_for(1ms);
            if(address >= ramBase) {
                ++ramReads;
                for(std::size_t i = 0; i != out.size(); ++i) {
                    out[i] = ram[address - ramBase + i];
                }
                return true;
            }
            ++reads;
            for(std::size_t i = 0; i != out.size(); ++i) {
                out[i] = flash[address - 0x1000'0000U + i];
            }
            return true;
        };
    }
};

void matchAndDifferent() {
    Fixture f;
    f.check.check(f.hex.string(), false, f.reader(), f.emit);
    CHECK(f.said.states.size() == 1 && f.said.states[0] == State::match && !f.said.errors[0]
            && f.said.texts[0].starts_with("firmware matches "),
          "a match is a status line");
    CHECK(f.check.get().state == State::match, "and the summary says so");
    f.flash[2] = std::byte{9};
    f.check.check(f.hex.string(), false, f.reader(), f.emit);
    CHECK(f.said.states.size() == 2 && f.said.states[1] == State::different && f.said.errors[1]
            && f.said.texts[1].contains("does NOT run"),
          "a mismatch is an error");
}

void ramImage() {
    Fixture f;
    f.check.check(f.hex.string(), true, f.reader(), f.emit);
    CHECK(f.said.states.size() == 1 && f.said.states[0] == State::unchecked && !f.said.errors[0]
            && f.said.texts[0].contains("RAM image") && f.reads == 0,
          "a RAM image: unchecked, no error, nothing read");
}

void gate() {
    Fixture f;
    f.flash[0] = std::byte{7};
    f.check.check(f.hex.string(), false, f.reader(), f.emit);
    CHECK(!f.check.gate(f.hex.string(), false, f.reader(), f.emit) && f.said.states.size() == 1,
          "the first gate call primes: closed on the check, nothing read again");
    CHECK(!f.check.gate(f.hex.string(), false, f.reader(), f.emit) && f.said.states.size() == 1,
          "within 2 s: no new check");
    f.flash[0] = std::byte{1};
    uc_log::printer::Clock::sleep_for(2100ms);
    CHECK(f.check.gate(f.hex.string(), false, f.reader(), f.emit) && f.said.states.size() == 2
            && f.said.states[1] == State::match,
          "\"different\" re-checked after 2 s: a flash read that was wrong for a moment opens the "
          "gate");
    CHECK(f.check.gate(f.hex.string(), false, f.reader(), f.emit) && f.said.states.size() == 2,
          "a match is not re-checked");
    // a rebuilt hex that matches what runs
    f.flash[1] = std::byte{5};
    std::ofstream{f.hex} << ":020000041000EA\n:0400000001050304EF\n:00000001FF\n";
    std::filesystem::last_write_time(f.hex, std::filesystem::last_write_time(f.hex) + 1s);
    CHECK(f.check.gate(f.hex.string(), false, f.reader(), f.emit) && f.said.states.size() == 3
            && f.said.states[2] == State::match,
          "a changed hex file is checked again");
}

// The RAM copy of the build id, XOR the epoch it was written for, equal to the hex file's note: a match without a
// flash read. Another id, one an earlier boot left (written for another epoch: the stale RAM of another image), an odd
// epoch (a boot in its init), none (zeros), a hex file without a note or no place in the map: the flash is compared.
void buildId() {
    {
        Fixture f{hexWithNote};
        f.bootWords(0x1234'5678U);
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.said.states.size() == 1 && f.said.states[0] == State::match && f.reads == 0
                && f.ramReads == 1
                && f.said.texts[0].contains("(build 9b6805e9, build id a0a1a2a3a4a5a6a7 in RAM"),
              "build id: the RAM copy matches - no flash read");
        f.bootWords(0x1234'5678U, std::byte{0});
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECKF(f.said.states.size() == 2 && f.said.states[1] == State::different && f.reads != 0,
               "build id: another id in RAM - the flash is compared (%d reads)",
               f.reads);
    }
    {
        Fixture f{hexWithNote};
        f.bootWords(0x1234'5678U);
        f.ram[0] = std::byte{
          0x7a};   // a later boot moved the epoch on and wrote no id: the copy is an older boot's
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.said.states.size() == 1 && f.said.states[0] == State::different && f.reads != 0,
              "build id: a copy written for another epoch (stale RAM) - the flash is compared");
    }
    {
        Fixture f{hexWithNote};
        f.bootWords(0x1234'5679U);   // odd: a boot in its init
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.said.states.size() == 1 && f.reads != 0,
              "build id: an odd epoch - the flash is compared");
    }
    {
        Fixture f{hexWithNote};
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.said.states.size() == 1 && f.reads != 0,
              "build id: none in RAM (zeros) - the flash is compared");
    }
    {
        Fixture f;   // no note at 0x10000004: the hex file ends there
        f.bootWords(0x1234'5678U);
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.said.states.size() == 1 && f.said.states[0] == State::match && f.reads != 0
                && f.ramReads == 0,
              "build id: no note in the hex file - the flash is compared, RAM not read");
    }
    {
        Fixture f{hexWithOtherNote};
        f.bootWords(0x1234'5678U);
        f.check.check(f.hex.string(), false, f.reader(), f.emit, place);
        CHECK(f.reads != 0 && f.ramReads == 0,
              "build id: a note of another type - the flash is compared");
    }
    {
        Fixture f{hexWithNote};
        f.bootWords(0x1234'5678U);
        f.check.check(f.hex.string(), false, f.reader(), f.emit);
        CHECK(f.reads != 0 && f.ramReads == 0,
              "build id: no place in the map - the flash is compared");
    }
}
}   // namespace

int main(int    argc,
         char** argv) {
    uc_log::printer::VirtualTime const virtualTime;
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"matchAndDifferent", matchAndDifferent},
                               {         "ramImage",          ramImage},
                               {             "gate",              gate},
                               {          "buildId",           buildId},
    });
}
