#pragma once

// Reading a .rttlog back (LogFormat.hpp writes it): one RFC 4180 CSV row per physical line, nine columns, the text
// columns escaped as fmt's {:?} does (\n, \\, \xHH, \u{H...}). The one reader for the host's tools and tests (the
// stress checker, log_format_tests); Python scripts read the same file with uc_log_client.rttlog_rows.

#include "uc_log/detail/Lifetimebound.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rttlog {

// The columns, in the order of LogFormat.hpp's header line.
enum class Column : std::size_t {
    recvTime,
    channel,
    file,
    line,
    function,
    level,
    ucTime,
    message,
    module
};
inline constexpr std::size_t columnCount = 9;

// The header line (without its newline) is not a row.
inline bool isHeader(std::string_view line) { return line.starts_with("recv_time_utc,"); }

// The fields of one row: split at commas, a quoted field may hold commas and "" for a quote. nullopt for a line
// that is no RFC 4180 row (a quote inside an unquoted field, an unterminated quote). A '\r' before the end is the
// line end of a CRLF file, not part of the last field.
inline std::optional<std::vector<std::string>> fields(std::string_view line) {
    if(line.ends_with('\r')) { line.remove_suffix(1); }
    std::vector<std::string> out;
    std::string              field;
    bool                     quoted = false;
    for(std::size_t i = 0; i < line.size(); ++i) {
        char const c = line[i];
        if(quoted) {
            if(c == '"') {
                if(i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if(c == '"') {
            if(!field.empty()) { return std::nullopt; }   // a quote inside an unquoted field
            quoted = true;
        } else if(c == ',') {
            out.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    if(quoted) { return std::nullopt; }
    out.push_back(std::move(field));
    return out;
}

// One column of a row that has all of them.
inline std::string const& at(std::vector<std::string> const& row UC_LOG_LIFETIMEBOUND,
                             Column                              column) {
    return row[static_cast<std::size_t>(column)];
}

namespace escapes {
    inline void appendUtf8(std::string&  out,
                           std::uint32_t cp) {
        if(cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if(cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if(cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    inline std::optional<std::uint32_t> hex(std::string_view digits) {
        std::uint32_t value{};
        auto const [ptr, ec] = std::from_chars(std::to_address(digits.begin()),
                                               std::to_address(digits.end()),
                                               value,
                                               16);
        if(digits.empty() || ec != std::errc{} || ptr != std::to_address(digits.end())) {
            return std::nullopt;
        }
        return value;
    }
}   // namespace escapes

// A text column's exact bytes: the escapes fmt's {:?} writes (\t \n \r \\ \" \' \xHH \u{H...}) undone. nullopt for
// an escape fmt does not write.
inline std::optional<std::string> unescape(std::string_view s) {
    std::string out;
    for(std::size_t i = 0; i < s.size(); ++i) {
        if(s[i] != '\\') {
            out.push_back(s[i]);
            continue;
        }
        if(++i == s.size()) { return std::nullopt; }
        switch(s[i]) {
        case 't':  out.push_back('\t'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case '\\': out.push_back('\\'); break;
        case '"':  out.push_back('"'); break;
        case '\'': out.push_back('\''); break;
        case 'x':
            {
                auto const v = i + 2 < s.size() ? escapes::hex(s.substr(i + 1, 2)) : std::nullopt;
                if(!v) { return std::nullopt; }
                out.push_back(static_cast<char>(*v));
                i += 2;
                break;
            }
        case 'u':
            {
                auto const close = s.find('}', i);
                if(i + 1 >= s.size() || s[i + 1] != '{' || close == std::string_view::npos) {
                    return std::nullopt;
                }
                auto const v = escapes::hex(s.substr(i + 2, close - i - 2));
                if(!v) { return std::nullopt; }
                escapes::appendUtf8(out, *v);
                i = close;
                break;
            }
        default: return std::nullopt;
        }
    }
    return out;
}

}   // namespace uc_log::printer::rttlog
