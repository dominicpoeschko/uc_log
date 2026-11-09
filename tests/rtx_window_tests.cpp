// The RTXUI window, drawn headless (support/RtxWindow.hpp): the status line, the log at the end and after keys, a
// click that marks a row, text that looks like markup shown as it is, quiet frames that format nothing, a click on a
// badge, the jump to a target time, export and the scrollbar, the tabs and the head's buttons, drags on a panel's
// title and the hints, the context menu and a drag on the scrollbar.
#include "support/Check.hpp"
#include "support/RtxWindow.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace uc_log::test::rtx_window;

namespace {

void drawsStatusLogAndModeLine() {
    Window     w{100};
    auto const rows = rowsOf(w.text());
    CHECK(rows.size() >= 12, "twelve rows");
    if(rows.size() < 12) { return; }
    CHECK(contains(rows[0], "LOGS 100") && contains(rows[0], "link ○"), "the status line on top");
    CHECK(contains(rows[9], "line 99") && contains(rows[9], "main.cpp:7"),
          "the newest line at the bottom of the log");
    CHECK(contains(rows[2], "📄 Logs · everything shown") && contains(rows[3], "╭─ 📜 The lines")
            && rows[10].starts_with("╰"),
          "the log as every tab: its head, then its lines in a frame");
    CHECK(contains(rows[1], "1 📄 Logs") && contains(rows[1], "6 📈") && contains(rows[1], "0 ❓"),
          "the tabs under it (80 cells: the icons, the open one with its name)");
    CHECK(contains(rows[4], "line 94"), "six rows of log: line 94 on top");
    CHECK(contains(rows[11], "follow · 100 lines"), "the mode line says it follows");
}

void keysMoveTheView() {
    Window w{100};
    w.screen->input("\x1b[5~");   // PageUp
    auto rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[11], "line 94 / 100"), "PageUp: the mark a page up");
    CHECK(rows.size() >= 12 && contains(rows[4], "line 93"), "the view keeps the mark on screen");
    w.screen->input("g");
    rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[4], "line 0") && contains(rows[11], "line 1 / 100"),
          "g: the oldest line");
    w.add("new");
    w.frame();
    w.screen->input("");
    rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[4], "line 0"),
          "anchored: a new line does not move the view");
    w.screen->input("G");
    rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[9], "new") && contains(rows[11], "follow"),
          "G follows again");
}

void clickMarksARow() {
    Window w{100};
    w.screen->click(
      5,
      4);   // 0-based: the second log row (the status line, the tabs, the frame, then the log)
    auto const rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[11], "line 95 / 100"), "the clicked row is marked");
}

void markupIsText() {
    Window w{0};
    w.add("a {mirror} b <b>bold</b> & \x1b[31mred\x1b[0m {s.text}");
    w.frame();
    w.screen->input("");
    auto const text = w.text();
    CHECK(contains(text, "a {mirror} b <b>bold</b> & red {s.text}"),
          "braces, tags and entities are text, the colour escape is a style");
}

void quietFramesFormatNothing() {
    Window     w{1000};
    auto const before = w.app->log->formattedRows();
    for(int i = 0; i < 20; ++i) { w.frame(); }
    w.screen->input("");
    CHECK(w.app->log->formattedRows() == before, "frames with nothing new format no row");
    for(int i = 0; i < 500; ++i) { w.add("more"); }
    w.frame();
    CHECK(w.app->log->formattedRows() == before + 6, "500 new lines format one view of 6 rows");
}

void badgeClickOpensMessages() {
    Window w{100};
    w.frame(2);
    w.screen->input("");
    auto const rows = rowsOf(w.text());
    CHECK(!rows.empty() && contains(rows[0], "⚠ 2"), "the errors badge");
    if(rows.empty()) { return; }
    w.screen->click(static_cast<int>(cellOf(rows[0], "LOGS")), 0);
    CHECK(!contains(w.text(), "Messages · "), "a badge without a command does nothing");
    w.screen->click(static_cast<int>(cellOf(rows[0], "⚠ 2")), 0);
    CHECK(contains(w.text(), "Messages · "), "a click on the errors opens the messages");
}

void jumpExportScrollbar() {
    Window w{100};
    w.screen->input("T");
    w.screen->input("42.5");
    w.screen->input("\r");
    CHECK(contains(w.text(), "line 44 / 100"),
          "T 42.5: the first line at or after 42.5 s (43 s, the 44th)");
    w.screen->input("e");
    CHECK(w.exports == 1, "e exports");
    w.screen->input("G");
    auto const rows  = rowsOf(w.text());
    bool       thumb = false;
    bool       track = false;
    for(std::size_t r = 3; r + 2 < rows.size(); ++r) {
        if(rows[r].ends_with("█│")) { thumb = true; }
        if(rows[r].ends_with("││")) { track = true; }
    }
    CHECK(thumb && track,
          "the scrollbar: a thumb and a track in the column left of the frame's border");
    CHECK(rows.size() >= 10 && rows[9].ends_with("█│"), "following: the thumb at the bottom");
}

