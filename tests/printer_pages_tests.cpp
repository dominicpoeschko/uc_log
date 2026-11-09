// The front-end-neutral pages of the printer and the inspector (PrinterPages.hpp, InspectorPages.hpp,
// InspectorText.hpp): their rows, what a button on them does, a body appended to another, the gauge, a watched value,
// a long name shortened, the health records, a trace ring's table, the watches, the profile. No window, no clock: the
// times are given.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/printer/InspectorPages.hpp"
#include "uc_log/printer/InspectorText.hpp"
#include "uc_log/printer/PrinterPages.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace uc_log::printer;

namespace {

std::string rowText(PanelRow const& row) {
    std::string out;
    for(auto const& p : row) { out += p.text; }
    return out;
}

bool has(Page const&      p,
         std::string_view text) {
    for(auto const& box : p.boxes) {
        if(box.legend.contains(text)) { return true; }
        for(auto const& r : box.rows) {
            if(rowText(r).contains(text)) { return true; }
        }
    }
    return false;
}

// the action of the first button (the page's tools, then its boxes) whose text holds `text`
std::optional<WindowAction> actionOf(Page const&      p,
                                     std::string_view text) {
    for(auto const& part : p.tools) {
        if(part.action != 0 && part.text.contains(text)) { return p.actions.at(part.action - 1U); }
    }
    for(auto const& box : p.boxes) {
        for(auto const& r : box.rows) {
            for(auto const& part : r) {
                if(part.action != 0 && part.text.contains(text)) {
                    return p.actions.at(part.action - 1U);
                }
            }
        }
    }
    return std::nullopt;
}

PrinterFacts facts() {
    PrinterFacts f;
    f.model.controlSocketPath = "/run/a/control.sock";
    f.model.controlEnabled    = true;
    f.model.controlStatus     = SocketPortStatus::active;
    f.model.controlClients    = 1;
    f.model.bindAddress       = "0.0.0.0";
    f.model.logFilePath       = "/logs/a.rttlog";
    f.model.logFileStatus     = LogFileStatus::error;
    f.model.duplexBasePort    = 34600;
    DuplexChannelInfo d;
    d.ordinal      = 0;
    d.name         = "shell";
    d.port         = 34600;
    d.status       = SocketPortStatus::active;
    d.connected    = true;
    d.bytesDropped = 2048;
    f.duplex.push_back(d);
    TargetPort::Probe p;
    p.serialNumber = 123;
    p.product      = "J-Link";
    p.nickName     = "left";
    p.onUsb        = true;
    f.probes.push_back(p);
    return f;
}

void settingsRows() {
    LogColumns columns;
    columns.module = true;
    auto const b   = settingsForm(facts(), columns);
    CHECK(has(b, "✔ host time") && has(b, "✔ module") && has(b, "✘ function"),
          "the columns as switches");
    CHECK(has(b, "cannot write /logs/a.rttlog") && has(b, "◉ unix sockets")
            && has(b, "exposed: a duplex port"),
          "the log file's error, the transport, a bind address that is not loopback");
    CHECK(has(b, "/run/a/control.sock · 1 client") && has(b, "shell") && has(b, "2.0KB dropped"),
          "the control socket, the duplex channel");
    auto const column = actionOf(b, "function");
    CHECK(column && std::get_if<action::Run>(&*column) != nullptr
            && std::get<action::Run>(*column).command == Command::toggleFunction,
          "a column's switch runs its command");
    auto const tcp = actionOf(b, "TCP ports");
    CHECK(tcp && std::get_if<action::Transport>(&*tcp) != nullptr
            && !std::get<action::Transport>(*tcp).unixSockets,
          "TCP ports: the transport");
    auto const port = actionOf(b, "Port …");
    CHECK(port && std::get_if<action::Edit>(&*port) != nullptr
            && std::get<action::Edit>(*port).field == Field::controlPort,
          "Port …: a value for the prompt");
}

void debuggerRows() {
    auto           f = facts();
    DebuggerChoice c;
    c.probe = "left";
    auto b  = debuggerForm(f, c);
    CHECK(has(b, "○ not running") && has(b, "▶ running") && has(b, "◉ USB J-Link 123 \"left\""),
          "the link, the core, the probe chosen by its nickname");
    auto const pick = actionOf(b, "J-Link 123");
    CHECK(pick && std::get_if<action::PickProbe>(&*pick) != nullptr
            && std::get<action::PickProbe>(*pick).name == "123",
          "a probe picked by its serial number");
    auto const halt = actionOf(b, "Halt");
    CHECK(halt && std::get_if<action::Core>(&*halt) != nullptr
            && std::holds_alternative<target_op::Halt>(std::get<action::Core>(*halt).op),
          "halt: a core op");
    f.halted = true;
    c.usb    = false;
    c.host   = "192.0.2.7";
    b        = debuggerForm(f, c);
    CHECK(has(b, "⏸ halted") && has(b, "the J-Link at 192.0.2.7") && !has(b, "J-Link 123"),
          "halted; over IP the host instead of the probes");
}

void statisticsRows() {
    PrinterFacts f;
    f.statistics.sessionStartTime   = std::chrono::system_clock::time_point{};
    f.statistics.totalBuildsStarted = 4;
    f.statistics.successfulBuilds   = 3;
    f.statistics.failedBuilds       = 1;
    EntryStore::Mirror mirror;
    auto const         b
      = statisticsForm(f,
                       mirror,
                       std::chrono::system_clock::time_point{} + std::chrono::seconds{3725});
    CHECK(has(b, "up 1h 02m 05s") && has(b, "builds: 4 started · 3 ok · 1 failed · 75.0 % ok"),
          "the uptime, the builds");
    auto const reset = actionOf(b, "Reset the statistics");
    CHECK(reset && std::holds_alternative<action::ResetStatistics>(*reset), "its reset");
}

void appendKeepsActions() {
    Page a;
    a.box("one").push_back({a.button("one", WindowAction{action::ListProbes{}})});
    Page b;
    b.box("two").push_back(
      {b.button("two", WindowAction{action::ResetProbe{}}, ButtonLook::danger)});
    b.tool("tool", WindowAction{action::ReadRecords{}});
    a.append(std::move(b));
    CHECK(a.boxes.size() == 2 && a.actions.size() == 3 && a.boxes[1].rows[0][0].action == 2
            && std::holds_alternative<action::ResetProbe>(a.actions[1]),
          "the appended box's button still resets the probe");
    CHECK(a.tools.size() == 1 && a.tools[0].action == 3
            && std::holds_alternative<action::ReadRecords>(a.actions[2]),
          "and its tool still reads the records");
    auto const detail = a.boxes[1].rows[0][0].detail;
    CHECK(buttonLook(detail) == ButtonLook::danger && !buttonOn(detail)
            && buttonTone(detail) == Tone::plain,
          "a button keeps its look");
}

void gaugeValueName() {
    CHECK_EQ(gaugeText(0.5, 4), std::string{"██░░"}, "half of four cells");
    CHECK_EQ(gaugeText(1.0 / 16.0, 2), std::string{"▏░"}, "an eighth of a cell");
    CHECK_EQ(watchValue({std::byte{0xff}}),
             std::string{"255 / -1 / 0xff"},
             "a byte: unsigned, signed, hex");
    CHECK_EQ(watchValue({std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3f}}),
             std::string{"1065353216 / 1065353216 / 0x3f800000 / 1f"},
             "four bytes: also as a float");
    std::string const deep = "ns::Ring<" + std::string(200, 'x') + "<int>>::buffer";
    auto const        cut  = shortName(deep);
    CHECK(cut.size() < deep.size() && cut.contains("…"), "a long nested name is cut");
}

