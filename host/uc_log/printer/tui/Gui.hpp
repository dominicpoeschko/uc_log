#pragma once

// The printer's FTXUI window, as main and the demo see it: built once in the uc_log_tui library (Gui.cpp), so a
// change of a tab recompiles that library and not every program that opens a window. It renders the PrinterModel,
// sends its commands, drives the target through a TargetPort and owns the inspector (none without a window).

#include "uc_log/LogFilter.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace uc_log::printer { struct MapSnapshot; }   // namespace uc_log::printer

namespace uc_log::printer::tui {

struct Window;

class Gui {
public:
    using MapSource = std::function<std::shared_ptr<printer::MapSnapshot const>()>;

    explicit Gui(PrinterModel& model UC_LOG_LIFETIMEBOUND);
    ~Gui();
    Gui(Gui const&)            = delete;
    Gui& operator=(Gui const&) = delete;

    /// A decoded line for the Logs tab. From any thread.
    void add(std::chrono::system_clock::time_point recvTime,
             printer::LogEntry const&              entry);

    /// Before run(): the firmware's compile-time filter for the Filter tab, the map the inspector reads.
    void setCompiledFilter(uc_log::detail::FilterTable const& table);
    void setMapSource(MapSource source);

    void triggerRedraw();

    /// Until the window is quit; the exit code.
    int run(TargetPort&        target,
            std::string const& buildCommand,
            std::string const& initialHost  = "",
            std::string const& initialProbe = "");

private:
    std::unique_ptr<Window> window_;
};

}   // namespace uc_log::printer::tui
