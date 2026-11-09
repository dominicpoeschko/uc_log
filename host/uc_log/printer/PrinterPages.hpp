#pragma once

// The printer's own pages of a window, for any front end: the settings (the columns, the log file, the sockets and the
// duplex channels), the debugger (the link, the core, the reset type, the connection and the probes) and the session's
// statistics - as boxes of rows whose buttons carry a WindowAction (a PageForm). What they show is a PrinterFacts the
// window took once in its frame and the window's own choices; nothing here reads a clock or talks to the printer.

#include "uc_log/printer/DuplexChannelInfo.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/PageForm.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/WindowActions.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// What the printer's pages show, taken once a frame.
struct PrinterFacts {
    PrinterModel::Snapshot         model;
    std::vector<DuplexChannelInfo> duplex;
    Statistics                     statistics;
    TargetPort::LinkStatus         link;
    bool                           halted{};
    bool                           flashing{};
    std::vector<TargetPort::Probe> probes;
};

/// The debugger page's choices, the window's until it connects: USB with a probe, or IP with a host.
struct DebuggerChoice {
    bool          usb{true};
    std::string   probe;   // "" the only J-Link on USB
    std::string   host;
    std::uint8_t  resetType{0};
    std::uint32_t noLogTimeout{15};
};

using Page = PageForm<WindowAction>;

namespace Detail {
    inline PanelPart gap() { return label(" "); }

    inline std::pair<std::string,
                     Tone>
    stateOf(SocketPortStatus status,
            bool             enabled) {
        if(!enabled) { return {"○ off", Tone::plain}; }
        switch(status) {
        case SocketPortStatus::active:       return {"● active", Tone::ok};
        case SocketPortStatus::portOccupied: return {"⚠ port in use", Tone::bad};
        case SocketPortStatus::notStarted:   return {"○ not started", Tone::plain};
        }
        return {"○ not started", Tone::plain};
    }

    inline std::pair<std::string,
                     Tone>
    stateOf(LogFileStatus status,
            bool          enabled) {
        if(!enabled) { return {"○ off", Tone::plain}; }
        switch(status) {
        case LogFileStatus::active:     return {"● writing", Tone::ok};
        case LogFileStatus::error:      return {"✘ error", Tone::bad};
        case LogFileStatus::notStarted: return {"○ not started", Tone::plain};
        }
        return {"○ not started", Tone::plain};
    }

    inline bool loopback(std::string_view address) {
        return address.starts_with("127.") || address == "::1" || address == "localhost";
    }

    inline PanelPart path(std::string text) { return {RunKind::location, 0, {}, std::move(text)}; }
}   // namespace Detail

