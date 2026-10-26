// The .rttlog row format (LogFormat.hpp): RFC 4180 CSV, one physical line per record, and the
// escapes decode back to the exact message. Writes the rows it checks to <dir>/format_sample.rttlog
// and the fields a CSV reader must see to <dir>/format_sample.expected, for
// test_rttlog_format.py, which reads them with Python's csv module as a .rttlog script does.
#include "support/Check.hpp"
#include "uc_log/printer/LogFormat.hpp"
#include "uc_log/printer/RttlogRows.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using uc_log::printer::rttlog::unescape;

struct Case {
    std::string message;
    std::string function;
};

std::vector<Case> cases() {
    return {
      {                              "plain",                                                          "f"},
      {                                   "",                                                          "f"},
      {                        "0.96\" oled",                                                          "f"},
      {          R"("quoted" at both ends")",                                                          "f"},
      {                            "a, b, c",                                                          "f"},
      {              R"(backslash \ and \\)",                                                          "f"},
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
    uc_log::printer::logformat::writeHeader(text);
    auto const all = cases();
    for(std::size_t i = 0; i != all.size(); ++i) {
        uc_log::printer::LogEntry entry{i % 4, {}};
        entry.fileName     = "dir, with comma/main.cpp";
        entry.line         = 100 + i;
        entry.functionName = all[i].function;
        entry.logLevel     = uc_log::LogLevel::info;
        entry.logMsg       = all[i].message;
        entry.module       = "bench.\"module\"";
        uc_log::printer::logformat::writeEntry(text, {}, entry);
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
        auto const fields = uc_log::printer::rttlog::fields(lines[i + 1]);
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

    return uc_log::test::report();
}
