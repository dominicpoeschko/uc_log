// The front-end-neutral pieces of a log view: a line's shown text and ANSI runs (LineText.hpp), where the view stands
// (LogViewport.hpp), how often it draws (FramePacer.hpp), the runs of the visible rows (LogScreen.hpp) and a list's
// marked row (ListCursor.hpp). No window, no clock: the pacer is handed its time points, and the layout's cost is
// counted in formatted rows, never timed.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/FramePacer.hpp"
#include "uc_log/printer/LineText.hpp"
#include "uc_log/printer/ListCursor.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/LogViewport.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace uc_log::printer;
using uc_log::LogLevel;

namespace {

// one cell per code point: what these tests write is ASCII, "…" and "⚠"
struct CodePoints {
    std::size_t operator()(std::string_view text) const {
        std::size_t n = 0;
        for(char const c : text) {
            if((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) { ++n; }
        }
        return n;
    }
};

std::string joined(std::vector<AnsiRun> const& runs) {
    std::string s;
    for(auto const& r : runs) { s += r.text; }
    return s;
}

std::string joined(std::vector<ScreenRun> const& runs) {
    std::string s;
    for(auto const& r : runs) { s += r.text; }
    return s;
}

LogEntry entry(std::string msg,
               std::string file  = "main.cpp",
               std::size_t line  = 7,
               LogLevel    level = LogLevel::info) {
    LogEntry e{0, {}};
    e.logLevel     = level;
    e.fileName     = std::move(file);
    e.line         = line;
    e.logMsg       = std::move(msg);
    e.functionName = "fn";
    e.parsedOk     = true;
    return e;
}

// a store with `n` lines "line <i>" and its mirror
struct Filled {
    EntryStore         store;
    EntryStore::Mirror mirror;

    explicit Filled(std::size_t n) {
        for(std::size_t i = 0; i < n; ++i) { add("line " + std::to_string(i)); }
        refresh();
    }

    void add(std::string msg) {
        store.addEntry(std::chrono::system_clock::time_point{}, entry(std::move(msg)));
    }

    void refresh() { store.refreshMirror(mirror); }

    [[nodiscard]] SnapshotLines lines() const { return SnapshotLines{&mirror.entries}; }
};

// plain ids 0..n-1 as LogViewport's lines; `drop` removes ids (a trim, a refilter)
struct Ids {
    std::vector<LineId> ids;

    explicit Ids(std::size_t n) {
        for(std::size_t i = 0; i < n; ++i) { ids.push_back({i, 0}); }
    }

    [[nodiscard]] std::size_t size() const { return ids.size(); }