/// The settings: the log's columns, the log file, the transport, the bind address, the control socket, the duplex
/// channels. A value to type is an Edit (the window asks in a dialog: Questions.hpp), a switch or a choice acts at
/// once.
inline Page settingsForm(PrinterFacts const& f,
                         LogColumns const&   columns) {
    using namespace Detail;
    Page       p;
    auto const run = [](Command c) { return WindowAction{action::Run{c}}; };

    auto& shown = p.box("📋 Columns of the log");
    shown.push_back({p.check("host time", columns.hostTime, run(Command::toggleHostTime)),
                     p.check("target time", columns.targetTime, run(Command::toggleTargetTime)),
                     p.check("channel", columns.channel, run(Command::toggleChannel)),
                     p.check("level", columns.level, run(Command::toggleLevel))});
    shown.push_back({p.check("location", columns.location, run(Command::toggleLocation)),
                     p.check("module", columns.module, run(Command::toggleModule)),
                     p.check("function", columns.function, run(Command::toggleFunction))});

    auto const& m = f.model;
    {
        auto const [state, tone] = stateOf(m.logFileStatus, m.logFileEnabled);
        auto& file               = p.box("📄 Log file");
        file.push_back(
          {toned(tone, state), gap(), path(m.logFilePath.empty() ? "-" : m.logFilePath)});
        if(m.logFileStatus == LogFileStatus::error && m.logFileEnabled) {
            file.push_back({toned(Tone::bad, fmt::format("cannot write {}", m.logFilePath))});
        }
        file.push_back(
          {p.check("write it", m.logFileEnabled, WindowAction{action::ToggleLogFile{}}),
           p.button("📁 Directory …", WindowAction{action::Edit{Field::logDir}})});
    }

    bool const unixSockets = !m.controlSocketPath.empty();
    auto&      transport   = p.box("🔌 Transport");
    transport.push_back(
      {p.option("unix sockets", unixSockets, WindowAction{action::Transport{true}}),
       p.option("TCP ports", !unixSockets, WindowAction{action::Transport{false}})});
    transport.push_back(
      {label(unixSockets ? "control.sock and duplex.<n>.sock in the log directory"
                         : "the control port and the duplex ports on the bind address")});

    bool const exposed = !loopback(m.bindAddress);
    auto&      bind    = p.box("🌐 Bind address");
    bind.push_back(
      {toned(exposed ? Tone::bad : Tone::ok, m.bindAddress),
       label(exposed ? "  exposed: a duplex port is an unauthenticated shell into the target"
                     : "  this host only")});
    bind.push_back({p.option("loopback",
                             m.bindAddress == "127.0.0.1",
                             WindowAction{action::BindAddress{"127.0.0.1"}}),
                    p.option("all interfaces",
                             m.bindAddress == "0.0.0.0",
                             WindowAction{action::BindAddress{"0.0.0.0"}}),
                    p.button("✎ Address …", WindowAction{action::Edit{Field::bindAddress}})});

    {
        auto const [state, tone] = stateOf(m.controlStatus, m.controlEnabled);
        auto const where         = !m.controlSocketPath.empty() ? m.controlSocketPath
                                 : m.controlPort > 0 ? fmt::format("{}:{}", m.bindAddress, m.controlPort)
                                                     : std::string{"-"};
        auto&      control       = p.box("🎛 Control socket");
        control.push_back(
          {toned(tone, state),
           gap(),
           path(where),
           label(
             fmt::format(" · {} client{}", m.controlClients, m.controlClients == 1 ? "" : "s"))});
        control.push_back({p.check("serve it",
                                   m.controlEnabled,
                                   WindowAction{action::ControlOn{!m.controlEnabled}}),
                           p.button("✎ Port …", WindowAction{action::Edit{Field::controlPort}})});
    }

    auto& duplex = p.box("🔁 Duplex channels", true);
    duplex.push_back(
      {label(
         f.duplex.empty()
           ? fmt::format("base port {} - no channel reported by the target", m.duplexBasePort)
           : fmt::format("base port {} - one client a channel, the first wins", m.duplexBasePort)),
       gap(),
       p.button("✎ Base port …", WindowAction{action::Edit{Field::duplexBasePort}})});
    for(auto const& d : f.duplex) {
        auto const [state, tone] = stateOf(d.status, d.enabled);
        PanelRow row{
          label(fmt::format("{} ", d.ordinal)),
          PanelPart{RunKind::module, 0, {}, d.name},
          gap(),
          toned(tone, state),
          gap(),
          path(d.socketPath.empty() ? fmt::format("{}:{}", m.bindAddress, d.port) : d.socketPath),
          label(d.connected ? " · client ● · " : " · client ○ · "),
          label(fmt::format("→uc {}  uc→ {}",
                            shortBytes(d.bytesToTarget),
                            shortBytes(d.bytesFromTarget)))
        };
        if(d.hostToTargetOnly) { row.push_back(toned(Tone::warn, "  host→target only")); }
        if(d.bytesDropped != 0) {
            row.push_back(
              toned(Tone::bad, fmt::format("  {} dropped", shortBytes(d.bytesDropped))));
        }
        row.push_back(gap());
        row.push_back(p.check("on", d.enabled, WindowAction{action::ToggleDuplex{d.ordinal}}));
        row.push_back(p.button("✎ Port …",
                               WindowAction{
                                 action::Edit{Field::duplexPort, d.ordinal}
        }));
        duplex.push_back(std::move(row));
    }
    return p;
}

