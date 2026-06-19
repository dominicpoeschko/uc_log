#pragma once
#include "uc_log/detail/LogEntry.hpp"

#include <chrono>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <ostream>

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

inline void writeHeader(std::ostream& out) {
    fmt::print(out, "recv_time_utc,channel,file,line,function,log_level,uc_time,message\n");
}

inline void writeEntry(std::ostream&                         out,
                       std::chrono::system_clock::time_point recv_time,
                       uc_log::detail::LogEntry const&       entry) {
    fmt::print(out,
               "{},{},{:?},{},{:?},{:#},{},{:?}\n",
               toIso8601Utc(recv_time),
               entry.channel.channel,
               entry.fileName,
               entry.line,
               entry.functionName,
               entry.logLevel,
               entry.ucTime.time,
               entry.logMsg);
}

}   // namespace uc_log::detail::logformat
