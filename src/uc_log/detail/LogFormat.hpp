#pragma once
#include "remote_fmt/fmt_wrapper.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <chrono>
#include <ostream>
#include <string>
#include <string_view>

namespace uc_log::detail::logformat {

inline std::string toIso8601Utc(std::chrono::system_clock::time_point tp) {
    auto const t   = std::chrono::system_clock::to_time_t(tp);
    auto const utc = fmt::gmtime(t);
    auto const sec = std::chrono::duration_cast<std::chrono::seconds>(
      tp - std::chrono::time_point_cast<std::chrono::minutes>(tp));
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      tp - std::chrono::time_point_cast<std::chrono::seconds>(tp));
    return fmt::format("{:%FT%H:%M}:{:02}.{:03}Z", utc, sec.count(), ms.count());
}

// An RFC 4180 field on one line: `"` doubled, everything else escaped as fmt's `{:?}` does.
inline std::string csvField(std::string_view text) {
    std::string const debug = fmt::format("{:?}", text);   // "...", C escapes
    std::string       out;
    out.reserve(debug.size() + 2);
    out.push_back('"');
    for(std::size_t i = 1; i + 1 < debug.size(); ++i) {
        if(debug[i] == '\\' && i + 2 < debug.size()) {
            if(debug[i + 1] == '"') {
                out += "\"\"";
            } else {
                out.push_back('\\');
                out.push_back(debug[i + 1]);
            }
            ++i;
        } else {
            out.push_back(debug[i]);
        }
    }
    out.push_back('"');
    return out;
}

// `module` is last: scripts index the first eight columns by position.
inline void writeHeader(std::ostream& out) {
    fmt::print(out, "recv_time_utc,channel,file,line,function,log_level,uc_time,message,module\n");
}

inline void writeEntry(std::ostream&                         out,
                       std::chrono::system_clock::time_point recv_time,
                       uc_log::detail::LogEntry const&       entry) {
    fmt::print(out,
               "{},{},{},{},{},{:#},{},{},{}\n",
               toIso8601Utc(recv_time),
               entry.channel.channel,
               csvField(entry.fileName),
               entry.line,
               csvField(entry.functionName),
               entry.logLevel,
               entry.ucTime.time,
               csvField(entry.logMsg),
               csvField(entry.module));
}

}   // namespace uc_log::detail::logformat
