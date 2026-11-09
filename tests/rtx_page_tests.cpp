// The RTXUI window's panels and pages, drawn headless (support/RtxWindow.hpp): the messages, the help and a line's
// details, the filter page and a filter from the marked line, the metrics' table and plot, the settings, the debugger,
// health and the inspector's pages.
#include "support/Check.hpp"
#include "support/RtxWindow.hpp"

#include <chrono>
#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <vector>

using namespace uc_log::test::rtx_window;

namespace {

void messagesPanel() {
    Window w{100};
    w.app->messageEntries.push_back({MessageLevel::error, {}, "boom", MessageKind::plain});
    w.frame();
    w.screen->input("s");
    CHECK(contains(w.text(), "Messages · 1") && contains(w.text(), "boom"), "s opens the messages");
    auto const rows = rowsOf(w.text());
    CHECK(rows.size() >= 12 && contains(rows[11], "follow"), "the mode line still at the bottom");
    w.screen->input("\x1b");
    w.screen->input("");
    CHECK(!contains(w.text(), "Messages ·"), "Escape closes it");
}

void helpAndDetails() {
    Window w{100};
    w.screen->input("?");
    CHECK(contains(w.text(), " ❓ Help ") && contains(w.text(), "⌨ Keys and commands")
            && contains(w.text(), "1 Esc      tab 📄 Logs"),
          "? opens the help (its head, its box; 80 cells: no room for its hint): the tabs first");
    w.screen->input("?");
    CHECK(!contains(w.text(), "⌨ Keys and commands"), "? again closes it");
    w.screen->input("k");
    w.screen->input("d");
    CHECK(contains(w.text(), "message   line 98") && contains(w.text(), "location  main.cpp:7"),
          "d: the marked line's details (one above the newest), the message first");
    CHECK(contains(w.text(), "line 99 / 100"),
          "the marked line stays on screen when the log gets smaller");
}

void filterPanel() {
    Window w{100};
    w.screen->input("|");
    CHECK(contains(w.text(), "Filter · everything shown") && contains(w.text(), "✔ trace"),
          "| opens the filter");
    w.screen->input(
      "\t");   // the keyboard: Tab to the first button (the head's first tool), Enter presses it
    w.screen->input("\r");
    CHECK(w.filters == 1 && w.lastFilter == FilterState{},
          "Tab and Enter press a page's button: show everything");
    CHECK(clickText(w, "✔ trace") && w.filters == 2 && w.lastFilter.enabledLogLevels.size() == 5
            && !w.lastFilter.enabledLogLevels.contains(uc_log::LogLevel::trace),
          "a click on trace: hidden, handed to the store");
    CHECK(contains(w.text(), "✘ trace") && contains(w.text(), "5 of 6 levels"),
          "the page shows it");
    CHECK(clickText(w, "✔ 3") && w.filters == 3 && w.lastFilter.enabledChannels.size() == 5,
          "a click on a channel: hidden");
    CHECK(clickText(w, "○ last 10 s") && w.lastTime && w.lastTime->enabled && w.lastTime->live
            && w.lastTime->liveWindowSecs == 10.0,
          "last 10 s: a live window handed to the store");
    w.screen->input(" ");   // the last one clicked has the focus: Space presses it again
    CHECK(w.lastTime && w.lastTime->live && w.lastTime->liveWindowSecs == 10.0 && w.filters == 3,
          "Space presses the focused button");
    w.screen->input("=");
    CHECK(w.lastFilter == FilterState{} && w.lastTime && !w.lastTime->enabled,
          "= clears everything");
}

void filterFromTheLine() {
    Window w{100};
    w.screen->input("-");
    CHECK(w.lastFilter.excludedLocations.contains(SourceLocation{"main.cpp", 7}),
          "- hides the line's location");
    w.screen->input("+");
    CHECK(w.lastFilter.includedLocations.contains(SourceLocation{"main.cpp", 7})
            && w.lastFilter.excludedLocations.empty(),
          "+ shows only it");
    w.screen->input("M");
    CHECK(contains(w.text(), "the line has no module"), "M on a line without a module says so");
    auto const rows = rowsOf(w.text());
    if(rows.empty()) { return; }
    w.screen->click(static_cast<int>(cellOf(rows[0], "LOGS")), 0);
    CHECK(contains(w.text(), "Filter · only 1 location"),
          "a click on LOGS opens the filter, which says what it does");
}

void metricsPanel() {
    Window w{10};
    w.resize(100, 40);
    w.screen->input("n");
    CHECK(contains(w.text(), "no metric yet"), "n: no metric yet");
    for(int i = 0; i < 4; ++i) {
        auto const at = std::chrono::system_clock::time_point{} + std::chrono::seconds{i};
        w.add("adc @METRIC(adc::vin[V]=" + std::to_string(i + 1) + ")", at);
        w.add("cpu @METRIC(cpu::load[%]=50)", at);
    }
    w.frame();
    w.screen->input("");   // a frame: drawn
    auto text = w.text();
    CHECK(contains(text, "Metrics · 2 in 2 groups") && contains(text, "📈 2 metrics")
            && contains(text, "▾ adc · 1") && contains(text, "▾ cpu · 1"),
          "the table: each scope's row");
    auto const rowWith = [](std::string const& screen, std::string_view part) {
        auto const lines = rowsOf(screen);
        auto const at    = rowOf(lines, part);
        return at ? lines[*at] : std::string{};
    };
    CHECK(contains(text, "name") && contains(text, "latest") && contains(text, "unit")
            && contains(rowWith(text, "  vin "), "  4  V ")
            && contains(rowWith(text, "  load "), "  50  % ")
            && contains(rowWith(text, "  vin "), "1/s"),
          "each metric's row under the heading: its numbers, its unit, its rate");
    CHECK(contains(text, "📉 adc::vin [V]") && contains(text, "◉ all") && contains(text, " 4 ┤")
            && contains(text, " 1 ┤") && contains(text, "└ -3.0 s") && contains(text, "newest")
            && contains(text, "4 values · min 1 V · mean 2.5 V · max 4 V · 1/s"),
          "the first one's plot, all of it: its value and time axes, its numbers under it");
    w.screen->input("\x1b[B");   // down: cpu::load
    w.screen->input("\x1b[C");   // right: the last 10 s
    text = w.text();
    CHECK(contains(text, "📉 cpu::load [%]") && contains(text, "◉ the last 10 s")
            && contains(text, " 55 ┤") && contains(text, "└ -10.0 s"),
          "down marks the next, over the scope's row; right takes the next range");
    w.add("first @METRIC(aaa::x=1)");   // sorts in before the marked one
    w.frame();
    w.screen->input("");
    CHECK(contains(w.text(), "Metrics · 3 in 3 groups") && contains(w.text(), "📉 cpu::load [%]"),
          "a new metric ahead of the marked one: the same one stays marked");
    CHECK(clickText(w, "  vin ") && contains(w.text(), "📉 adc::vin [V]"),
          "a click on a metric marks it");
    CHECK(clickText(w, "▾ adc · 1") && contains(w.text(), "▸ adc · 1")
            && !contains(w.text(), "  vin ") && contains(w.text(), "📉 adc::vin [V]"),
          "a click on a scope's row folds it, its metric still plotted");
    CHECK(clickText(w, "▸ adc · 1") && contains(w.text(), "  vin "), "and opens it again");

    CHECK(clickText(w, "🔎 Narrow …") && contains(w.text(), "🔎 Narrow the metrics")
            && contains(w.text(), "cpu::load [%]"),
          "the head's narrow: a dialog, the metrics as choices");
    w.screen->input("cpu");
    w.screen->input("\r");
    text = w.text();
    CHECK(!w.app->dialog->open && contains(text, "📈 1 of 3 metrics") && contains(text, "🔎 cpu …")
            && !contains(text, "  vin ") && contains(text, "📉 cpu::load [%]"),
          "Enter narrows the table to the metrics that hold the words; the mark goes to one shown");
    CHECK(clickText(w, "✕ all") && contains(w.text(), "📈 3 metrics"), "✕ all: every metric again");

    w.resize(200, 40);
    w.frame();
    w.screen->input("");
    auto const rows    = rowsOf(w.text());
    auto const plotAt  = rowOf(rows, "─ 📉 ");
    auto const tableAt = rowOf(rows, "📈 3 metrics");
    CHECK(plotAt && tableAt && *plotAt < *tableAt && rows[*plotAt].starts_with("╭─ 📉")
            && rows[*tableAt].starts_with("╭─ 📈"),
          "a wide window too: the plot on top across it, the table under it");
    w.resize(100, 40);

    CHECK(clickText(w, "○ the last 60 s") && contains(w.text(), "◉ the last 60 s"),
          "a click on a range takes it");
    CHECK(clickText(w, "🗑 Clear") && contains(w.text(), "no metric yet")
            && contains(w.text(), "metrics cleared"),
          "the head's clear: gone, and said");
    w.screen->input("n");
    CHECK(!contains(w.text(), "📈 Metrics · "), "n closes it");
}

void metricsWindowGrows() {
    // the crash a click found: the table scrolled to its end in a small window, then the window grew -
    // the rows shown ran past the table's last one
    Window w{0};
    w.resize(100, 20);
    for(int i = 0; i < 60; ++i) {
        w.add(fmt::format("v @METRIC(s{}::m{}[V]={})", i / 10, i % 10, i));
    }
    w.frame();
    w.screen->input("6");
    for(int i = 0; i < 30; ++i) {
        w.screen->input("\x1b[6~");
    }   // PgDn, a page of shown rows each: to the last metric
    CHECK(contains(w.text(), "📉 s5::m9 [V]") && contains(w.text(), "of 66"),
          "PgDn to the end: the last metric marked, the table scrolled");
    w.resize(284, 87);
    auto text = w.text();
    CHECK(contains(text, "📉 s5::m9 [V]") && contains(text, "rows 22-66 of 66"),
          "a big window: more rows above, the shown ones ending at the last, the mark kept");
    w.resize(100, 36);
    w.screen->input(
      "\x1b[A");   // up: the mark moves, the table's last 12 rows show it and its scope's row
    CHECK(clickText(w, "▾ s5 · 10") && contains(w.text(), "▸ s5 · 10")
            && contains(w.text(), "📉 s5::m8 [V]"),
          "its scope folded while scrolled to the end: fewer rows, drawn");
    w.resize(40, 12);
    w.resize(284, 87);
    CHECK(contains(w.text(), "▸ s5 · 10"), "tiny, then big again: drawn");
}

void metricsTableScrolls() {
    // the table under the plot scrolls by itself: the wheel and its scrollbar move its rows, the mark - the plot -
    // stays; a key moves the mark and its row comes back into view
    Window w{0};
    w.resize(100, 30);
    for(int i = 0; i < 60; ++i) {
        w.add(fmt::format("v @METRIC(s{}::m{}[V]={})", i / 10, i % 10, i));
    }
    w.frame();
    w.screen->input("6");
    auto       rows  = rowsOf(w.text());
    auto const table = rowOf(rows, "─ 📈 60 metrics · rows 1-");
    CHECK(table && contains(w.text(), "📉 s0::m0 [V]") && contains(rows[*table + 2], "▾ s0 · 10"),
          "the plot of the first metric, the table from its first row");
    if(!table) { return; }
    w.screen->input(
      sgrMouse(65, 20, static_cast<int>(*table) + 4));   // the wheel down, over the table
    rows = rowsOf(w.text());
    CHECK(contains(rows[*table], "rows 4-") && contains(w.text(), "📉 s0::m0 [V]"),
          "the wheel: three rows on, the plot still the marked metric's");
    // the scrollbar: in the frame's last column, beside the rows under the heading; its last cell is the end
    auto const& top  = rows[*table];
    auto const  bar  = static_cast<int>(rtx::cellWidth(top)) - 2;
    auto        last = *table + 2;
    while(last + 1 < rows.size() && !rows[last + 1].starts_with("╰")) { ++last; }
    w.screen->click(bar, static_cast<int>(last));
    rows = rowsOf(w.text());
    CHECK(contains(rows[*table], "of 66") && contains(rows[*table], "-66 of 66")
            && contains(w.text(), "📉 s0::m0 [V]"),
          "a click on the scrollbar's last cell: the table's end, the mark where it was");
    w.screen->input("\x1b[B");   // down: the next metric, its row shown again
    rows = rowsOf(w.text());
    CHECK(contains(w.text(), "📉 s0::m1 [V]") && contains(rows[*table], "rows 3-"),
          "a key moves the mark, the rows go back to it: its row the first shown");
}

void settingsPage() {
    Window w{10};
    w.resize(140, 40);
    auto& m             = w.app->printer.model;
    m.controlSocketPath = "/run/a/control.sock";
    m.controlEnabled    = true;
    m.controlStatus     = SocketPortStatus::active;
    m.controlClients    = 2;
    m.bindAddress       = "127.0.0.1";
    m.logFilePath       = "/logs/a.rttlog";
    m.logFileStatus     = LogFileStatus::active;
    w.screen->input("4");
    auto const text = w.text();
    CHECK(contains(text, "Settings") && contains(text, "✘ host time")
            && contains(text, "◉ unix sockets")
            && contains(text, "/run/a/control.sock · 2 clients"),
          "4: the settings - the columns, the transport, the control socket");
    CHECK(clickText(w, "✘ host time") && w.app->log->columns().hostTime,
          "a click on a column's switch sets it");
    CHECK(clickText(w, "○ TCP ports") && lastAct<action::Transport>(w) != nullptr
            && !lastAct<action::Transport>(w)->unixSockets,
          "a choice goes to the printer");
    CHECK(clickText(w, "✎ Port …") && contains(w.text(), "🎛 The control socket's TCP port"),
          "port …: a dialog asks for it");
    w.screen->input("80\r");
    CHECK(w.app->dialog->open && contains(w.text(), "not a port (1024-65535): \"80\"")
            && lastAct<action::ControlPort>(w) == nullptr,
          "a port it cannot be: said in the dialog, which stays");
    w.screen->input(
      "\x7f\x7f"
      "5000\r");
    CHECK(!w.app->dialog->open && lastAct<action::ControlPort>(w) != nullptr
            && lastAct<action::ControlPort>(w)->text == "5000",
          "Enter hands the port over");
    CHECK(clickText(w, "✎ Address …") && contains(w.text(), "127.0.0.1")
            && contains(w.text(), "this machine only") && contains(w.text(), "0.0.0.0"),
          "the bind address: this machine only, every interface, ...");
    CHECK(clickText(w, "every interface: the network too")
            && lastAct<action::BindAddress>(w) != nullptr
            && lastAct<action::BindAddress>(w)->text == "0.0.0.0" && !w.app->dialog->open,
          "a click on a choice answers with it");
}

void debuggerPage() {
    Window w{10};
    w.resize(140, 40);
    TargetPort::Probe probe;
    probe.serialNumber = 123;
    probe.product      = "J-Link";
    probe.nickName     = "left";
    probe.onUsb        = true;
    w.app->printer.probes.push_back(probe);
    w.screen->input("5");
    CHECK(contains(w.text(), "○ not running") && contains(w.text(), "○ USB J-Link 123 \"left\""),
          "5: the debugger, the probes on USB");
    CHECK(clickText(w, "⏸ Halt") && lastAct<action::Core>(w) != nullptr
            && std::holds_alternative<target_op::Halt>(lastAct<action::Core>(w)->op),
          "halt: a core op for the printer");
    CHECK(clickText(w, "○ 1 core") && contains(w.text(), "◉ 1 core")
            && lastAct<action::Core>(w) != nullptr,
          "a reset type: chosen and set");
    CHECK(clickText(w, "○ USB J-Link 123") && clickText(w, "⏎ Connect")
            && lastAct<action::Connect>(w) != nullptr && lastAct<action::Connect>(w)->probe == "123"
            && lastAct<action::Connect>(w)->host.empty(),
          "a probe picked, then connect: that probe on USB");
    CHECK(clickText(w, "✎ Name …") && contains(w.text(), "🔌 The J-Link on USB")
            && contains(w.text(), "the only J-Link on USB"),
          "name …: the probes on USB to choose");
    CHECK(clickText(w, "J-Link 123 \"left\"  on USB") && !w.app->dialog->open
            && contains(w.text(), "◉ USB J-Link 123"),
          "a click on one: that probe");
    CHECK(clickText(w, "○ IP") && clickText(w, "✎ Address …"), "IP: its host asked for");
    w.screen->input("192.0.2.7\r");
    CHECK(clickText(w, "⏎ Connect") && lastAct<action::Connect>(w) != nullptr
            && lastAct<action::Connect>(w)->host == "192.0.2.7",
          "connect over IP");
}

void healthPage() {
    Window w{10};
    w.resize(140, 60);
    auto s            = std::make_shared<TargetInspector::Snapshot>();
    s->symbolCount    = 10;
    s->lldMap         = true;
    s->havePanic      = true;
    w.app->inspection = s;
    w.screen->input("8");
    CHECK(w.app->inspectorPage() == TargetInspector::Page::health && contains(w.text(), "Health"),
          "8: the health page, read by the inspector");
    CHECK(clickText(w, "○ abort") && contains(w.text(), "◉ abort"), "a panic cause chosen");
    CHECK(clickText(w, "Raise a panic …") && contains(w.text(), "Raise it now"),
          "the first click arms");
    CHECK(clickText(w, "Raise it now") && lastAct<action::RaisePanic>(w) != nullptr
            && lastAct<action::RaisePanic>(w)->cause == 1,
          "the second raises the chosen cause");
    CHECK(contains(w.text(), "empty: no panic") && contains(w.text(), "📕 Fault record"),
          "the records, each in its box");
}

void inspectPages() {
    Window w{10};
    w.resize(140, 40);
    w.app->inspection = std::make_shared<TargetInspector::Snapshot const>();
    w.screen->input("9");
    CHECK(w.app->inspectorPage() == TargetInspector::Page::trace
            && contains(w.text(), "no Kvasir::Trace::Ring"),
          "9: inspect, the trace rings first");
    w.screen->input("\x1b[C");   // right: the watches
    CHECK(w.app->inspectorPage() == TargetInspector::Page::watch
            && contains(w.text(), "Watches · 0"),
          "→ the watches");
    CHECK(clickText(w, "➕ Watch …") && contains(w.text(), "👁 Watch a data symbol")
            && contains(w.text(), "type part of a name"),
          "watch …: a dialog asks for a symbol");
    w.screen->input("cou");
    CHECK(contains(w.text(), "1 match") && contains(w.text(), "counter  4 B @0x20000000"),
          "the matches follow what is typed");
    w.screen->input("\r");
    CHECK(lastAct<action::Watch>(w) != nullptr && lastAct<action::Watch>(w)->name == "counter"
            && !w.app->dialog->open,
          "Enter watches the marked match");
    CHECK(clickText(w, "➕ Watch …"), "again");
    w.screen->input("cou");
    w.acts.clear();
    CHECK(clickText(w, "counter  4 B") && lastAct<action::Watch>(w) != nullptr,
          "a click on a match watches it");
    w.screen->input("\x1b[C");
    CHECK(clickText(w, "▶ Start") && lastAct<action::Profile>(w) != nullptr, "the profile: start");
    CHECK(clickText(w, "🧵 Trace") && w.app->inspectorPage() == TargetInspector::Page::trace,
          "the title's buttons pick the page");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {      "messagesPanel",       messagesPanel},
                               {     "helpAndDetails",      helpAndDetails},
                               {        "filterPanel",         filterPanel},
                               {  "filterFromTheLine",   filterFromTheLine},
                               {       "metricsPanel",        metricsPanel},
                               { "metricsWindowGrows",  metricsWindowGrows},
                               {"metricsTableScrolls", metricsTableScrolls},
                               {       "settingsPage",        settingsPage},
                               {       "debuggerPage",        debuggerPage},
                               {         "healthPage",          healthPage},
                               {       "inspectPages",        inspectPages}
    });
}