/// The debugger: the link as the probe reports it, the core (go, halt, breakpoints), the target (reset, flash, the
/// probe itself), the reset type, the connection (USB and its probes, or IP and its host), the no-log reconnect.
inline Page debuggerForm(PrinterFacts const&   f,
                         DebuggerChoice const& choice) {
    using namespace Detail;
    Page       p;
    auto const core = [](TargetOp op) { return WindowAction{action::Core{std::move(op)}}; };

    bool const running = f.link.isRunning != 0;
    auto&      link    = p.box("🔗 Link");
    link.push_back(
      {toned(running ? Tone::ok : Tone::bad, running ? "● running" : "○ not running")});
    link.push_back({label(
      fmt::format("read {} · overflows {}",
                  shortBytes(f.link.numBytesRead),
                  shortCount(static_cast<std::uint64_t>(std::max(0, f.link.hostOverflowCount)))))});
    link.push_back(
      {label(fmt::format("buffers ↑{} ↓{}", f.link.numUpBuffers, f.link.numDownBuffers))});

    std::pair<std::string, Tone> const state
      = f.flashing ? std::pair{std::string{"⚡ flashing"}, Tone::warn}
      : f.halted   ? std::pair{std::string{"⏸ halted"}, Tone::bad}
                   : std::pair{std::string{"▶ running"}, Tone::ok};
    auto& c = p.box("⚙ Core");
    c.push_back({toned(state.second, state.first)});
    c.push_back({p.button("▶ Go", core(target_op::Go{}), ButtonLook::go),
                 p.button("⏸ Halt", core(target_op::Halt{.capture = true}), ButtonLook::danger),
                 p.button("Clear breakpoints", core(target_op::ClearBreakpoints{}))});

    auto& target = p.box("🎯 Target");
    target.push_back(
      {p.button("🔄 Reset  r", WindowAction{action::Run{Command::reset}}),
       p.button("⚡ Flash  f", WindowAction{action::Run{Command::flash}}, ButtonLook::go),
       p.button("🔌 Reset the probe", WindowAction{action::ResetProbe{}}, ButtonLook::danger)});

    static constexpr std::array<std::string_view, 3> resetTypes{"0 normal",
                                                                "1 core",
                                                                "2 reset pin"};
    auto&                                            resets = p.box("🔁 Reset type");
    PanelRow                                         row;
    for(std::size_t i = 0; i < resetTypes.size(); ++i) {
        row.push_back(p.option(resetTypes[i],
                               choice.resetType == i,
                               core(target_op::SetResetType{static_cast<std::uint8_t>(i)})));
    }
    resets.push_back(std::move(row));

    auto& connection = p.box("🔌 Connection", true);
    connection.push_back(
      {p.option("USB", choice.usb, WindowAction{action::UseUsb{true}}),
       p.option("IP", !choice.usb, WindowAction{action::UseUsb{false}}),
       gap(),
       label(choice.usb ? fmt::format("the J-Link {}",
                                      choice.probe.empty() ? std::string{"on USB (the only one)"}
                                                           : fmt::format("\"{}\"", choice.probe))
                        : fmt::format("the J-Link at {}", choice.host.empty() ? "-" : choice.host)),
       gap(),
       p.button("⏎ Connect", WindowAction{action::Connect{}}, ButtonLook::go)});
    if(choice.usb) {
        connection.push_back({p.option("any: the only J-Link on USB",
                                       choice.probe.empty(),
                                       WindowAction{action::PickProbe{}})});
        for(auto const& probe : f.probes) {
            auto const serial = std::to_string(probe.serialNumber);
            auto       text   = fmt::format("{} {}{}",
                                            probe.onUsb ? "USB" : "IP ",
                                            probe.product.empty() ? "" : probe.product + " ",
                                            serial);
            if(!probe.nickName.empty()) { text += fmt::format(" \"{}\"", probe.nickName); }
            if(!probe.onUsb) { text += " (by serial number)"; }
            bool const chosen = choice.probe == serial
                             || (!probe.nickName.empty() && choice.probe == probe.nickName);
            connection.push_back({p.option(text, chosen, WindowAction{action::PickProbe{serial}})});
        }
        connection.push_back({p.button("✎ Name …", WindowAction{action::Edit{Field::probe}}),
                              p.button("🔍 List the probes", WindowAction{action::ListProbes{}})});
    } else {
        connection.push_back({label(choice.host.empty() ? "no host yet" : choice.host),
                              gap(),
                              p.button("✎ Address …", WindowAction{action::Edit{Field::host}})});
    }

    auto& quiet = p.box("⏱ No log");
    quiet.push_back(
      {label(fmt::format("reconnect after {} s without a log line", choice.noLogTimeout))});
    quiet.push_back({p.button("✎ Seconds …", WindowAction{action::Edit{Field::noLogTimeout}})});
    return p;
}