void healthRows() {
    Inspection s;
    s.symbolCount = 10;
    s.lldMap      = true;
    s.haveUbsan   = true;
    s.ubsan       = records::Ubsan{.count = 2, .lastReturnAddress = 0x1000'0100};
    s.ubsanWhere  = "f()";
    s.havePanic   = true;
    Inspection::Stack st;
    st.name = "main";
    st.low  = 0x2000'0000;
    st.high = 0x2000'1000;
    st.use  = records::StackUse{.size = 4096, .used = 3900, .never = 196};
    s.stacks.push_back(st);
    HealthChoice c;
    auto         b = healthForm(s, false, c);
    CHECK(has(b, "symbols from the linker map: 10 (lld)") && has(b, "2 since boot")
            && has(b, "0x10000100  f()"),
          "the map, the sanitizer count and where");
    CHECK(has(b, "not in this image (no Fault::HandlerFor)") && has(b, "empty: no panic")
            && has(b, "95.2 %  3900 of 4096 B used"),
          "no fault record, an empty panic record, a stack nearly full");
    auto const read = actionOf(b, "Read now");
    CHECK(read && std::holds_alternative<action::ReadRecords>(*read) && !b.tools.empty(),
          "read now: a tool of the page");
    auto const arm = actionOf(b, "Raise a panic");
    CHECK(arm && std::holds_alternative<action::ArmPanic>(*arm), "the first click arms");
    c.panicCause     = 1;
    c.panicArmed     = true;
    b                = healthForm(s, true, c);
    auto const raise = actionOf(b, "Raise it now");
    CHECK(raise && std::get_if<action::RaisePanic>(&*raise) != nullptr
            && std::get<action::RaisePanic>(*raise).cause == 1 && has(b, "◉ abort")
            && has(b, "to leave it"),
          "armed: the second raises the chosen cause; halted: the way out");
}

void traceRows() {
    Inspection       s;
    Inspection::Ring r;
    r.name     = "events";
    r.fields   = {"t", "v"};
    r.count    = 3;
    r.capacity = 8;
    r.rows     = {
      {.index = 1,   .values = {100, 7}},
      {.index = 2,   .values = {130, 8}},
      {.index = 3, .values = {175, 255}}
    };
    s.rings.push_back(r);
    TraceChoice c;
    c.delta = true;
    c.hex   = true;
    c.rows  = 2;
    auto b  = traceForm(s, c);
    CHECK(has(b, "events: 3 written, capacity 8, the last 2") && has(b, "Δ t"),
          "the ring, its delta column");
    CHECK(has(b, "0x82") && has(b, "+30") && has(b, "0xff") && has(b, "+45"),
          "hex values, each delta from the row before (the first shown has one too)");
}

void watchRows() {
    Inspection        s;
    Inspection::Watch w;
    w.name       = "counter";
    w.address    = 0x2000'0010;
    w.size       = 4;
    w.symbolSize = 4;
    w.value      = {std::byte{5}, std::byte{0}, std::byte{0}, std::byte{0}};
    w.changes    = 3;
    w.changed    = Clock::time_point{} + std::chrono::seconds{10};
    s.watches.push_back(w);
    auto const b   = watchForm(s, Clock::time_point{} + std::chrono::seconds{10});
    auto const add = actionOf(b, "Watch …");
    CHECK(add && std::get_if<action::Edit>(&*add) != nullptr
            && std::get<action::Edit>(*add).field == Field::watch,
          "➕ Watch … asks for a symbol (its matches: the dialog's)");
    auto const drop = actionOf(b, "✖");
    CHECK(drop && std::get_if<action::Unwatch>(&*drop) != nullptr
            && std::get<action::Unwatch>(*drop).name == "counter",
          "✖ un-watches it");
    CHECK(has(b, "5 / 5 / 0x00000005") && has(b, "3 change(s)"),
          "the watch: its value, its changes");
}

void profileRows() {
    Inspection s;
    s.profiling      = true;
    s.profileSamples = 200;
    s.profileSeconds = 2.0;
    s.profile        = {
      {.name = "loop", .samples = 150},
      { .name = "isr",  .samples = 50}
    };
    auto const b = profileForm(s, 10);
    CHECK(has(b, "● sampling, 200 samples over 2.0 s (100/s)") && has(b, "75.0 %")
            && has(b, "loop"),
          "sampling: the samples, a function's share");
    auto const stop = actionOf(b, "Stop");
    CHECK(stop && std::holds_alternative<action::Profile>(*stop), "stop while it runs");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {      "settingsRows",       settingsRows},
                               {      "debuggerRows",       debuggerRows},
                               {    "statisticsRows",     statisticsRows},
                               {"appendKeepsActions", appendKeepsActions},
                               {    "gaugeValueName",     gaugeValueName},
                               {        "healthRows",         healthRows},
                               {         "traceRows",          traceRows},
                               {         "watchRows",          watchRows},
                               {       "profileRows",        profileRows},
    });
}
