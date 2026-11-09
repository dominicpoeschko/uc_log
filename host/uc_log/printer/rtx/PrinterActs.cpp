#include "uc_log/printer/rtx/PrinterActs.hpp"

#include "uc_log/printer/InspectorText.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"

#include <array>
#include <fmt/format.h>
#include <functional>
#include <string>
#include <utility>
#include <variant>

namespace uc_log::printer::rtx {

namespace {
    // a notice in the window, if it takes them
    void tell(PrinterActs::Parts const& parts,
              std::string               text) {
        if(parts.notify) { parts.notify(std::move(text)); }
    }

    // the action as the printer does it: what a posted job needs it captures from the parts (the visitor is gone by
    // then)
    struct Act {
        PrinterActs::Parts const& p;

        void onTarget(std::function<void(TargetPort&)> job) const {
            if(p.onTarget) { p.onTarget(std::move(job)); }
        }

        // a model change after the event that asked for it (it may rebind sockets): on the UI thread, as a task
        static void onModel(std::function<void()> job) { ::rtxui::PostTask(std::move(job)); }

        void operator()(action::Core const& c) const {
            onTarget([&messages = p.messages, op = c.op](TargetPort& t) {
                std::array<TargetOp, 1> const ops{op};
                if(auto const r = t.accessTarget(ops, TargetPort::defaultTimeout); !r) {
                    messages.toolError(fmt::format("{}: {}", describe(op), r.error()));
                }
            });
        }

        void operator()(action::ResetProbe const&) const {
            onTarget([](TargetPort& t) { t.resetJLink(); });
        }

        void operator()(action::ListProbes const&) const {
            onTarget([](TargetPort& t) { t.refreshProbes(); });
        }

        void operator()(action::Connect const& c) const {
            onTarget([s = TargetPort::ConnectionSettings{c.host, c.probe}](TargetPort& t) {
                t.setConnection(s);
            });
        }

        void operator()(action::NoLogTimeout const& n) const {
            onTarget([s = n.seconds](TargetPort& t) { t.setNoLogTimeout(s); });
        }

        void operator()(action::ToggleLogFile const&) const {
            onModel([&m = p.model] { m.toggleLogFile(); });
        }

        void operator()(action::LogDir const& d) const {
            onModel([&m = p.model, dir = d.dir] { m.applyLogDir(dir); });
        }

        void operator()(action::ControlOn const& o) const {
            onModel([&m = p.model, on = o.on] { m.setControlEnabled(on); });
        }

        void operator()(action::ControlPort const& c) const {
            onModel([&parts = p, text = c.text] {
                if(!parts.model.applyControlPort(text)) {
                    tell(parts, fmt::format("not a port (1024-65535): {:?}", text));
                }
            });
        }

        void operator()(action::Transport const& t) const {
            onModel([&m = p.model, unixSockets = t.unixSockets] { m.setTransport(unixSockets); });
        }

        void operator()(action::BindAddress const& b) const {
            onModel([&parts = p, text = b.text] {
                if(!parts.model.applyBindAddress(text)) {
                    tell(parts, fmt::format("cannot bind to {:?}", text));
                }
            });
        }

        void operator()(action::ToggleDuplex const& d) const {
            onModel([&m = p.model, ordinal = d.ordinal] { m.toggleDuplex(ordinal); });
        }

        void operator()(action::DuplexPort const& d) const {
            onModel([&parts = p, ordinal = d.ordinal, text = d.text] {
                if(!parts.model.applyDuplexPort(ordinal, text)) {
                    tell(parts, fmt::format("not a port (1024-65535): {:?}", text));
                }
            });
        }

        void operator()(action::DuplexBasePort const& d) const {
            onModel([&parts = p, text = d.text] {
                if(!parts.model.applyDuplexBasePort(text)) {
                    tell(parts, fmt::format("not a port (1024-65535): {:?}", text));
                }
            });
        }

        void operator()(action::ResetStatistics const&) const {
            p.model.resetStatistics();
            p.store.resetLogStats();
        }

        void operator()(action::ReadRecords const&) const {
            if(p.inspector) { p.inspector->refresh(); }
        }

        void operator()(action::ClearRecords const&) const {
            if(p.inspector) { p.inspector->clearRecords(p.halted && p.halted()); }
        }

        void operator()(action::RaisePanic const& r) const {
            if(p.inspector) { p.inspector->raisePanic(r.cause); }
        }

        void operator()(action::Watch const& a) const {
            if(!p.inspector) { return; }
            auto const said = p.inspector->addWatch(a.name);
            tell(p, said.empty() ? fmt::format("watching {}", shortName(a.name)) : said);
        }

        void operator()(action::Unwatch const& a) const {
            if(!p.inspector) { return; }
            p.inspector->removeWatch(a.name);
            tell(p, fmt::format("no longer watching {}", shortName(a.name)));
        }

        void operator()(action::Profile const&) const {
            if(!p.inspector) { return; }
            if(p.inspector->snapshot().profiling) {
                p.inspector->stopProfile();
            } else {
                p.inspector->startProfile();
            }
        }

        void operator()(action::ClearProfile const&) const {
            if(p.inspector) { p.inspector->clearProfile(); }
        }

        // the window's own: the App ran them
        void operator()(action::Run const&) const {}

        void operator()(action::Edit const&) const {}

        void operator()(action::UseUsb const&) const {}

        void operator()(action::PickProbe const&) const {}

        void operator()(action::PanicCause const&) const {}

        void operator()(action::ArmPanic const&) const {}

        void operator()(action::ShowInspect const&) const {}

        void operator()(action::TraceDelta const&) const {}

        void operator()(action::TraceHex const&) const {}
    };
}   // namespace

void PrinterActs::run(WindowAction const& action) const { std::visit(Act{parts_}, action); }

}   // namespace uc_log::printer::rtx
