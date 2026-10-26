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
    std::filesystem::path          hex;
    std::array<std::byte, 4>       flash{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    int                            reads{};
    Said                           said;
    uc_log::printer::FirmwareCheck check;
    uc_log::printer::FirmwareCheck::EmitF emit = [this](State s, std::string const& t, bool e) {
        said.states.push_back(s);
        said.texts.push_back(t);
        said.errors.push_back(e);
    };

    Fixture()
      : hex{std::filesystem::temp_directory_path()
            / ("uc_log_firmware_check_" + std::to_string(::getpid()) + ".hex")} {
        std::ofstream{hex} << hexImage;
    }

    ~Fixture() { std::filesystem::remove(hex); }

    Fixture(Fixture const&)            = delete;
    Fixture& operator=(Fixture const&) = delete;

    // a probe round trip (a zero-time fake would hang a loop on the virtual clock)
    auto reader() UC_LOG_LIFETIMEBOUND {
        return [this](std::uint32_t address, std::span<std::byte> out) {
            uc_log::printer::Clock::sleep_for(1ms);
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
    });
}
