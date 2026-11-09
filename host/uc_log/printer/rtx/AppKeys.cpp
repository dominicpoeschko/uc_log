// The App: an event - to the dialog or the menu while open, a key to a command (the tab keys, the vim keys), the
// focus ring while Tab moves it.

#include "uc_log/printer/rtx/App.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace uc_log::printer::rtx {

bool App::OnEvent(::rtxui::Event event) {
    using Keyboard = ::rtxui::Event::Keyboard;
    if(auto const r = event.get_if<::rtxui::Event::Resized>()) {
        setSize(r->width(), r->height());
        return ComponentBase::OnEvent(event);
    }
    auto const k = event.get_if<Keyboard>();
    if(k && k->motion() == Keyboard::Released) { return k->special() == Keyboard::Escape; }
    // the focus is painted while Tab moves it, not after a click and a key (RTXUI's :focus-visible would)
    if(k) {
        auto const special = k->special();
        bool const space   = special == Keyboard::None && k->codepoint() == ' ';
        if(special == Keyboard::Tab || special == Keyboard::TabReverse) {
            setRing(true);
        } else if(special != Keyboard::Return && !space) {
            setRing(false);
        }
    } else if(auto const m = event.get_if<::rtxui::Event::Mouse>();
              m && m->motion() == ::rtxui::Event::Mouse::Pressed)
    {
        setRing(false);
    }

    if(dialogState_.modal != Modal::none) { return dialogEvent(event); }
    if(menu->open) {
        // the menu's items take their own clicks; a key or a press elsewhere closes it
        auto const m = event.get_if<::rtxui::Event::Mouse>();
        if(k || (m && m->motion() == ::rtxui::Event::Mouse::Pressed)) {
            menu->open = false;
            return true;
        }
        return ComponentBase::OnEvent(event);
    }
    if(k) {
        // a page's buttons: Tab moves between them, Enter or Space presses the focused one - the screen does both,
        // for a key the App leaves alone
        bool const space = k->special() == Keyboard::None && k->codepoint() == ' ';
        if(k->special() == Keyboard::Tab || k->special() == Keyboard::TabReverse
           || (formOpen && (k->special() == Keyboard::Return || space)))
        {
            return ComponentBase::OnEvent(event);
        }
        if(panel_ == Tab::metrics && metricsKey(*k)) { return true; }
        if(panel_ == Tab::inspect
           && (k->special() == Keyboard::ArrowLeft || k->special() == Keyboard::ArrowRight))
        {
            // the inspect pages, as the metrics' ranges
            auto const n    = std::size_t{3};
            auto const at   = static_cast<std::size_t>(pages_.inspect);
            auto const next = k->special() == Keyboard::ArrowLeft ? (at + n - 1) % n : (at + 1) % n;
            pages_.inspect  = static_cast<InspectPage>(next);
            relayout();
            return true;
        }
        if(key(*k)) { return true; }
        // an Escape nobody takes ends RTXUI's loop: here it closes what is open, or means nothing
        if(k->special() == Keyboard::Escape) {
            if(panel_ != Tab::logs) { run(Command::closePanel); }
            return true;
        }
    }
    if(auto const m = event.get_if<::rtxui::Event::Mouse>()) {
        if(mouse(*m)) {
            relayout();
            return true;
        }
    }
    return ComponentBase::OnEvent(event);
}

void App::setRing(bool on) {
    if(ring_ == on) { return; }
    ring_ = on;
    std::string const cls{on ? "ring" : ""};
    page->ring      = cls;
    panelView->ring = cls;
    log->ring       = cls;
    for(auto& box : page->boxes) { box->ring = cls; }
}

bool App::key(::rtxui::Event::Keyboard const& k) {
    using Keyboard                 = ::rtxui::Event::Keyboard;
    auto const             special = k.special();
    std::optional<Command> command;
    if(special == Keyboard::ArrowUp) {
        command = Command::lineUp;
    } else if(special == Keyboard::ArrowDown) {
        command = Command::lineDown;
    } else if(special == Keyboard::PageUp) {
        command = Command::pageUp;
    } else if(special == Keyboard::PageDown) {
        command = Command::pageDown;
    } else if(special == Keyboard::Home) {
        command = Command::oldest;
    } else if(special == Keyboard::End) {
        command = Command::follow;
    } else if(special == Keyboard::ArrowLeft) {
        command = Command::shiftLeft;
    } else if(special == Keyboard::ArrowRight) {
        command = Command::shiftRight;
    } else if(special == Keyboard::Return) {
        command = Command::details;
    } else if(special == Keyboard::None && k.modifier().ctrl()
              && (k.codepoint() == 'p' || k.codepoint() == 'P'))
    {
        command = Command::palette;
    } else if(special == Keyboard::None && !k.modifier().ctrl() && !k.modifier().alt()) {
        if(auto const tab = tabForKey(k.codepoint())) {
            openPanel(*tab);
            relayout();
            return true;
        }
        command = commandForKey(k.codepoint());
    }
    if(!command) { return false; }
    run(*command);
    return true;
}

}   // namespace uc_log::printer::rtx
