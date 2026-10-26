// printer/MessageLog.hpp: the levels' wire names, the counts, the format-error prefixes, the history and stream, the
// .status.log rows and the stderr echo, observers called outside the log's lock, and a halt's kind and struct.
#include "support/Check.hpp"
#include "uc_log/printer/MessageLog.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using uc_log::printer::HaltCapture;
using uc_log::printer::MessageKind;
using uc_log::printer::MessageLevel;
using uc_log::printer::MessageLog;

namespace {

void wireNames() {
    using uc_log::printer::wireName;
    CHECK(wireName(MessageLevel::fatal) == "fatal" && wireName(MessageLevel::error) == "error"
            && wireName(MessageLevel::status) == "status"
            && wireName(MessageLevel::toolError) == "tool error"
            && wireName(MessageLevel::tool) == "tool",
          "the names scripts match (kvasir_bench: level \"error\" + \"core halted:\")");
}

void counts() {
    MessageLog log;
    log.status("hello");
    log.tool("a tool line");
    CHECK(log.counts().errors == 0 && log.totalErrors() == 0, "status and tool are no errors");
    // the kind counts, not the words: a render error of any text is one, a plain error with the old prefix is none
    log.formatError("bad format for replacement field 3");
    log.formatError("bad style: invalid color");
    log.formatError("time value out of range (ratio: 1/1, value: 1e+300)");
    log.error("bad format for replacement field 4 (said as a plain error)");
    log.fatal("no map");
    log.toolError("control server: gone");
    auto const c = log.counts();
    CHECK(c.errors == 6 && c.formatErrors == 3, "errors and format errors counted");
    CHECK(log.totalErrors() == 6, "and the control socket's count");
    log.resetFormatErrors();
    CHECK(log.counts().formatErrors == 0 && log.counts().errors == 6,
          "a flash resets the format errors only");
    log.clear();
    CHECK(log.counts().errors == 0 && log.totalErrors() == 6,
          "Clear empties the tab, not the socket's count");
}

void historyAndStream() {
    MessageLog                                           log;
    std::vector<uc_log::printer::control::StatusMessage> streamed;
    log.setStream([&](uc_log::printer::control::StatusMessage const& m) { streamed.push_back(m); });
    for(std::size_t i = 0; i != MessageLog::historyKept + 5; ++i) {
        log.status("line " + std::to_string(i));
    }
    log.error("two\nlines");
    auto const newest = log.newest(2);
    CHECK(newest.size() == 2 && newest[1].level == "error" && newest[1].text == "two lines"
            && newest[0].text == "line 1004",
          "the newest, a newline flattened");
    CHECK(log.newest(10'000).size() == MessageLog::historyKept, "the history keeps 1000");
    CHECK(streamed.size() == MessageLog::historyKept + 6, "every message streamed");

    std::vector<MessageLog::Entry> mirror;
    std::size_t                    clears = 0;
    log.copyNew(mirror, clears);
    CHECK(mirror.size() == MessageLog::historyKept + 6
            && mirror.back().level == MessageLevel::error,
          "the Status tab's mirror");
    log.status("more");
    log.copyNew(mirror, clears);
    CHECK(mirror.size() == MessageLog::historyKept + 7 && mirror.back().text == "more",
          "only the new ones added");
    log.clear();
    log.status("after");
    log.copyNew(mirror, clears);
    CHECK(mirror.size() == 1 && mirror[0].text == "after", "a clear starts the mirror over");
}

void statusFile() {
    auto const path = std::filesystem::temp_directory_path()
                    / ("uc_log_message_log_" + std::to_string(::getpid()) + ".status.log");
    {
        MessageLog log;
        log.openStatusFile(path);
        log.status("firmware matches build");
        log.toolError("probe gone");
    }
    std::ifstream            in{path};
    std::vector<std::string> lines;
    for(std::string line; std::getline(in, line);) { lines.push_back(line); }
    CHECK(lines.size() == 2 && lines[0].ends_with(" [status] firmware matches build")
            && lines[1].ends_with(" [tool error] probe gone") && lines[0].find('T') == 10,
          "\"<iso time> [level] text\" rows");
    std::filesystem::remove(path);
}

void observersOutsideTheLock() {
    MessageLog                log;
    std::vector<MessageLevel> seen;
    bool                      sayAgain = true;
    log.addObserver([&](MessageLog::Said const& said) {
        seen.push_back(said.level);
        // the log's lock is free: the observer may ask it (the Gui does) or say something itself
        static_cast<void>(log.counts());
        if(sayAgain && said.text == "core halted: pc=0x10000100") {
            sayAgain = false;
            log.status("noted");
        }
    });
    log.error("core halted: pc=0x10000100");
    CHECK((seen == std::vector{MessageLevel::error, MessageLevel::status}),
          "observers see every message");
}

// A halt is an error whose text scripts read, and a struct the front ends take: the kind, never the text, says which.
void haltKind() {
    MessageLog                                         log;
    std::vector<std::pair<MessageKind, std::uint32_t>> seen;   // kind, pc of the struct (0: none)
    log.addObserver([&](MessageLog::Said const& said) {
        seen.emplace_back(said.kind, said.halt != nullptr ? said.halt->pc : 0U);
    });
    log.error("core halted: pc=0x10000100 (a plain error with the halt's words)");
    HaltCapture const capture{
      .pc    = 0x1000'0100U,
      .sp    = 0x2000'0000U,
      .stack = {0x1U, std::nullopt}
    };
    log.halt(capture);
    CHECK((seen
           == std::vector<std::pair<MessageKind, std::uint32_t>>{
             {MessageKind::plain, 0U}, {MessageKind::halt, 0x1000'0100U}}),
          "the kind comes with the message, the struct only with a halt");
    std::vector<MessageLog::Entry> mirror;
    std::size_t                    clears{};
    log.copyNew(mirror, clears);
    CHECK(mirror.size() == 2 && mirror[1].kind == MessageKind::halt
            && mirror[1].level == MessageLevel::error && mirror[1].text == capture.text()
            && log.counts().errors == 2,
          "kept as an error with the struct's text");
    CHECK(log.newest(1).front().text == capture.text(), "the control socket sees the text");
    CHECK(log.newest(2).front().kind == std::nullopt
            && log.newest(1).front().kind == uc_log::printer::control::MessageKind::halt,
          "and the kind: absent for a plain message, halt for the halt");
}

// A line lost to its format: an error of its own kind, on the socket as format_error.
void formatErrorKind() {
    MessageLog               log;
    std::vector<MessageKind> seen;
    log.addObserver([&](MessageLog::Said const& said) { seen.push_back(said.kind); });
    log.formatError("bad style: x");
    CHECK(seen == std::vector<MessageKind>{MessageKind::formatError}, "the observer gets the kind");
    CHECK(log.newest(1).front().kind == uc_log::printer::control::MessageKind::formatError
            && log.newest(1).front().level == "error" && log.counts().formatErrors == 1,
          "an error, format_error on the socket, counted");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {              "wireNames",               wireNames},
                               {                 "counts",                  counts},
                               {       "historyAndStream",        historyAndStream},
                               {             "statusFile",              statusFile},
                               {"observersOutsideTheLock", observersOutsideTheLock},
                               {               "haltKind",                haltKind},
                               {        "formatErrorKind",         formatErrorKind},
    });
}
