// The App: its children and what a click on them runs, the sizes of its parts, a frame (the badges, the log, the
// open tab, the mode line), notices, the digest (counted).

#include "uc_log/printer/rtx/App.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

App::App(EntryStore::Mirror const& mirror UC_LOG_LIFETIMEBOUND,
         Hooks                            hooks)
  : status{::rtxui::Ref<StatusLine>::New()}
  , tabs{::rtxui::Ref<TabBar>::New()}
  , log{::rtxui::Ref<LogView>::New()}
  , panelView{::rtxui::Ref<Panel>::New()}
  , mode{::rtxui::Ref<ModeLine>::New()}
  , dialog{::rtxui::Ref<Dialog>::New()}
  , page{::rtxui::Ref<FormPage>::New()}
  , menu{::rtxui::Ref<ContextMenu>::New()}
  , mirror_{mirror}
  , hooks_{std::move(hooks)} {
    // the children are the App's own: the template's tags reuse these instances
    Import("status-line", ::rtxui::ComponentFactory{[c = status] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("tab-bar", ::rtxui::ComponentFactory{[c = tabs] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("log-view", ::rtxui::ComponentFactory{[c = log] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("info-panel", ::rtxui::ComponentFactory{[c = panelView] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("mode-line", ::rtxui::ComponentFactory{[c = mode] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("window-dialog", ::rtxui::ComponentFactory{[c = dialog] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("form-page", ::rtxui::ComponentFactory{[c = page] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("context-menu", ::rtxui::ComponentFactory{[c = menu] {
               return ::rtxui::Ref<::rtxui::ComponentBase>{c};
           }});
    Import("textOpen", &App::textOpen);
    Import("formOpen", &App::formOpen);
    Import("logShown", &App::logShown);
    // what a click runs: the children's onclick hands it here (on the UI thread, inside OnEvent)
    tabs->picked = [this](Tab tab) {
        openPanel(tab);
        relayout();
    };
    auto const tool = [this](std::size_t i) {
        if(i >= buttonActions_.size()) { return; }
        auto const action = buttonActions_[i];   // it may show the panel again, and so replace them
        action();
        relayout();
    };
    panelView->pressed = tool;
    page->pressed      = tool;
    page->changed      = [this] { relayout(); };
    page->scrolledTo   = [this](std::size_t, std::size_t first) {
        // the metrics' table is the one page box that scrolls
        pages_.metricTop    = first;
        pages_.metricFollow = false;
        relayout();
    };
    status->chosen = [this](Command command) { run(command); };
    log->pressed   = [this](std::size_t i) {
        if(i >= logActions_.size()) { return; }
        auto const action = logActions_[i];   // it shows the head again
        action();
        relayout();
    };
    menu->changed = [this] { relayout(); };
    hintCommands_ = {Command::help, Command::palette, Command::search};
    for(auto const c : hintCommands_) {
        auto const& info = infoOf(c);
        auto const  key  = info.keys.substr(0, info.keys.find(' '));
        std::string name{info.name.substr(0, info.name.find_first_of(":("))};
        if(c == Command::palette) { name = "commands"; }
        mode->hints.push_back({"", std::string{key}, fmt::format(" {}  ", name)});
    }
    mode->hints.push_back(
      {"", "q", " quit "});   // a key only: a stray click never ends the printer
    mode->hinted = [this](std::size_t i) {
        if(i < hintCommands_.size()) { run(hintCommands_[i]); }
    };
    dialog->picked = [this](std::size_t row) {
        pickRow(dialogState_.cursor.top + row);
        relayout();
    };
    dialog->pressed = [this](std::size_t i) {
        // the first closes (Cancel, a combo box's Done), the second answers (a question's ✓, the palette's ▶ Run)
        if(i == 1) {
            acceptDialog(true);
        } else {
            closeDialog();
        }
        relayout();
    };
    dialog->closed = [this] {
        closeDialog();
        relayout();
    };
}

void App::setSize(int width,
                  int height) {
    width_  = std::max(width, 10);
    height_ = std::max(height, 3);
    relayout();
}

std::size_t App::panelRows() const {
    if(panel_ == Tab::logs) { return 0; }
    // zoomed: all but the status line, the tabs and the mode line; dragged: that, the log keeps a row
    if(!split_) { return static_cast<std::size_t>(std::max(2, height_ - 3)); }
    if(panelHeight_) {
        // the log keeps its head, its frame and a line
        return static_cast<std::size_t>(std::clamp(*panelHeight_, 3, std::max(3, height_ - 7)));
    }
    // the metrics panel: half the window, a plot needs the rows
    if(panel_ == Tab::metrics || panel_ == Tab::settings || panel_ == Tab::debugger
       || panel_ == Tab::health || panel_ == Tab::inspect)
    {
        return static_cast<std::size_t>(std::clamp(height_ / 2, 8, 30));
    }
    return static_cast<std::size_t>(std::clamp(height_ / 3, 6, 18));
}

std::size_t App::logRows() const {
    // the status line, the tabs and the mode line; zoomed, the log is not drawn but keeps its place (a row or more)
    if(logHidden()) { return static_cast<std::size_t>(std::max(1, height_ - 3)); }
    auto const used = 3 + static_cast<int>(panelRows());
    return static_cast<std::size_t>(std::max(1, height_ - used));
}

std::size_t App::logBody() const {
    return logRows() > 3 ? logRows() - 3 : 1;   // under its head, inside its frame
}

std::size_t App::panelBody() const {
    return panelRows() > 3 ? panelRows() - 3 : 1;   // under its head, inside its frame
}

bool App::logHidden() const { return !split_ && panel_ != Tab::logs; }

void App::show(std::vector<Badge> const& badges) {
    status->show(badges);
    relayout();
}

void App::relayout() {
    tabs->show(panel_, static_cast<std::size_t>(width_));
    // an open combo box takes in what the log brought (a new module, the counts): its rows keep their places; the
    // probes a question lists come from the reader when they come
    if(dialogState_.modal == Modal::modules || dialogState_.modal == Modal::locations
       || (dialogState_.modal == Modal::question && dialogState_.asked == Field::probe))
    {
        fillDialog(true);
    }
    if(logShown == logHidden()) { logShown = !logHidden(); }
    auto const frameWidth
      = static_cast<std::size_t>(std::max(width_ - 2, 2));   // inside the log's frame
    if(logShown) { log->show(mirror_, logBody(), frameWidth); }
    showPanel();
    auto const  lines = SnapshotLines{&mirror_.entries};
    auto const  w     = log->viewport().place(lines, logBody());
    auto const  total = lines.size();
    std::string position;
    if(w.follow) {
        position = fmt::format(" ▼ follow · {} lines", total);
    } else {
        position = fmt::format(" line {} / {} (End: follow)",
                               w.selected ? *w.selected + 1 : w.first + 1,
                               total);
    }
    if(log->hOffset() > 0) { position += fmt::format(" · →{}", log->hOffset()); }
    if(!mode->notice.empty()) { position += "  "; }
    if(mode->position != position) { mode->position = std::move(position); }
    if(logShown) { showLogHead(); }
}

void App::notify(std::string text) {
    mode->notice      = std::move(text);
    auto const serial = ++noticeSerial_;
    if(hooks_.later) {
        hooks_.later(std::chrono::milliseconds{3000}, [this, serial] {
            if(serial == noticeSerial_) {
                mode->notice.clear();
                relayout();
            }
        });
    }
    relayout();
}

bool App::Digest() {
    // the dialog's rows follow what is typed (the field changes without telling anybody)
    if(dialogState_.modal != Modal::none && dialog->query != dialogState_.shownFor) {
        fillDialog(false);
    }
    auto const start   = std::chrono::steady_clock::now();
    bool const changed = Component<App>::Digest();
    ++digestStats_.calls;
    if(changed) { ++digestStats_.changed; }
    digestStats_.seconds
      += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return changed;
}

}   // namespace uc_log::printer::rtx
