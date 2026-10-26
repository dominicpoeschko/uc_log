#pragma once

// The rows the printer adds to the log itself, module uc_log, level warn, on a channel no RTT channel has: whoever
// reads the log (the stress checker, a person) finds the event where it happened.

#include "uc_log/LogLevel.hpp"
#include "uc_log/printer/LogEntry.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>

namespace uc_log::printer {

// the channel of the printer's own log rows: no RTT channel has this number
inline constexpr std::size_t unreadBootsChannel = 0xFFFF;

// A row of the printer's own: its channel, target time 0 (it goes out ahead of the next boot's lines), file
// "uc_log_printer", function "printer", module uc_log, level warn.
inline printer::LogEntry printerRow(std::string text) {
    printer::LogEntry entry{unreadBootsChannel, std::string_view{}};
    entry.fileName     = "uc_log_printer";
    entry.functionName = "printer";
    entry.logLevel     = LogLevel::warn;
    entry.module       = "uc_log";
    entry.logMsg       = std::move(text);
    entry.parsedOk     = true;
    return entry;
}

// The count of boots that logged starts again (the printer's own download wrote over the target's RAM): the next
// boot's number is no continuation of the last one's.
inline printer::LogEntry bootCountRestartRow() {
    return printerRow(
      "⚠ the boot count starts again: the printer's download wrote over the target's RAM");
}

// Boots that logged and were replaced unread (printer/BootLedger.hpp).
inline printer::LogEntry unreadBootsRow(std::uint32_t unread,
                                        std::uint32_t from,
                                        std::uint32_t to) {
    return printerRow(fmt::format("⚠ {} boot{} passed unread (boots that logged {} -> {})",
                                  unread,
                                  unread == 1 ? "" : "s",
                                  from,
                                  to));
}

}   // namespace uc_log::printer
