#pragma once

// The status line's badges, without the window: what the printer, the link, the build and the store report, each as a
// short text, a tone and the command a click runs (the page that says more); the window only colours a tone. A badge
// that counts trouble is "bad" from the first one on, a firmware that does not count its lost records shows "-", a
// trim or a running refilter shows only while it is there.

#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/Tone.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer {

struct Badge {
    std::string            text;
    Tone                   tone{Tone::plain};
    bool                   strong{false};   // drawn bold: trouble to look at now
    std::optional<Command> command{};       // what a click on it does

    bool operator==(Badge const&) const = default;
};

/// 999, 12K, 3M
inline std::string shortCount(std::uint64_t n) {
    if(n >= 1'000'000) { return fmt::format("{}M", n / 1'000'000); }
    if(n >= 1000) { return fmt::format("{}K", n / 1000); }
    return std::to_string(n);
}

inline std::string shortBytes(std::uint64_t bytes) {
    auto const b = static_cast<double>(bytes);
    if(bytes >= 1024ULL * 1024 * 1024) {
        return fmt::format("{:.1f}GB", b / (1024.0 * 1024 * 1024));
    }
    if(bytes >= 1024ULL * 1024) { return fmt::format("{:.1f}MB", b / (1024.0 * 1024)); }
    if(bytes >= 1024) { return fmt::format("{:.1f}KB", b / 1024.0); }
    return fmt::format("{}B", bytes);
}

struct StatusFacts {
    TargetPort::LinkStatus       link;
    bool                         flashing{};
    bool                         halted{};
    BuildStatus                  build{BuildStatus::idle};
    std::size_t                  logs{};        // original lines retained
    std::size_t                  shownLogs{};   // ... passing the filter
    std::size_t                  trimmedLogs{};
    bool                         filterActive{};
    std::string                  search;
    std::optional<unsigned>      refilterPercent;   // a refilter runs
    std::size_t                  formatErrors{};
    std::size_t                  errors{};
    std::optional<std::uint64_t> targetLost;   // nullopt: the firmware does not count
    control::FirmwareState       firmware{control::FirmwareState::unchecked};
    std::size_t                  firmwareMismatches{};
};

inline std::vector<Badge> statusBadges(StatusFacts const& f) {
    std::vector<Badge> b;
    bool const         linked = f.link.isRunning != 0;
    b.push_back({linked ? "link ●" : "link ○", linked ? Tone::ok : Tone::plain});
    if(f.halted) { b.push_back({"⏸ HALTED", Tone::bad, true, Command::messages}); }
    if(f.flashing) {
        b.push_back({"⚡ flashing", linked ? Tone::warn : Tone::bad, true, Command::messages});
    }
    switch(f.build) {
    case BuildStatus::idle: break;
    case BuildStatus::running:
        b.push_back({"build ●", Tone::warn, false, Command::buildOutput});
        break;
    case BuildStatus::success:
        b.push_back({"build ✔", Tone::ok, false, Command::buildOutput});
        break;
    case BuildStatus::failed:
        b.push_back({"build ✘", Tone::bad, true, Command::buildOutput});
        break;
    }
    b.push_back({f.filterActive
                   ? fmt::format("LOGS {}/{}", shortCount(f.shownLogs), shortCount(f.logs))
                   : fmt::format("LOGS {}", shortCount(f.logs)),
                 Tone::info,
                 false,
                 Command::filter});
    if(!f.search.empty()) {
        b.push_back({fmt::format("🔎 {:?}", f.search), Tone::warn, false, Command::search});
    }
    if(f.refilterPercent) { b.push_back({fmt::format("⏳ {}%", *f.refilterPercent), Tone::warn}); }
    if(f.trimmedLogs > 0) {
        b.push_back({fmt::format("♻ {}", shortCount(f.trimmedLogs)), Tone::warn});
    }
    b.push_back({fmt::format("DATA {}", shortBytes(f.link.numBytesRead)), Tone::plain});
    auto const overflows
      = static_cast<std::uint64_t>(f.link.hostOverflowCount < 0 ? 0 : f.link.hostOverflowCount);
    b.push_back({fmt::format("OVFL {}", shortCount(overflows)),
                 overflows == 0 ? Tone::plain : Tone::bad,
                 overflows != 0,
                 Command::messages});
    if(f.formatErrors > 0) {
        b.push_back(
          {fmt::format("📝 ✘ {}", shortCount(f.formatErrors)), Tone::bad, true, Command::messages});
    }
    b.push_back({fmt::format("⚠ {}", shortCount(f.errors)),
                 f.errors == 0 ? Tone::plain : Tone::bad,
                 f.errors != 0,
                 Command::messages});
    if(!f.targetLost) {
        b.push_back({"📉 -", Tone::plain});
    } else {
        b.push_back({fmt::format("📉 {}", shortCount(*f.targetLost)),
                     *f.targetLost == 0 ? Tone::plain : Tone::bad,
                     *f.targetLost != 0,
                     Command::messages});
    }
    auto const times
      = f.firmwareMismatches == 0 ? std::string{} : fmt::format(" ({}x)", f.firmwareMismatches);
    switch(f.firmware) {
    case control::FirmwareState::match:
        b.push_back({"FW ✔" + times,
                     f.firmwareMismatches == 0 ? Tone::ok : Tone::warn,
                     false,
                     Command::messages});
        break;
    case control::FirmwareState::different:
        b.push_back({"FW ✘ MISMATCH" + times, Tone::bad, true, Command::messages});
        break;
    case control::FirmwareState::unchecked:
        b.push_back({"FW ?" + times, Tone::plain, false, Command::messages});
        break;
    }
    return b;
}

}   // namespace uc_log::printer