void tabsAndButtons() {
    Window w{100};
    auto   rows = rowsOf(w.text());
    if(rows.size() < 12) { return; }
    w.screen->click(static_cast<int>(cellOf(rows[1], "3 🔍")), 1);
    CHECK(w.app->panel() == Tab::filter && contains(w.text(), "Filter · everything shown"),
          "a click on a tab opens it");
    rows = rowsOf(w.text());
    CHECK(contains(rows[1], "3 🔍 Filter"), "the open tab with its icon");
    w.resize(140, 20);
    w.screen->input("2");
    CHECK(w.app->panel() == Tab::build, "its key picks a tab too");
    rows = rowsOf(w.text());
    w.screen->click(static_cast<int>(cellOf(rows[1], "2 🔨 Build")), 1);
    CHECK(w.app->panel() == Tab::build, "a click on the open tab keeps it open");
    // the build panel's frame: its title and its buttons in the top border
    CHECK(rows.size() > 2 && contains(rows[2], "🔨 Build · idle") && contains(rows[2], "▶ Build b")
            && contains(rows[2], "✕ Esc") && !contains(w.text(), "line 99"),
          "the page has the window, its buttons in its frame");
    CHECK(clickText(w, "▶ Build b") && w.builds == 1, "a click on a button runs it");
    CHECK(clickText(w, "▤ Split") && w.app->split() && contains(w.text(), "line 99"),
          "the split button: the page under the log");
    w.screen->input("z");
    CHECK(!w.app->split() && !contains(w.text(), "line 99"), "z: the whole window again");
    CHECK(clickText(w, "✕ Esc") && w.app->panel() == Tab::logs, "the close button");
    rows = rowsOf(w.text());
    w.screen->click(static_cast<int>(cellOf(rows[1], "6 📈")), 1);
    w.screen->input("1");
    CHECK(w.app->panel() == Tab::logs && !contains(w.text(), "📈 Metrics"), "1: the log alone");
}

void dragHintsAndPalette() {
    Window w{100};
    w.screen->input(
      "|");   // the filter has the window: its head on row 2 (under the status line and the tabs)
    CHECK(contains(rowsOf(w.text())[2], "Filter ·"), "the page's head row");
    w.screen->input(sgrMouse(0, 3, 2) + sgrMouse(32, 3, 6) + sgrMouse(0, 3, 6, true));
    auto rows = rowsOf(w.text());
    CHECK(
      rows.size() >= 12 && w.app->split() && contains(rows[6], "Filter ·")
        && contains(rows[2], "📄 Logs") && contains(rows[4], "line 99"),
      "a drag on the head splits: the page under the log (in its frame), from where it was let go");
    w.screen->input("\x1b");
    rows = rowsOf(w.text());
    if(rows.size() < 12) { return; }
    w.screen->click(static_cast<int>(cellOf(rows[11], "/  search")), 11);
    CHECK(w.app->asking() == Field::search, "a click on a hint runs it");
    w.screen->input("\x1b");
    w.screen->input(":");
    w.screen->input("follow");
    rows           = rowsOf(w.text());
    std::size_t at = 0;
    for(std::size_t r = 0; r < rows.size(); ++r) {
        if(contains(rows[r], "follow the newest line")) { at = r; }
    }
    w.screen->input("g");   // typed into the query: nothing matches "followg"
    w.screen->input("\x7f");
    w.screen->click(static_cast<int>(cellOf(rows[at], "follow the newest")), static_cast<int>(at));
    CHECK(at > 0 && !w.app->dialog->open,
          "a click on a palette row runs it and closes the palette");
}

void contextMenuAndScrollbarDrag() {
    Window w{100};
    // a right click on a log row: the row marked, a menu where the mouse is
    w.screen->input(sgrMouse(2, 20, 4));
    CHECK(w.app->menu->open && contains(w.text(), "🔎 Details")
            && contains(w.text(), "🙈 Hide main.cpp:7") && contains(w.text(), "line 95 / 100"),
          "a right click: the line marked, its menu");
    CHECK(clickText(w, "🎯 Only main.cpp:7") && !w.app->menu->open
            && w.lastFilter.includedLocations.contains(SourceLocation{"main.cpp", 7}),
          "a menu item runs, the menu closes");
    w.screen->input(sgrMouse(2, 20, 4));
    w.screen->input(sgrMouse(0, 70, 1));
    CHECK(!w.app->menu->open, "a click elsewhere closes it");
    // the log's scrollbar (the column left of the frame's border): a press jumps, a drag follows
    w.screen->input(sgrMouse(0, 78, 4) + sgrMouse(32, 78, 4) + sgrMouse(0, 78, 4, true));
    CHECK(contains(w.text(), "line 1 / 100"), "the thumb dragged to the top: the oldest line");
    w.screen->input(sgrMouse(0, 78, 4) + sgrMouse(32, 78, 7) + sgrMouse(0, 78, 7, true));
    CHECK(contains(w.text(), "line 51 / 100"),
          "dragged three rows down: three sixths into the log");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {  "drawsStatusLogAndModeLine",   drawsStatusLogAndModeLine},
                               {            "keysMoveTheView",             keysMoveTheView},
                               {             "clickMarksARow",              clickMarksARow},
                               {               "markupIsText",                markupIsText},
                               {   "quietFramesFormatNothing",    quietFramesFormatNothing},
                               {    "badgeClickOpensMessages",     badgeClickOpensMessages},
                               {        "jumpExportScrollbar",         jumpExportScrollbar},
                               {             "tabsAndButtons",              tabsAndButtons},
                               {        "dragHintsAndPalette",         dragHintsAndPalette},
                               {"contextMenuAndScrollbarDrag", contextMenuAndScrollbarDrag}
    });
}
