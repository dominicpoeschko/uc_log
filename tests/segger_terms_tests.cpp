// printer/SeggerTerms.hpp: today's acceptance of SEGGER's EDU terms written into a registry file - both places the key
// is kept, every other byte as it was, the date packed (year << 16) | (month << 8) | day; a missing file or key is said,
// not created.
#include "support/Check.hpp"
#include "uc_log/printer/SeggerTerms.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

constexpr std::string_view before
  = "<?xml version=\"1.0\"?>\n<SEGGER>\n  <Software>\n    <SEGGER>\n"
    "      <J-Link StayOnTop=\"0x00000001\" LicenseEDU_DontShowAgainToday=\"0x07EA0802\" "
    "LicenseEDU_DontShowAgainToday_SEGGERRegType=\"SYS_REG_TYPE_DWORD\">\n"
    "        <DontShowAgain LicenseEDU_DontShowAgainToday=\"0x07ea0802\" "
    "LicenseEDU_DontShowAgainToday_SEGGERRegType=\"SYS_REG_TYPE_DWORD\"/>\n"
    "      </J-Link>\n    </SEGGER>\n  </Software>\n</SEGGER>\n";

std::string read(std::filesystem::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

void write(std::filesystem::path const& p,
           std::string const&           text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
}

// noon local time on a fixed day (packed 0x07EA0A0B)
std::chrono::system_clock::time_point fixedDay() {
    std::tm t{};
    t.tm_year  = 2026 - 1900;
    t.tm_mon   = 9;
    t.tm_mday  = 11;
    t.tm_hour  = 12;
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

std::chrono::system_clock::time_point fixedDayAt(int hour,
                                                 int min,
                                                 int sec) {
    std::tm t{};
    t.tm_year  = 2026 - 1900;
    t.tm_mon   = 9;
    t.tm_mday  = 11;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

// A printer running across local midnight (a reconnect hung on the dialog at 00:00:07): the date written before a
// connect is the day the DLL checks on.
void acrossMidnight(std::filesystem::path const& file) {
    namespace st = uc_log::printer::segger_terms;
    using namespace std::chrono_literals;
    write(file, std::string{before});
    auto const midnight = fixedDayAt(24, 0, 0);   // mktime normalises: the next day, 00:00:00 local
    CHECK(st::waitForNewDay(fixedDayAt(12, 0, 0)) == 0s, "noon: no wait");
    CHECK(st::waitForNewDay(midnight - st::midnightMargin - 1s) == 0s,
          "before the margin: no wait");
    auto const late = midnight - 100ms;   // a connect starting at 23:59:59.9
    auto const wait = st::waitForNewDay(late);
    CHECK(wait == 1100ms, "23:59:59.9: waits until a second past midnight");
    CHECK(st::waitForNewDay(midnight - st::midnightMargin + 1s) == st::midnightMargin,
          "in the margin: waits");
    CHECK(st::acceptToday(file, late + wait) == st::Result::accepted
            && read(file).contains("0x07EA0A0C"),
          "after the wait: the new day written");
    CHECK(st::waitForNewDay(midnight + 1s) == 0s, "00:00:01: no wait");
    write(file, std::string{before});
    CHECK(st::acceptToday(file, fixedDayAt(23, 0, 0)) == st::Result::accepted,
          "accepted the evening before");
    CHECK(st::acceptToday(file, midnight + 1s) == st::Result::accepted
            && read(file).contains("0x07EA0A0C") && !read(file).contains("0x07EA0A0B"),
          "a reconnect at 00:00:01 writes the new day, in both places");
}
}   // namespace

int main() {
    namespace st = uc_log::printer::segger_terms;
    CHECK(st::packedDate(fixedDay()) == 0x07EA'0A0BU, "the date packed: 2026 << 16 | 10 << 8 | 11");

    auto const dir
      = std::filesystem::temp_directory_path() / fmt::format("segger_terms_{}", ::getpid());
    std::filesystem::create_directories(dir);
    auto const file = dir / "SEGGER_REG_HKEY_CURRENT_USER.xml";
    write(file, std::string{before});
    CHECK(st::acceptToday(file, fixedDay()) == st::Result::accepted, "accepted");
    std::string       expected{before};
    std::string const today{"LicenseEDU_DontShowAgainToday=\"0x07EA0A0B\""};
    expected.replace(expected.find("LicenseEDU_DontShowAgainToday=\"0x07EA0802\""),
                     today.size(),
                     today);
    expected.replace(expected.find("LicenseEDU_DontShowAgainToday=\"0x07ea0802\""),
                     today.size(),
                     today);
    CHECK(read(file) == expected, "both places written, every other byte unchanged");
    CHECK(st::acceptToday(file, fixedDay()) == st::Result::alreadyToday,
          "a second connect the same day: nothing");
    CHECK(read(file) == expected, "and the file stays");

    std::string noKey{before};
    for(auto at = noKey.find("LicenseEDU"); at != std::string::npos; at = noKey.find("LicenseEDU"))
    {
        noKey.replace(at, 10, "OtherThing");
    }
    write(file, noKey);
    CHECK(st::acceptToday(file, fixedDay()) == st::Result::noKey && read(file) == noKey,
          "no key: said, the file untouched");
    acrossMidnight(file);
    std::filesystem::remove(file);
    CHECK(st::acceptToday(file, fixedDay()) == st::Result::noFile && !std::filesystem::exists(file),
          "no file: said, none created");
    std::filesystem::remove_all(dir);

    return uc_log::test::report();
}
