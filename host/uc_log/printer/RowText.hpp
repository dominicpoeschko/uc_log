#pragma once

// A log line as plain text, as the Logs tab exports it (.txt) and yanks it to the clipboard:
// "<recv time> <channel> <uc time> <level>| <text> ([module] <file>:<line> <function>)". The export pads the level
// to 5 columns, the yank does not.

#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace uc_log::printer {

struct RowFields {
    std::string_view recvTime;
    std::size_t      channel{};
    std::string_view ucTime;
    std::string_view level;
    std::string_view text;
    std::string_view modulePrefix;   // "[module] " or empty
    std::string_view file;
    std::size_t      line{};
    std::string_view function;
};

inline std::string plainRow(RowFields const& f,
                            bool             padLevel) {
    return padLevel ? fmt::format("{} {} {} {:<5}| {} ({}{}:{} {})",
                                  f.recvTime,
                                  f.channel,
                                  f.ucTime,
                                  f.level,
                                  f.text,
                                  f.modulePrefix,
                                  f.file,
                                  f.line,
                                  f.function)
                    : fmt::format("{} {} {} {}| {} ({}{}:{} {})",
                                  f.recvTime,
                                  f.channel,
                                  f.ucTime,
                                  f.level,
                                  f.text,
                                  f.modulePrefix,
                                  f.file,
                                  f.line,
                                  f.function);
}

}   // namespace uc_log::printer
