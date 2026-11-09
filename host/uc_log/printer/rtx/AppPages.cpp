// The App: the printer's pages (settings, debugger, health, inspect) and a click on their buttons - the window's
// own actions here, the printer's handed on (Hooks::act).

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/rtx/HeadFit.hpp"

#include <chrono>
#include <fmt/format.h>
#include <string>
#include <variant>

namespace uc_log::printer::rtx {

void App::showPage() {
    auto height = fmt::format("height: {};", panelRows());
    if(page->height != height) { page->height = std::move(height); }
    auto const run = [](std::function<void()> const& f) { f(); };
    // every tab's head: its icon and name, what it shows; dimmed, what it is for or its keys
    auto const& info = infoOf(panel_);
    auto const  head = [&](std::string_view what) {
        return what.empty() ? fmt::format("{} {}", info.icon, info.name)
                            : fmt::format("{} {} · {}", info.icon, info.name, what);
    };
    std::string title;
    std::string hint;
    if(panel_ == Tab::filter) {
        auto const what = filterSummary(filter_, timeWindow());
        page->fill(filterForm(filter_, filterSources()),
                   [this](FilterItem const& item) { filterAction(item); });
        title = head(what.empty() ? std::string{"everything shown"} : what);
        hint  = " · a click switches, Choose … picks modules and locations";
    } else if(panel_ == Tab::metrics) {
        page->fill(metricsForm(), run);
        title = head(metricsWhat());
        hint  = " · ↑↓ a metric, ←→ the range, a scope's row folds it";
    } else {
        std::string what;
        if(panel_ == Tab::settings) { hint = " · a click switches or chooses, … asks for a value"; }
        if(panel_ == Tab::debugger) {
            what = printer.halted ? "halted" : (printer.flashing ? "flashing" : "");
            hint = " · the link, the core, the probe";
        }
        if(panel_ == Tab::health) {
            what = printer.halted ? "the core is halted" : "";
            hint = " · the records a crash leaves";
        }
        if(panel_ == Tab::inspect) { hint = " · ←→ or the buttons pick the page"; }
        page->fill(printerPage(), [this](WindowAction const& a) { act(a); });
        title = head(what);
    }
    if(page->title != title) { page->title = std::move(title); }
    showButtons();
    hint = fitHint(page->title, std::move(hint), page->tools, width_);
    if(page->hint != hint) { page->hint = std::move(hint); }
}

TargetInspector::Page App::inspectorPage() const {
    using Page = TargetInspector::Page;
    if(panel_ == Tab::health) { return Page::health; }
    if(panel_ != Tab::inspect) { return Page::none; }
    switch(pages_.inspect) {
    case InspectPage::trace:   return Page::trace;
    case InspectPage::watch:   return Page::watch;
    case InspectPage::profile: return Page::profile;
    }
    return Page::none;
}

void App::act(WindowAction const& a) {
    auto const forward = [this](WindowAction const& x) {
        if(hooks_.act) { hooks_.act(x); }
    };
    if(auto const* r = std::get_if<action::Run>(&a)) {
        run(r->command);
        return;
    }
    if(auto const* e = std::get_if<action::Edit>(&a)) {
        ask(e->field, e->index);
        return;
    }
    if(auto const* u = std::get_if<action::UseUsb>(&a)) {
        pages_.debugger.usb = u->usb;
        return;
    }
    if(auto const* p = std::get_if<action::PickProbe>(&a)) {
        pages_.debugger.probe = p->name;
        return;
    }
    if(auto const* c = std::get_if<action::PanicCause>(&a)) {
        pages_.health.panicCause = c->cause;
        pages_.health.panicArmed = false;
        return;
    }
    if(std::holds_alternative<action::ArmPanic>(a)) {
        pages_.health.panicArmed = true;
        return;
    }
    if(auto const* i = std::get_if<action::ShowInspect>(&a)) {
        pages_.inspect = i->page;
        return;
    }
    if(std::holds_alternative<action::TraceDelta>(a)) {
        pages_.trace.delta = !pages_.trace.delta;
        return;
    }
    if(std::holds_alternative<action::TraceHex>(a)) {
        pages_.trace.hex = !pages_.trace.hex;
        return;
    }
    if(auto const* c = std::get_if<action::Core>(&a)) {
        if(auto const* t = std::get_if<target_op::SetResetType>(&c->op)) {
            pages_.debugger.resetType = t->type;
        }
        forward(a);
        notify(describe(c->op));
        return;
    }
    if(std::holds_alternative<action::Connect>(a)) {
        // the page's choice: USB and a probe, or IP and a host
        auto const connect = pages_.debugger.usb ? action::Connect{"", pages_.debugger.probe}
                                                 : action::Connect{pages_.debugger.host, ""};
        forward(WindowAction{connect});
        notify(pages_.debugger.usb
                 ? fmt::format("connecting to the J-Link {}",
                               connect.probe.empty() ? std::string{"on USB"} : connect.probe)
                 : fmt::format("connecting to the J-Link at {}", connect.host));
        return;
    }
    if(std::holds_alternative<action::RaisePanic>(a)) { pages_.health.panicArmed = false; }
    forward(a);
}

Page App::printerPage() const {
    if(panel_ == Tab::settings) { return settingsForm(printer, log->columns()); }
    if(panel_ == Tab::debugger) {
        auto page_ = debuggerForm(printer, pages_.debugger);
        page_.append(statisticsForm(printer, mirror_, std::chrono::system_clock::now()));
        return page_;
    }
    if(!inspection) {
        Page none;
        none.box("🔬 Inspector").push_back({label("no inspector: this window reads no target")});
        return none;
    }
    if(panel_ == Tab::health) { return healthForm(*inspection, printer.halted, pages_.health); }
    switch(pages_.inspect) {
    case InspectPage::trace:   return traceForm(*inspection, pages_.trace);
    case InspectPage::watch:   return watchForm(*inspection, Clock::now());
    case InspectPage::profile: return profileForm(*inspection, 100);
    }
    return {};
}

}   // namespace uc_log::printer::rtx
