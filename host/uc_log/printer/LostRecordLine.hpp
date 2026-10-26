#pragma once

// N, when a log message is the lost-record line (uc_log/detail/LostRecord.hpp, the text the firmware writes):
// "<sign> N log records lost on this channel ...", with either sign - "⚠" from an image with the catalog, "!" from an
// inline string. The text only: who wrote it (the site, the module) is the caller's check. The printer's
// lostRecordCount (TargetLoss.hpp) and the stress checker (stress/LogStressCheck.hpp) both read it here.

#include "uc_log/detail/LostRecord.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace uc_log::printer {

inline std::optional<std::uint64_t> lostRecordNumber(std::string_view message) {
    namespace lost = uc_log::detail::lost_record;
    for(std::string_view const sign : {lost::catalogSign, lost::inlineSign}) {
        if(!message.starts_with(sign) || !message.substr(sign.size()).starts_with(' ')) {
            continue;
        }
        std::string_view rest = message.substr(sign.size() + 1);
        std::uint64_t    n{};
        auto const [ptr, ec] = std::from_chars(rest.begin(), rest.end(), n);
        if(ec != std::errc{} || ptr == rest.begin()) { return std::nullopt; }
        rest.remove_prefix(static_cast<std::size_t>(ptr - rest.begin()));
        if(!rest.starts_with(' ') || !rest.substr(1).starts_with(lost::phrase)) {
            return std::nullopt;
        }
        return n;
    }
    return std::nullopt;
}

}   // namespace uc_log::printer
