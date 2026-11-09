// The front-end-neutral parts of a log window's panels and commands: the command table and its palette matching
// (Commands.hpp), the word match (TextMatch.hpp), where a click on the status line lands (StatusBadges.hpp), the
// panels' rows cut, padded and wrapped and the scrollbar (PanelRows.hpp), the messages, the build output, a line's
// details, the help, a line as plain text (PanelText.hpp), a typed target time and the line at it (TargetTime.hpp).
// No window, no clock.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/PanelText.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/TargetTime.hpp"
#include "uc_log/printer/TextMatch.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace uc_log::printer;
using namespace std::chrono_literals;

namespace {

struct CodePoints {
    std::size_t operator()(std::string_view text) const {
        std::size_t n = 0;
        for(char const c : text) {
            if((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) { ++n; }
        }
        return n;
    }
};

std::string joined(std::vector<ScreenRun> const& runs) {
    std::string s;
    for(auto const& r : runs) { s += r.text; }
    return s;
}

std::string joined(PanelRow const& row) {
    std::string s;
    for(auto const& p : row) { s += p.text; }
    return s;
}

LogEntry entry(std::string              msg,
               std::chrono::nanoseconds uc = 0ns) {
    LogEntry e{0, {}};
    e.logLevel     = uc_log::LogLevel::warn;
    e.fileName     = "main.cpp";
    e.line         = 7;
    e.logMsg       = std::move(msg);
    e.functionName = "fn";
    e.ucTime.time  = uc;
    e.parsedOk     = true;
    return e;
}

void commandKeys() {
    CHECK(commandForKey(U's') == Command::messages, "s: messages");
    CHECK(commandForKey(U'q') == Command::quit, "q: quit");
    CHECK(commandForKey(U'?') == Command::help, "?: help");
    CHECK(!commandForKey(U'x').has_value() && !commandForKey(0).has_value(),
          "no command on x, none on 0");
    auto const  all     = matchCommands("");
    std::size_t palette = 0;
    for(auto const& c : commandTable) { palette += c.palette ? 1 : 0; }
    CHECK(all.size() == palette, "an empty query offers every palette entry");
    CHECK(std::ranges::find(all, Command::lineUp) == all.end(),
          "moving a line is no palette entry");
}

void paletteMatching() {
    auto const flash = matchCommands("fla");
    CHECK(flash.size() == 2 && flash[0] == Command::flash && flash[1] == Command::buildAndFlash,
          "a name starting with the word first, then a word starting with it");
    auto const module = matchCommands("COL mod");
    CHECK(module.size() == 1 && module[0] == Command::toggleModule, "every word, any case");
    CHECK(matchCommands("zzz").empty(), "nothing matches: nothing offered");
}

void wordMatch() {
    CHECK((queryWords("  I2C  bus ") == std::vector<std::string>{"i2c", "bus"}),
          "the words, in lower case");
    CHECK(queryWords("   ").empty(), "only spaces: no word");
    CHECK(matchesWords("Kvasir::I2C::Bus", "bus i2c"), "every word, in any order, any case");
    CHECK(!matchesWords("Kvasir::I2C::Bus", "bus spi"), "a word it lacks: no match");
    CHECK(matchesWords("anything", ""), "an empty query matches everything");
    CHECK(asciiLower("ÄBC") == "Äbc", "ASCII only: other bytes stay as they are");
}

void badgeCommands() {
    StatusFacts facts;
    facts.errors = 3;
    facts.build  = BuildStatus::failed;
    bool errors  = false;
    bool build   = false;
    for(auto const& b : statusBadges(facts)) {
        if(b.text == "⚠ 3") { errors = b.command == Command::messages; }
        if(b.text == "build ✘") { build = b.command == Command::buildOutput; }
    }
    CHECK(errors && build, "the errors open the messages, a failed build its output");
}

void cutPadWrap() {
    std::vector<PanelRow> const rows{
      {{RunKind::message, 0, {}, "abc"}},
      {{RunKind::message, 0, {}, "abcdefghij"}},
      {label("ab"), {RunKind::message, 0, {}, ""}, {RunKind::message, 0, {}, "cdef"}}
    };
    CHECK_EQ(joined(panelRuns(rows, 0, 3, 6, CodePoints{})),
             std::string{"abc   \nabcde…\nabcdef"},
             "padded, cut with …, an exact fit not cut (an empty part neither)");
    CHECK_EQ(joined(panelRuns(rows, 1, 5, 6, CodePoints{})),
             std::string{"abcde…\nabcdef"},
             "from row 1, past the end");

    auto const wrapped = wrapParts(
      {
        {RunKind::message, 0, {}, std::string(20, 'x')}
    },
      10,
      2,
      CodePoints{});
    CHECK(wrapped.size() == 3 && joined(wrapped[0]) == std::string(10, 'x')
            && joined(wrapped[1]) == "  " + std::string(8, 'x')
            && joined(wrapped[2]) == "  " + std::string(2, 'x'),
          "wrapped at the width, the rows after the first indented");
}

void messagesAndBuild() {
    std::vector<MessageLog::Entry> const messages{
      {MessageLevel::error, {}, "it \x1b[31mbroke\x1b[0m", MessageKind::plain},
      { MessageLevel::tool, {},                 "started", MessageKind::plain}
    };
    auto const rows = messageRows(messages);
    CHECK(rows.size() == 2, "one row a message");
    CHECK(rows[0].size() >= 3 && rows[0][1].kind == RunKind::toned
            && rows[0][1].detail == static_cast<std::uint8_t>(Tone::bad)
            && rows[0][1].text.starts_with("error"),
          "an error is bad");
    CHECK(joined(rows[0]).ends_with("it broke")
            && rows[0].back().style.fg.kind == AnsiColor::Kind::palette
            && rows[0][rows[0].size() - 2].style.plain(),
          "the message's own colour stays a style");
    std::vector<BuildEntry> const build{
      {{},   "compiling", false, false},
      {{}, "error: nope", false,  true}
    };
    auto const out = buildRows(build);
    CHECK(out.size() == 2 && out[1].back().detail == static_cast<std::uint8_t>(Tone::bad),
          "a build error is bad");
    std::vector<BuildEntry> const progress{
      {{}, "[0/7] Building host tool\r[1/7] Updating\r[2/7] Linking", false, false}
    };
    CHECK_EQ(joined(buildRows(progress)[0]).substr(13),
             std::string{"[2/7] Linking"},
             "a line rewritten with a carriage return: its last state, as a terminal shows it");
}

void detailsAndHelp() {
    EntryStore         store;
    EntryStore::Mirror mirror;
    store.addEntry(std::chrono::system_clock::time_point{},
                   entry("first line\nsecond @METRIC(a::b[V]=3)"));
    store.refreshMirror(mirror);
    auto const               rows = detailRows(mirror.entries[0], mirror, {}, 60, CodePoints{});
    std::vector<std::string> text;
    text.reserve(rows.size());
    for(auto const& r : rows) { text.push_back(joined(r)); }
    auto const has = [&](std::string_view part) {
        return std::ranges::any_of(text, [&](std::string const& t) { return t.contains(part); });
    };
    CHECK(text.size() >= 2 && text[0].starts_with("message   first line")
            && text[1] == "          second 3 V",
          "the message first, every line of it, markers shown");
    CHECK(has("function  fn") && has("location  main.cpp:7")
            && has("line      warn · channel 0 · target "),
          "the fields after it");
    CHECK(helpRows().size() == 1 + tabTable.size() + commandTable.size(),
          "the help: a heading, every tab and every command");
}

void targetTime() {
    CHECK(parseTargetTime("12.5") == std::chrono::nanoseconds{12'500'000'000}, "seconds");
    CHECK(parseTargetTime(" 250ms ") == std::chrono::nanoseconds{250'000'000}, "ms, with spaces");
    CHECK(parseTargetTime("3 us") == std::chrono::nanoseconds{3000}, "us");
    CHECK(parseTargetTime("1min") == std::chrono::nanoseconds{60'000'000'000}, "min");
    CHECK(!parseTargetTime("-1") && !parseTargetTime("abc") && !parseTargetTime("5 parsecs")
            && !parseTargetTime("1e30") && !parseTargetTime(""),
          "negative, no number, an unknown unit, past the clock, nothing");

    EntryStore         store;
    EntryStore::Mirror mirror;
    for(int i = 0; i < 10; ++i) {
        store.addEntry(std::chrono::system_clock::time_point{},
                       entry("t", std::chrono::seconds{i}));
    }
    store.refreshMirror(mirror);
    CHECK(firstAtOrAfter(mirror.entries, 4500ms) == 5U, "the first at or after");
    CHECK(firstAtOrAfter(mirror.entries, 0ns) == 0U, "from the oldest");
    CHECK(!firstAtOrAfter(mirror.entries, 20s), "none after the newest");
}

void plainAndScrollbar() {
    EntryStore         store;
    EntryStore::Mirror mirror;
    store.addEntry(std::chrono::system_clock::time_point{}, entry("a \x1b[1mbold\x1b[0m word"));
    store.refreshMirror(mirror);
    auto const text = plainText(mirror.entries[0], mirror, {}, false);
    CHECK(text.contains("warn| a bold word (main.cpp:7 fn)"), "the colours out, the location in");

    CHECK_EQ(scrollbarText(3, 0, 2, 2), std::string{" \n \n "}, "everything fits: blank");
    CHECK_EQ(scrollbarText(4, 0, 4, 8), std::string{"█\n█\n│\n│"}, "at the top: the thumb on top");
    CHECK_EQ(scrollbarText(4, 4, 4, 8),
             std::string{"│\n│\n█\n█"},
             "at the end: the thumb at the bottom");
    CHECK_EQ(scrollbarText(4, 2, 1, 1000),
             std::string{"█\n│\n│\n│"},
             "a long log: a thumb of one row");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {      "commandKeys",       commandKeys},
                               {  "paletteMatching",   paletteMatching},
                               {        "wordMatch",         wordMatch},
                               {    "badgeCommands",     badgeCommands},
                               {       "cutPadWrap",        cutPadWrap},
                               { "messagesAndBuild",  messagesAndBuild},
                               {   "detailsAndHelp",    detailsAndHelp},
                               {       "targetTime",        targetTime},
                               {"plainAndScrollbar", plainAndScrollbar},
    });
}
