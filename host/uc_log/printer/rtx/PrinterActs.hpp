#pragma once

// What a page's button asks the printer to do (printer/WindowActions.hpp, the printer's side): a core op or the
// probe's on the worker (it may wait for the probe), the model's after the event that asked for it (it may rebind
// sockets), the inspector's at once (it reads on a thread of its own). The window's own actions - the App ran them -
// do nothing here.

#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/PrinterModel.hpp"
#include "uc_log/printer/TargetInspector.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/WindowActions.hpp"

#include <functional>
#include <string>
#include <utility>

namespace uc_log::printer::rtx {

class PrinterActs {
public:
    /// What the actions act on, and how the window lets them.
    struct Parts {
        PrinterModel&         model;
        EntryStore&           store;
        MessageLog&           messages;
        TargetInspector*      inspector{};   // none: its actions do nothing
        std::function<bool()> halted;        // the core is halted now
        std::function<void(std::function<void(TargetPort&)>)> onTarget;   // a job for the worker
        std::function<void(std::string)> notify;   // a notice (on the UI thread)
    };

    explicit PrinterActs(Parts parts) : parts_{std::move(parts)} {}

    /// On the UI thread, as the click came.
    void run(WindowAction const& action) const;

private:
    Parts parts_;
};

}   // namespace uc_log::printer::rtx
