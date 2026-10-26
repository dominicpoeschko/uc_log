#pragma once

// The lost-record line's text, shared by the firmware that writes it (RecordFrame<>::reportLost, uc_log.hpp) and the
// host code that counts it (printer/LostRecordLine.hpp: the printer's lostRecordCount and the stress checker):
//   "<sign> N log records lost on this channel (ring full, or no debugger)"
// The sign is "⚠" in an image with the catalog; an inline string (no catalog) is ASCII only, so there it is "!".

#include <string_view>

namespace uc_log::detail::lost_record {
inline constexpr std::string_view catalogSign{"⚠"};
inline constexpr std::string_view inlineSign{"!"};
inline constexpr std::string_view phrase{"log records lost on this channel"};
inline constexpr std::string_view cause{"(ring full, or no debugger)"};

constexpr std::string_view sign(bool const catalog) { return catalog ? catalogSign : inlineSign; }
}   // namespace uc_log::detail::lost_record
