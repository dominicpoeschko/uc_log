#pragma once

// What a click in a window's panel does, for any front end (a PageForm's actions): a command, a value the window
// asks for in a dialog, a choice only the window holds (the USB or IP connection, the probe, the panic cause, the
// inspect page) - those the window does itself - and what the printer does: the target (core ops, the probe), the model
// (log file, sockets, duplex channels, statistics) and the inspector (records, panic, watches, profile). The window
// hands the printer's ones over as they are, with the values its dialogs or its choices filled in.

#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/TargetOp.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace uc_log::printer {

/// A value the window asks for (a dialog: Questions.hpp says what it shows).
enum class Field : std::uint8_t {
    search,       // the log's search text
    jumpToTime,   // a target time: the first line at or after it marked
    timeFrom,     // the target-time window's start
    timeTo,       // and its end
    saveFilter,   // a file to save the filter to
    loadFilter,   // and to load one from
    logDir,
    controlPort,
    bindAddress,
    duplexBasePort,
    duplexPort,   // the channel: Edit::index
    probe,        // a J-Link's serial number or nickname
    host,         // a J-Link's IP address or host name
    noLogTimeout,
    traceRows,
    watch,          // part of a data symbol's name
    metricFilter,   // words a metric must hold for the metrics' table
};

/// The inspect tab's pages.
enum class InspectPage : std::uint8_t { trace, watch, profile };

namespace action {
    // the window's own
    struct Run {
        Command command;
    };

    struct Edit {
        Field       field;
        std::size_t index{};
    };

    struct UseUsb {
        bool usb;
    };

    struct PickProbe {
        std::string name;   // "" any: the only J-Link on USB
    };

    struct PanicCause {
        std::uint32_t cause;
    };

    struct ArmPanic {};   // the first click; the second is RaisePanic

    struct ShowInspect {
        InspectPage page;
    };

    struct TraceDelta {};

    struct TraceHex {};

    // the printer's: the target and the probe
    struct Core {
        TargetOp op;
    };

    struct ResetProbe {};

    struct ListProbes {};

    struct Connect {
        std::string host;    // not empty: J-Link over IP
        std::string probe;   // else: this J-Link on USB ("" the only one)
    };

    struct NoLogTimeout {
        std::uint32_t seconds;
    };

    // the model
    struct ToggleLogFile {};

    struct LogDir {
        std::string dir;
    };

    struct ControlOn {
        bool on;
    };

    struct ControlPort {
        std::string text;
    };

    struct Transport {
        bool unixSockets;
    };

    struct BindAddress {
        std::string text;
    };

    struct ToggleDuplex {
        std::size_t ordinal;
    };

    struct DuplexPort {
        std::size_t ordinal;
        std::string text;
    };

    struct DuplexBasePort {
        std::string text;
    };

    struct ResetStatistics {};

    // the inspector
    struct ReadRecords {};

    struct ClearRecords {};

    struct RaisePanic {
        std::uint32_t cause;
    };

    struct Watch {
        std::string name;
    };

    struct Unwatch {
        std::string name;
    };

    struct Profile {};   // start, or stop when it runs

    struct ClearProfile {};
}   // namespace action

using WindowAction = std::variant<action::Run,
                                  action::Edit,
                                  action::UseUsb,
                                  action::PickProbe,
                                  action::PanicCause,
                                  action::ArmPanic,
                                  action::ShowInspect,
                                  action::TraceDelta,
                                  action::TraceHex,
                                  action::Core,
                                  action::ResetProbe,
                                  action::ListProbes,
                                  action::Connect,
                                  action::NoLogTimeout,
                                  action::ToggleLogFile,
                                  action::LogDir,
                                  action::ControlOn,
                                  action::ControlPort,
                                  action::Transport,
                                  action::BindAddress,
                                  action::ToggleDuplex,
                                  action::DuplexPort,
                                  action::DuplexBasePort,
                                  action::ResetStatistics,
                                  action::ReadRecords,
                                  action::ClearRecords,
                                  action::RaisePanic,
                                  action::Watch,
                                  action::Unwatch,
                                  action::Profile,
                                  action::ClearProfile>;

}   // namespace uc_log::printer
