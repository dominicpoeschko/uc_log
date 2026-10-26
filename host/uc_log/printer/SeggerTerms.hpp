#pragma once

// SEGGER's daily terms-of-use notice for J-Link EDU probes: once a calendar day the DLL shows a dialog (through
// JLinkGUIServerExe) and every connect waits on it - after "ConfigTargetSettings() end" nothing more happens, which
// looks like a dead probe; replugging or power-cycling does not clear it. The answer "do not show this again today"
// is kept in SEGGER's registry file as a packed date, (year << 16) | (month << 8) | day, under the key
// LicenseEDU_DontShowAgainToday - on the <J-Link> element and on its <DontShowAgain> child (the one the DLL reads).
// Writing today's date there before a connect is what ticking the box does, so the dialog never opens. The key is
// the EDU notice's only: other probes do not show it.
//
// The day is the local calendar day (localtime, as the DLL in the same process sees it). Around local midnight a date
// written just before it is yesterday's by the time the DLL checks: a running printer's reconnect then hung on the
// dialog until someone clicked it. So a connect that would start in the last midnightMargin of a day waits until the
// new day has begun and writes that one (waitForNewDay).

#include "uc_log/printer/Environment.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace uc_log::printer::segger_terms {

inline constexpr std::string_view key{"LicenseEDU_DontShowAgainToday"};

/// The registry file of the user running the printer.
inline std::filesystem::path registryFile() {
    char const* const home = environment("HOME");
    return home == nullptr
           ? std::filesystem::path{}
           : std::filesystem::path{home} / ".config/SEGGER/SEGGER_REG_HKEY_CURRENT_USER.xml";
}

/// (year << 16) | (month << 8) | day of the local calendar day `now` falls on.
inline std::uint32_t packedDate(std::chrono::system_clock::time_point now) {
    auto const t = std::chrono::system_clock::to_time_t(now);
    std::tm    local{};
    localtime_r(&t, &local);
    return (static_cast<std::uint32_t>(local.tm_year + 1900) << 16U)
         | (static_cast<std::uint32_t>(local.tm_mon + 1) << 8U)
         | static_cast<std::uint32_t>(local.tm_mday);
}

/// Every Key="0x<hex>" in `xml` set to `date`; the rest of the text untouched. Returns how many there were.
inline std::size_t setAcceptedDate(std::string&  xml,
                                   std::uint32_t date) {
    std::string const prefix = fmt::format("{}=\"0x", key);
    std::string const digits = fmt::format("{:08X}", date);
    std::size_t       count  = 0;
    for(auto at = xml.find(prefix); at != std::string::npos; at = xml.find(prefix, at + 1)) {
        auto const from = at + prefix.size();
        auto const end  = xml.find_first_not_of("0123456789abcdefABCDEF", from);
        if(end == std::string::npos || end == from || xml[end] != '"') { continue; }
        xml.replace(from, end - from, digits);
        ++count;
    }
    return count;
}

/// How long before local midnight a connect waits for the new day: more than the DLL takes from JLINK_OpenEx to its
/// date check (a connect, ~1-2 s, longer when the probe re-enumerates). Costs at most this once a day, for any probe.
inline constexpr std::chrono::seconds midnightMargin{30};

/// How long a connect starting at `now` waits before the acceptance is written: zero, or - in the last midnightMargin
/// of a local day - until a second past its midnight.
inline std::chrono::system_clock::duration
waitForNewDay(std::chrono::system_clock::time_point const now) {
    auto const later = now + midnightMargin;
    if(packedDate(later) == packedDate(now)) { return {}; }
    auto const t = std::chrono::system_clock::to_time_t(later);
    std::tm    midnight{};
    localtime_r(&t, &midnight);
    midnight.tm_hour  = 0;
    midnight.tm_min   = 0;
    midnight.tm_sec   = 0;
    midnight.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&midnight)) + std::chrono::seconds{1}
         - now;
}

enum class Result { accepted, alreadyToday, noFile, noKey, unwritable };

/// Today's acceptance written into `file` (only when it is not there already).
inline Result acceptToday(std::filesystem::path const&          file,
                          std::chrono::system_clock::time_point now
                          = std::chrono::system_clock::now()) {
    std::string xml;
    {
        std::ifstream in{file, std::ios::binary};
        if(!in) { return Result::noFile; }
        xml.assign(std::istreambuf_iterator<char>{in}, {});
    }
    std::string updated = xml;
    if(setAcceptedDate(updated, packedDate(now)) == 0) { return Result::noKey; }
    if(updated == xml) { return Result::alreadyToday; }
    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    out << updated;
    return out ? Result::accepted : Result::unwritable;
}

}   // namespace uc_log::printer::segger_terms
