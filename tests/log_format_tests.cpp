// The .rttlog row format (LogFormat.hpp): RFC 4180 CSV, one physical line per record, and the
// escapes decode back to the exact message. Writes the rows it checks to <dir>/format_sample.rttlog
// and the fields a CSV reader must see to <dir>/format_sample.expected, for
// test_rttlog_format.py, which reads them with Python's csv module like the bench scripts do.
#include "uc_log/detail/LogFormat.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

namespace {
// RFC 4180: fields split at commas, a quoted field may hold commas and "" for a quote
std::optional<std::vector<std::string>> splitCsv(std::string_view line) {
    std::vector<std::string> fields;
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
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    if(quoted) { return std::nullopt; }
    fields.push_back(std::move(field));
    return fields;
}

void appendUtf8(std::string&  out,
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

// the escapes fmt's {:?} writes: \t \n \r \\ \" \' \xHH \u{H...}
std::optional<std::string> unescape(std::string_view s) {
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
            if(i + 2 >= s.size()) { return std::nullopt; }
            out.push_back(
              static_cast<char>(std::stoi(std::string{s.substr(i + 1, 2)}, nullptr, 16)));
            i += 2;
            break;
        case 'u':
            {
                auto const close = s.find('}', i);
                if(i + 1 >= s.size() || s[i + 1] != '{' || close == std::string_view::npos) {
                    return std::nullopt;
                }
                appendUtf8(out,
                           static_cast<std::uint32_t>(
                             std::stoul(std::string{s.substr(i + 2, close - i - 2)}, nullptr, 16)));
                i = close;
                break;
            }
        default: return std::nullopt;
        }
    }
    return out;
}

struct Case {
    std::string message;
    std::string function;
};

std::vector<Case> cases() {
    return {
      {                              "plain",                                                          "f"},
      {                                   "",                                                          "f"},
      {                        "0.96\" oled",                                                          "f"},
      {          "\"quoted\" at both ends\"",                                                          "f"},
      {                            "a, b, c",                                                          "f"},
      {              "backslash \\ and \\\\",                                                          "f"},
      {                 "a \\\" in the text",                                                          "f"}, // a backslash followed by a quote
      {                 "line one\nline two",                                                          "f"},
      {                     "cr\r and tab\t",                                                          "f"},
      {                "escape \x1b[31m red",                                                          "f"},
      {"unit 23.5 \xe2\x84\x83, rate 3 mK/s",                                                          "f"}, // U+2103
      {             "emoji \xf0\x9f\x93\x9c",                                                          "f"}, // U+1F4DC
      {         "invalid utf-8 \xff\xfe end",                                                          "f"},
      {              "trailing backslash \\",                                                          "f"},
      {                            "message", "BusScan<Bus<>, SizedCatalogue<>, AddressProbe>::operator()"},
    };
}
}   // namespace

int main(int    argc,
         char** argv) {
    std::string const dir = argc > 1 ? *std::next(argv, 1) : ".";

    std::ostringstream text;
    uc_log::detail::logformat::writeHeader(text);
    auto const all = cases();
    for(std::size_t i = 0; i != all.size(); ++i) {
        uc_log::detail::LogEntry entry{i % 4, {}};
        entry.fileName     = "dir, with comma/main.cpp";
        entry.line         = 100 + i;
        entry.functionName = all[i].function;
        entry.logLevel     = uc_log::LogLevel::info;
        entry.logMsg       = all[i].message;
        entry.module       = "bench.\"module\"";
        uc_log::detail::logformat::writeEntry(text, {}, entry);
    }
    std::string const file = text.str();

    // one physical line per record
    std::vector<std::string_view> lines;
    for(std::size_t start = 0; start < file.size();) {
        auto const end = file.find('\n', start);
        lines.push_back(std::string_view{file}.substr(start, end - start));
        start = end + 1;
    }
    CHECK(lines.size() == all.size() + 1, "one line per record, plus the header");

    std::ofstream expected{dir + "/format_sample.expected"};
    for(std::size_t i = 0; i + 1 < lines.size() && i < all.size(); ++i) {
        auto const fields = splitCsv(lines[i + 1]);
        CHECK(fields && fields->size() == 9, "9 CSV fields");
        if(!fields || fields->size() != 9) {
            std::printf("  row %zu: %.*s\n",
                        i,
                        static_cast<int>(lines[i + 1].size()),
                        lines[i + 1].data());
            continue;
        }
        auto const message = unescape((*fields)[7]);
        CHECK(message && *message == all[i].message, "the message decodes back exactly");
        CHECK(unescape((*fields)[4]) == all[i].function, "the function decodes back exactly");
        CHECK(unescape((*fields)[2]) == "dir, with comma/main.cpp",
              "the file decodes back exactly");
        CHECK(unescape((*fields)[8]) == "bench.\"module\"", "the module decodes back exactly");
        CHECK((*fields)[1] == std::to_string(i % 4) && (*fields)[3] == std::to_string(100 + i),
              "channel and line");
        // what a CSV reader must see in each text column, one per line
        expected << (*fields)[2] << '\n'
                 << (*fields)[4] << '\n'
                 << (*fields)[7] << '\n'
                 << (*fields)[8] << '\n';
    }
    std::ofstream{dir + "/format_sample.rttlog", std::ios::binary} << file;

    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