    [[nodiscard]] LineId idAt(std::size_t i) const { return ids[i]; }
};

// a row as the layout writes it: prefix, message padded to `room` cells, then " meta" (ASCII, one cell a byte)
std::string row(std::string_view prefix,
                std::string_view message,
                std::size_t      room,
                std::string_view meta = {}) {
    std::string r{prefix};
    r += message;
    r.append(room - message.size(), ' ');
    if(!meta.empty()) {
        r += ' ';
        r += meta;
    }
    return r;
}

AnsiColor palette(std::uint8_t index) { return {AnsiColor::Kind::palette, index, 0, 0, 0}; }

AnsiColor rgb(std::uint8_t r,
              std::uint8_t g,
              std::uint8_t b) {
    return {AnsiColor::Kind::rgb, 0, r, g, b};
}

LogColumns bare() {
    LogColumns c;
    c.hostTime   = false;
    c.targetTime = false;
    c.channel    = false;
    return c;
}

LogScreen<CodePoints>::Frame frameOf(Filled const&              f,
                                     LogViewport::Window const& window,
                                     std::size_t                width) {
    LogScreen<CodePoints>::Frame frame;
    frame.storeVersion = f.mirror.seenDisplayVersion;
    frame.window       = window;
    frame.width        = width;
    frame.columns      = bare();
    return frame;
}

// ---- LineText ------------------------------------------------------------------------------------------------------

void shownTextMarkers() {
    auto const line = std::string{"t @METRIC(adc::vin[V]=3.3) of @TYPENAME(int) end"};
    CHECK_EQ(shownText(line, {}),
             std::string{"t 3.3 V of  end"},
             "metric as value unit, typename hidden");
    CHECK_EQ(shownText(line, {.metricMarkers = true, .typenameMarkers = true}),
             std::string{"t @METRIC(adc::vin[V]=3.3) of int end"},
             "both kept");
    CHECK_EQ(shownText("no markers", {}), std::string{"no markers"}, "plain text unchanged");
    CHECK_EQ(shownText("open @TYPENAME(x", {}),
             std::string{"open @TYPENAME(x"},
             "an unclosed marker stays");
}

void ansiRunsColours() {
    CHECK(ansiRuns("").empty(), "empty text: no run");
    auto const plain = ansiRuns("plain");
    CHECK(plain.size() == 1 && plain[0].style.plain() && plain[0].text == "plain",
          "plain text: one plain run");

    auto const runs
      = ansiRuns("a\x1B[1;31mred\x1B[0m b\x1B[38;5;208mo\x1B[48;2;1;2;3mx\x1B[39;49m n");
    CHECK_EQ(joined(runs), std::string{"ared box n"}, "text without the escapes");
    CHECK(runs.size() == 6, "a, red, ' b', o, x, ' n'");
    if(runs.size() == 6) {
        CHECK(runs[1].text == "red" && runs[1].style.bold && runs[1].style.fg == palette(1),
              "1;31: bold red");
        CHECK(runs[2].text == " b" && runs[2].style.plain(), "0: reset");
        CHECK(runs[3].style.fg == palette(208), "38;5;208");
        CHECK(runs[4].text == "x" && runs[4].style.bg == rgb(1, 2, 3)
                && runs[4].style.fg == runs[3].style.fg,
              "48;2;1;2;3 keeps the foreground");
        CHECK(runs[5].text == " n" && runs[5].style.plain(), "39;49: default colours");
    }
    auto const bright = ansiRuns("\x1B[94mb\x1B[101mc");
    CHECK(bright.size() == 2 && bright[0].style.fg == palette(12)
            && bright[1].style.bg == palette(9),
          "90-97 / 100-107: the bright palette");

    auto const link = ansiRuns("see \x1B]8;;https://example.com\x1B\\here\x1B]8;;\x07 end");
    CHECK(link.size() == 3 && link[1].text == "here"
            && link[1].style.hyperlink == "https://example.com" && link[2].style.hyperlink.empty(),
          "OSC 8 hyperlink and its end");

    CHECK_EQ(joined(ansiRuns("x\x1B[2Jy\x1B(Bz\r\x07w\tv")),
             std::string{"xyzw\tv"},
             "a cursor move, a charset switch and control characters are dropped, a tab kept");
    CHECK_EQ(joined(ansiRuns("cut\x1B[1")),
             std::string{"cut"},
             "an escape cut off at the end is dropped");
}

// ---- LogViewport -------------------------------------------------------------------------------------------------

void viewportFollow() {
    Ids const   lines{100};
    LogViewport v;
    auto const  w = v.place(lines, 10);
    CHECK(w.follow && w.first == 90 && w.count == 10 && !w.selected,
          "follow: the last 10 lines, nothing marked");
    Ids const  few{3};
    auto const w3 = v.place(few, 10);
    CHECK(w3.first == 0 && w3.count == 3, "fewer lines than rows: all of them");
    CHECK(v.place(Ids{0}, 10).count == 0, "no lines: an empty window");
}

void viewportMoveAndPage() {
    Ids const   lines{100};
    LogViewport v;
    v.moveSelection(lines, 10, -1);
    auto w = v.place(lines, 10);
    CHECK(!w.follow && w.selected == 98U && w.first == 90,
          "Up from follow: the line before the newest marked");
    v.moveSelection(lines, 10, -10);
    w = v.place(lines, 10);
    CHECK(w.selected == 88U && w.first == 88,
          "PageUp: the mark moves a page, the view keeps it on top");
    v.moveSelection(lines, 10, 1000);
    w = v.place(lines, 10);
    CHECK(!w.follow && w.selected == 99U && w.first == 90,
          "past the end: the newest line, still anchored");
    v.home(lines);
    w = v.place(lines, 10);
    CHECK(w.first == 0 && w.selected == 0U, "Home");
    v.follow();
    CHECK(v.place(lines, 10).follow, "End follows again");
}

void viewportScrollBackToFollow() {
    Ids const   lines{100};
    LogViewport v;
    v.scroll(lines, 10, 3);
    CHECK(v.following(), "wheel down while following: nothing changes");
    v.scroll(lines, 10, -3);
    auto w = v.place(lines, 10);
    CHECK(!w.follow && w.first == 87 && w.selected == 96U,
          "wheel up: 3 lines back, the mark kept on screen");
    v.scroll(lines, 10, -100);
    w = v.place(lines, 10);
    CHECK(w.first == 0 && w.selected == 9U,
          "the mark follows the view when its line leaves the screen");
    v.selectRow(lines, 10, 4);
    CHECK(v.place(lines, 10).selected == 4U, "a click marks the row");
    v.scroll(lines, 10, 1000);
    CHECK(v.following(), "wheel down to the newest line follows again");
}

void viewportKeepsTheLine() {
    Ids         lines{100};
    LogViewport v;
    v.show(lines, 10, 50);
    auto w = v.place(lines, 10);
    CHECK(w.first == 45 && w.selected == 50U, "show: the line marked, half a view before it");

    // the store trims its oldest 20 lines: every index moves, the view does not
    lines.ids.erase(lines.ids.begin(), lines.ids.begin() + 20);
    w = v.place(lines, 10);
    CHECK(lines.idAt(w.first).group == 45 && lines.idAt(w.selected.value_or(0)).group == 50,
          "a trim keeps the lines shown");

    // a refilter drops the marked line and the top one: the view stays at the next ones
    std::erase_if(lines.ids, [](LineId id) { return id.group == 50 || id.group == 45; });
    w = v.place(lines, 10);
    CHECK(lines.idAt(w.first).group == 46 && lines.idAt(w.selected.value_or(0)).group == 51,
          "a line that went: the one after it");

    // everything shown trimmed away: the view stands at the oldest line left, the one after what it showed
    lines.ids.erase(lines.ids.begin(), lines.ids.begin() + 60);
    w = v.place(lines, 10);
    CHECK(w.first == 0 && w.count == 10 && w.selected == 0U && !w.follow,
          "at the oldest line left, still anchored");
}

// ---- FramePacer --------------------------------------------------------------------------------------------------

void pacerOneFramePerInterval() {
    using namespace std::chrono_literals;
    FramePacer                  pacer{33ms};
    FramePacer::TimePoint const t0{std::chrono::seconds{100}};
    CHECK(pacer.request(), "the first request posts the wake-up");
    CHECK(!pacer.request() && !pacer.request(), "more requests are the same frame");
    CHECK(pacer.delay(t0) == FramePacer::Duration::zero(), "nothing drawn yet: draw now");
    pacer.begin(t0);
    CHECK(pacer.request(), "a request after the frame began posts again");
    CHECK(pacer.delay(t0 + 10ms) == 23ms, "10 ms later: wait the rest of the interval");
    CHECK(pacer.delay(t0 + 40ms) == FramePacer::Duration::zero(), "past the interval: draw now");
    pacer.begin(t0 + 40ms);
    pacer.drewFor(t0 + 50ms);
    CHECK(pacer.delay(t0 + 60ms) == 23ms,
          "a frame for input counts: the next paced one waits from it");
}

// ---- LogScreen ----------------------------------------------------------------------------------------------------

void screenRowLayout() {
    Filled f{0};
    f.add("hello");
    f.add(std::string(60, 'x'));
    f.refresh();
    LogScreen<CodePoints> screen{CodePoints{}};
    auto                  frame = frameOf(f, LogViewport{}.place(f.lines(), 5), 40);
    CHECK(screen.build(frame, f.mirror.entries, f.mirror), "the first frame is built");
    // 40 cells: "info | " 7, " main.cpp:7" 11, the message 22
    CHECK_EQ(joined(screen.runs()),
             row("info | ", "hello", 22, "main.cpp:7") + '\n' + "info | " + std::string(21, 'x')
               + "… main.cpp:7",
             "level, message padded or cut with …, the location at the right edge");

    frame.hOffset = 50;
    screen.build(frame, f.mirror.entries, f.mirror);
    CHECK_EQ(joined(screen.runs()),
             row("info | ", "", 22, "main.cpp:7") + '\n'
               + row("info | ", std::string(10, 'x'), 22, "main.cpp:7"),
             "a horizontal offset shifts the message, the prefix and location stay");

    frame.hOffset = 0;
    frame.width   = 12;
    screen.build(frame, f.mirror.entries, f.mirror);
    CHECK_EQ(joined(screen.runs()),
             std::string{"info | hello\n"} + "info | xxxx…",
             "a narrow view leaves the location out");

    frame.width            = 40;
    frame.columns.level    = false;
    frame.columns.function = true;
    frame.columns.location = false;
    frame.window.selected  = 1;
    screen.build(frame, f.mirror.entries, f.mirror);
    auto const& runs = screen.runs();
    CHECK_EQ(joined(runs),
             row("", "hello", 37, "fn") + '\n' + std::string(36, 'x') + "… fn",
             "function at the edge, no level column");
    bool marked = true;
    for(std::size_t i = 0; i < runs.size(); ++i) {
        bool const second = i >= runs.size() / 2;
        if(runs[i].selected != second) { marked = false; }
    }
    CHECK(marked, "every run of the marked row, and only those, is selected");
}

void screenMultilineAndKinds() {
    Filled f{0};
    f.store.addEntry(std::chrono::system_clock::time_point{},
                     entry("first\nsecond", "a.cpp", 3, LogLevel::warn));
    auto broken     = entry("??");
    broken.parsedOk = false;
    f.store.addEntry(std::chrono::system_clock::time_point{}, broken);
    f.store.addEntry(std::chrono::system_clock::time_point{}, entry("\x1B[31mred\x1B[0m plain"));
    f.refresh();
    LogScreen<CodePoints> screen{CodePoints{}};
    auto                  frame = frameOf(f, LogViewport{}.place(f.lines(), 10), 30);
    frame.needle                = "plain";
    screen.build(frame, f.mirror.entries, f.mirror);
    // 30 cells; the unparsed row's location does not fit beside "⚠ unparsed " and is left out
    auto const brokenLevel = fmt::format("{:<5}| ", levelName(broken.logLevel));
    CHECK_EQ(joined(screen.runs()),
             row("warn | ", "first", 23) + '\n' + row("       ", "second", 15, "a.cpp:3") + '\n'
               + row(brokenLevel + "⚠ unparsed ", "??", 12) + '\n'
               + row("info | ", "red plain", 12, "main.cpp:7"),
             "a continuation row is indented and only the last row has the location; an unparsed "
             "line is marked");
    bool sawLevel = false;
    bool sawRed   = false;
    bool sawMatch = false;
    for(auto const& r : screen.runs()) {
        if(r.kind == RunKind::level && r.detail == static_cast<std::uint8_t>(LogLevel::warn)) {
            sawLevel = true;
        }
        if(r.kind == RunKind::message && r.text == "red" && r.style.fg == palette(1)) {
            sawRed = true;
        }
        if(r.match && r.text.starts_with(" plain")) { sawMatch = true; }
    }
    CHECK(sawLevel, "the level run carries its level");
    CHECK(sawRed, "the message's own colour is its run's style");
    CHECK(sawMatch, "a row the search matches marks its message runs");
}

void screenFormatsOnlyWhatChanged() {
    Filled                f{1000};
    LogScreen<CodePoints> screen{CodePoints{}};
    LogViewport           view;
    auto                  frameNow = [&] { return frameOf(f, view.place(f.lines(), 20), 80); };
    CHECK(screen.build(frameNow(), f.mirror.entries, f.mirror), "first frame");
    CHECK(screen.formattedRows() == 20, "one screen formatted, not the store");
    CHECK(!screen.build(frameNow(), f.mirror.entries, f.mirror), "the same frame again: not built");
    CHECK(screen.formattedRows() == 20, "nothing new formats nothing");

    for(int i = 0; i < 100; ++i) { f.add("new"); }
    f.refresh();
    CHECK(screen.build(frameNow(), f.mirror.entries, f.mirror), "new lines while following: built");
    CHECK(screen.formattedRows() == 40, "100 new lines format one screen, not 100 rows");
    CHECK(!joined(screen.runs()).contains("line "), "following: only the newest lines");

    view.scroll(f.lines(), 20, -500);
    CHECK(screen.build(frameNow(), f.mirror.entries, f.mirror), "scrolled: built");
    CHECK(screen.formattedRows() == 60, "a scroll formats one screen");
    auto const before = view.place(f.lines(), 20);
    f.add("more");
    f.refresh();
    CHECK(view.place(f.lines(), 20) == before, "a new line while anchored does not move the view");
}

void listCursorMovesAndFits() {
    ListCursor c;
    c.reset(false);
    CHECK(c.marked == 0U && c.top == 0, "from the start: the first row marked");
    c.move(5, 10, false);
    CHECK(c.marked == 5U, "five rows on");
    c.move(20, 10, false);
    CHECK(c.marked == 9U, "the last row held");
    c.move(-20, 10, false);
    CHECK(c.marked == 0U, "the first row held");
    c.move(-1, 10, true);
    CHECK(!c.marked.has_value(), "under a field: up from the first row is the field");
    c.move(1, 10, true);
    CHECK(c.marked == 0U, "and down from it the first row");
    c.reset(true);
    CHECK(!c.marked.has_value() && c.top == 0, "from the start under a field: the field");

    c.marked = 9;
    c.fit(10, 4);
    CHECK(c.top == 6, "the rows shown follow the mark down: it is the last of them");
    c.marked = 2;
    c.fit(10, 4);
    CHECK(c.top == 2, "and up: the first");
    c.marked = 8;
    c.fit(5, 4);
    CHECK(c.marked == 4U && c.top == 1,
          "fewer rows than the mark: the last one marked, the shown rows ending with it");
    c.marked = 99;
    c.fit(100, 4);
    c.fit(100, 80);   // the window grew while the view was at the end
    CHECK(c.top == 20 && c.top + 80 <= 100,
          "more room: more rows above, none past the last (a page read past its table)");
    c.fit(30, 80);   // the list shrank (a scope folded, words narrowed it)
    CHECK(c.top == 0 && c.marked == 29U, "fewer rows than the room: from the first");
    c.move(1, 0, false);
    c.fit(0, 4);
    CHECK(!c.marked.has_value() && c.top == 0, "no rows: nothing marked");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {            "shownTextMarkers",             shownTextMarkers},
                               {             "ansiRunsColours",              ansiRunsColours},
                               {              "viewportFollow",               viewportFollow},
                               {         "viewportMoveAndPage",          viewportMoveAndPage},
                               {  "viewportScrollBackToFollow",   viewportScrollBackToFollow},
                               {        "viewportKeepsTheLine",         viewportKeepsTheLine},
                               {    "pacerOneFramePerInterval",     pacerOneFramePerInterval},
                               {             "screenRowLayout",              screenRowLayout},
                               {     "screenMultilineAndKinds",      screenMultilineAndKinds},
                               {"screenFormatsOnlyWhatChanged", screenFormatsOnlyWhatChanged},
                               {      "listCursorMovesAndFits",       listCursorMovesAndFits},
    });
}
