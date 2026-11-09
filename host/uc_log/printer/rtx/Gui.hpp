#pragma once

// The printer's window, as main and the demo see it. Built once in the uc_log_rtxui library, so this header names no
// RTXUI type (the Window behind it holds them). It keeps its own store of lines, renders the PrinterModel, sends its
// commands and drives the target through a TargetPort; frames are paced (printer/FramePacer.hpp), so a log flood
// costs at most one frame per interval however many lines arrive.

#include "uc_log/LogFilter.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace uc_log::printer { struct MapSnapshot; }

namespace uc_log::printer::rtx {

class Window;

class Gui {
public:
    using MapSource = std::function<std::shared_ptr<MapSnapshot const>()>;

    explicit Gui(PrinterModel& model UC_LOG_LIFETIMEBOUND);
    ~Gui();
    Gui(Gui const&)            = delete;
    Gui& operator=(Gui const&) = delete;

    /// A decoded line for the log. From any thread.
    void add(std::chrono::system_clock::time_point recvTime,
             printer::LogEntry const&              entry);

    /// Before run(): the firmware's compile-time filter, the map the inspector reads.
    void setCompiledFilter(uc_log::Detail::FilterTable const& table);
    void setMapSource(MapSource source);

    /// Something the window shows changed outside the store and the model's messages. From any thread.
    void triggerRedraw();

    /// Until the window is quit; the exit code.
    int run(TargetPort&        target,
            std::string const& buildCommand,
            std::string const& initialHost,
            std::string const& initialProbe);

private:
    std::unique_ptr<Window> window_;
};

}   // namespace uc_log::printer::rtx