/// The session's numbers since the printer started (or the statistics were reset), `now` for the uptime.
inline Page statisticsForm(PrinterFacts const&                   f,
                           EntryStore::Mirror const&             mirror,
                           std::chrono::system_clock::time_point now) {
    using namespace Detail;
    Page        p;
    auto const& s = f.statistics;
    auto const  up
      = std::chrono::duration_cast<std::chrono::seconds>(now - s.sessionStartTime).count();
    auto& rows = p.box("📊 This session", true);
    rows.push_back(
      {label(fmt::format("up {}h {:02}m {:02}s · J-Link ", up / 3600, (up % 3600) / 60, up % 60)),
       toned(s.lastJLinkState ? Tone::ok : Tone::bad,
             s.lastJLinkState ? "connected" : "not connected"),
       label(" · reconnections "),
       toned(s.jlinkReconnectionCount == 0 ? Tone::plain : Tone::warn,
             std::to_string(s.jlinkReconnectionCount)),
       label(" · disconnections "),
       toned(s.jlinkDisconnectionCount == 0 ? Tone::plain : Tone::warn,
             std::to_string(s.jlinkDisconnectionCount))});
    rows.push_back(
      {label(
         fmt::format("builds: {} started · {} ok · ", s.totalBuildsStarted, s.successfulBuilds)),
       toned(s.failedBuilds == 0 ? Tone::plain : Tone::bad,
             fmt::format("{} failed", s.failedBuilds)),
       label(s.totalBuildsStarted == 0
               ? std::string{}
               : fmt::format(" · {:.1f} % ok",
                             100.0 * static_cast<double>(s.successfulBuilds)
                               / static_cast<double>(s.totalBuildsStarted)))});
    rows.push_back(
      {label(
         fmt::format("target: flashed {} · resets asked {} · ", s.flashCount, s.resetRequestCount)),
       toned(mirror.logStats.detectedResetCount == 0 ? Tone::plain : Tone::warn,
             fmt::format("resets seen {}", mirror.logStats.detectedResetCount))});
    rows.push_back(
      {label(fmt::format("logs: retained {} · ", shortCount(mirror.originalLogCount))),
       toned(mirror.trimmedLogCount == 0 ? Tone::plain : Tone::warn,
             fmt::format("trimmed {}", shortCount(mirror.trimmedLogCount))),
       label(" · "),
       toned(mirror.logStats.parseFailureCount == 0 ? Tone::plain : Tone::bad,
             fmt::format("unparsed {}", shortCount(mirror.logStats.parseFailureCount))),
       label(fmt::format(" · peak {}/s", shortCount(mirror.logStats.peakLogsPerSecond)))});
    rows.push_back({label(fmt::format("data: most read {} · most overflows {}",
                                      shortBytes(s.maxBytesRead),
                                      shortCount(s.maxOverflowCount))),
                    gap(),
                    p.button("🗑 Reset the statistics",
                             WindowAction{action::ResetStatistics{}},
                             ButtonLook::danger)});
    return p;
}

}   // namespace uc_log::printer
