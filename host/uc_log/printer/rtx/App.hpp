#pragma once

// The RTXUI window: the App and its components. One component per thing that changes on its own, because RTXUI
// renders a component's whole template again when one of its bound values changed:
//
//   App          (here) the frame, the keys, the mouse and the commands (printer/Commands.hpp); its implementation by
//                concern: App.cpp (sizes, a frame, notices), AppPanels.cpp, AppPages.cpp, AppHeads.cpp (the tabs'
//                content and heads), AppDialog.cpp, AppQuestions.cpp, AppFilter.cpp, AppMetrics.cpp, AppCommands.cpp,
//                AppKeys.cpp, AppMouse.cpp
//   StatusLine   the badges (printer/StatusBadges.hpp); a click on one runs its command
//   TabBar       the tabs (printer/Tabs.hpp); a click or its key picks one
//   LogView      the log: its head, its lines (LogLines: plain elements, formatted only when what they show changed),
//                its scrollbar
//   Panel        a text panel: messages, build output, the marked line's details, help (printer/PanelText.hpp)
//   FormPage     (Form.hpp) a page of boxes: filter, settings, debugger, metrics, health, inspect (a neutral PageForm
//                fills it)
//   ModeLine     where the view stands, a notice, the key hints (a click runs one)
//   Dialog       over the window, one at a time: the command palette, a combo box, a question (printer/Questions.hpp)
//   ContextMenu  what a right click on a log row offers, where the mouse is
//
// Their colours are one theme (Theme.hpp), their shared looks one style sheet (Look.hpp), their styled pieces Runs.hpp;
// a head's buttons and hint fitted to its width: HeadFit.hpp.
//
// What a click runs is the template's own `onclick` (tabs, buttons, hints, a dialog's rows) or, where the place in a
// row decides (a log row, a scrollbar), the App's hit test.
//
// Everything here runs on the UI thread. The window (Gui.cpp) pulls the store's mirror, the messages and the build
// output once per paced frame and hands them over; a key press acts at once (RTXUI digests once per burst). What a
// page asks the printer for the window hands to PrinterActs.cpp, a target job to its worker thread (Worker.hpp).

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/BuildRunner.hpp"
#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/FilterPanel.hpp"
#include "uc_log/printer/FilterPicks.hpp"
#include "uc_log/printer/InspectorPages.hpp"
#include "uc_log/printer/ListCursor.hpp"
#include "uc_log/printer/LogFilterState.hpp"
#include "uc_log/printer/LogViewport.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/MetricView.hpp"
#include "uc_log/printer/PageForm.hpp"
#include "uc_log/printer/PrinterPages.hpp"
#include "uc_log/printer/Questions.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/Tabs.hpp"
#include "uc_log/printer/WindowActions.hpp"
#include "uc_log/printer/rtx/ContextMenu.hpp"
#include "uc_log/printer/rtx/Dialog.hpp"
#include "uc_log/printer/rtx/Form.hpp"
#include "uc_log/printer/rtx/LogView.hpp"
#include "uc_log/printer/rtx/ModeLine.hpp"
#include "uc_log/printer/rtx/Panel.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/StatusLine.hpp"
#include "uc_log/printer/rtx/TabBar.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

namespace Detail {
    inline constexpr char appHtml[] = R"html(
<div class="app">
  <status-line/>
  <tab-bar/>
  <if condition="{logShown}"><log-view/></if>
  <if condition="{textOpen}"><info-panel/></if>
  <if condition="{formOpen}"><form-page/></if>
  <mode-line/>
  <window-dialog/>
  <context-menu/>
</div>
<style>
  self { width: 100%; height: 100%; }
  .app { display: flex; flex-direction: column; height: 100%; background-color: var(--bg); }
</style>
)html";

}   // namespace Detail

class App : public ::rtxui::Component<App> {
public:
    struct Hooks {
        std::function<void()>                   quit;
        std::function<void()>                   reset;
        std::function<void()>                   flash;
        std::function<void(bool)>               build;   // true: build and flash
        std::function<void(std::string const&)> search;
        std::function<void()>                   exportText;
        std::function<void()>                   clearMessages;
        std::function<void(FilterState const&)>
          setFilter;   // the store applies it (a refilter of its own)
        std::function<void(EntryStore::UcTimeWindow const&)> setTime;   // the target-time window
        // run `f` on the UI thread after `delay` (a notice's end); none: notices stay
        std::function<void(std::chrono::milliseconds, std::function<void()>)> later;
        // what the printer does for a page's button or prompt (the target, the model, the inspector); none: nothing
        std::function<void(WindowAction const&)> act;
        // the data symbols part of a name matches (the inspector's completions); none: no matches
        std::function<TargetInspector::Completions(std::string const&, std::size_t)> complete;
    };

    App(EntryStore::Mirror const& mirror UC_LOG_LIFETIMEBOUND,
        Hooks                            hooks);

    /// The terminal's size (TIOCGWINSZ at the start, then every Resized).
    void setSize(int width,
                 int height);

    /// A paced frame: the badges (the caller just refreshed the mirror, the messages and the build output).
    void show(std::vector<Badge> const& badges);

    /// Runs a command, from a key, a click or the palette.
    void run(Command command);

    /// A short result in the mode line, gone after a few seconds.
    void notify(std::string text);

    /// The firmware's compile-time filter (its uc_log_filter.txt), for the filter panel's modules; it outlives the App.
    void setCompiledFilter(uc_log::Detail::FilterTable const* table) { compiled_ = table; }

    [[nodiscard]] FilterState const& filter() const UC_LOG_LIFETIMEBOUND { return filter_; }

    /// The store the metrics panel reads (its metric series, under the store's own lock); it outlives the App.
    void setMetricSource(EntryStore* store) { metrics_ = store; }

    /// The open tab (Tab::logs: no panel) and whether it fills the window.
    [[nodiscard]] Tab panel() const { return panel_; }

    /// The page under the log (z) - or, as a tab opens, the whole window.
    [[nodiscard]] bool split() const { return split_; }

    /// What the inspector should read for the open tab (health, a page of inspect, or nothing).
    [[nodiscard]] TargetInspector::Page inspectorPage() const;

    /// A click on a page's button, as the window's own (a command, a prompt, a choice) or handed to Hooks::act.
    void act(WindowAction const& action);

    /// What the open dialog asks for (none: no question is open).
    [[nodiscard]] std::optional<Field> asking() const {
        return dialogState_.modal == Modal::question ? std::optional<Field>{dialogState_.asked}
                                                     : std::nullopt;
    }

    /// The connection the printer was started with (--host / --probe): the debugger page's first choice.
    void setConnection(std::string const& host,
                       std::string const& probe) {
        pages_.debugger.usb   = host.empty();
        pages_.debugger.host  = host;
        pages_.debugger.probe = probe;
    }

    bool OnEvent(::rtxui::Event event) override;

    /// RTXUI's digest of the whole window, counted and timed (UC_LOG_UI_STATS).
    bool Digest() override;

    struct DigestStats {
        std::size_t calls{};
        std::size_t changed{};
        double      seconds{};
    };

    [[nodiscard]] DigestStats digestStats() const { return digestStats_; }

    // what the window hands over every frame
    std::vector<MessageLog::Entry> messageEntries;
    std::vector<BuildEntry>        buildEntries;
    BuildStatus                    buildStatus{BuildStatus::idle};
    PrinterFacts                   printer;   // the settings, debugger and statistics pages
    std::shared_ptr<Inspection const>
      inspection;   // health and inspect (a new one when the inspector's changed)

    ::rtxui::Ref<StatusLine>  status;
    ::rtxui::Ref<TabBar>      tabs;
    ::rtxui::Ref<LogView>     log;
    ::rtxui::Ref<Panel>       panelView;
    ::rtxui::Ref<ModeLine>    mode;
    ::rtxui::Ref<Dialog>      dialog;
    ::rtxui::Ref<FormPage>    page;
    ::rtxui::Ref<ContextMenu> menu;

    std::string_view view{Detail::viewOf<Detail::appHtml>};

private:
    /// What the dialog is open for.
    enum class Modal : std::uint8_t { none, palette, modules, locations, question };

    /// The dialog's state: what it is open for, the field's text its rows are for, the marked row and the first one
    /// shown, its rows by what they are; a question's own.
    struct DialogState {
        static constexpr std::string_view nothingShown{
          "\x01"};   // no text yet: the rows are made anew

        Modal                        modal{Modal::none};
        std::string                  shownFor{nothingShown};
        ListCursor                   cursor;
        std::vector<Command>         commands;   // the palette's matches
        std::vector<PickRow>         picks;      // a combo box's
        std::vector<Choice>          choices;    // a question's
        Field                        asked{Field::search};
        std::size_t                  askedIndex{0};
        std::string                  askedValue;   // the value the question started with
        TargetInspector::Completions symbols;      // the watch question's matches
        std::vector<LocalAddress> addresses;   // this machine's, read as the bind address is asked

        void close() {
            modal = Modal::none;
            cursor.marked.reset();
            commands.clear();
            picks.clear();
            choices.clear();
        }
    };

    /// What the pages chose, kept while the window runs: the debugger's connection, the health page's panic cause,
    /// the trace page's columns, the inspect page shown, the marked metric (by what it is) and its range.
    struct PageChoices {
        DebuggerChoice            debugger;
        HealthChoice              health;
        TraceChoice               trace;
        InspectPage               inspect{InspectPage::trace};
        std::optional<MetricInfo> metric;
        std::size_t               metricRange{0};   // into metricRanges
        std::size_t               metricTop{0};     // the table's first row shown
        bool                      metricFollow{
          true};   // the shown rows go to the mark (it moved); not: they stay where scrolled
        std::size_t           metricRows{0};   // the table's rows the last fill showed (PgUp, PgDn)
        std::string           metricFilter;    // the words a metric of the table holds
        std::set<std::string> foldedScopes;    // the table's scopes shown as one row
        bool                  probesAsked{false};   // the debugger page listed the probes once
    };

    // ---- the window: its parts' sizes, the open tab (App.cpp, AppPanels.cpp)
    EntryStore::Mirror const& mirror_;
    Hooks                     hooks_;
    DigestStats               digestStats_;
    int                       width_{80};
    int                       height_{24};
    bool logShown{true};    // bound: the log is drawn (a page may have the window)
    bool textOpen{false};   // bound: a text panel - messages, build output, the line, help
    bool formOpen{
      false};   // bound: a page of boxes - filter, settings, debugger, metrics, health, inspect
    Tab                panel_{Tab::logs};
    bool               split_{false};   // the page under the log; not: it has the window
    std::optional<int> panelHeight_;    // dragged to (its head too); none: as the tab has it
    std::array<LogViewport, tabCount> panelViews_;   // each text panel's view, by Tab
    std::size_t detailLines_{0};                     // the details panel's rows, for its scrolling

    // ---- the heads (AppHeads.cpp), the mode line, the mouse (AppMouse.cpp) and the keys (AppKeys.cpp)
    std::vector<std::function<void()>>
      buttonActions_;                                 // the open tab's head's tools, as last shown
    std::vector<std::function<void()>> logActions_;   // the log's head's tools
    std::vector<Command>               hintCommands_;   // the mode line's hints
    std::uint64_t noticeSerial_{0};   // the newest notice (an older one's end leaves it)
    bool          dragging_{false};   // a page's head is dragged: it splits
    bool          logDrag_{false};    // the log's scrollbar thumb is dragged
    bool ring_{false};   // Tab moved the focus: it is painted (any other key or a click hides it)

    // ---- the filter (AppFilter.cpp) and the pages (AppPages.cpp, AppMetrics.cpp)
    FilterState filter_;   // what the log shows, as the filter page and the keys set it
    std::string filterPath_{"filter.json"};
    uc_log::Detail::FilterTable const* compiled_{nullptr};
    EntryStore*                        metrics_{nullptr};
    PageChoices                        pages_;

    // ---- the dialog (AppDialog.cpp, AppQuestions.cpp)
    DialogState                               dialogState_;
    std::map<Field, std::vector<std::string>> recent_;   // the answers given, the newest first

    // ---- App.cpp
    [[nodiscard]] std::size_t logRows() const;     // with its head and frame
    [[nodiscard]] std::size_t logBody() const;     // its rows of lines
    [[nodiscard]] bool        logHidden() const;   // a page has the window
    [[nodiscard]] std::size_t panelRows() const;   // with its head
    [[nodiscard]] std::size_t panelBody() const;   // a text panel's rows of text
    void relayout();   // the log, the open tab and the mode line after a key or a frame

    // ---- AppPanels.cpp
    [[nodiscard]] std::size_t panelLines() const;   // what the open text panel has to show
    void                      showPanel();          // the open tab: a page or a text panel
    void                      showTextPanel();      // messages, build output, the line, help
    void                      togglePanel(Tab kind);
    void                      openPanel(Tab kind);   // a tab picked: opened, never closed

    // ---- AppHeads.cpp
    void showButtons();   // the open tab's head's tools
    void addButton(std::vector<Chip>&    buttons,
                   std::string           text,
                   std::string_view      key,
                   bool                  on,
                   std::function<void()> action);
    void showLogHead();   // the log's head: what it shows, its tools

    // ---- AppPages.cpp, AppMetrics.cpp
    void               showPage();            // a page of boxes
    [[nodiscard]] Page printerPage() const;   // settings, debugger, health, inspect
    [[nodiscard]] PageForm<std::function<void()>> metricsForm();
    [[nodiscard]] std::string metricsWhat() const;   // the head's: "100 in 10 groups"
    bool                      metricsKey(::rtxui::Event::Keyboard const& k);
    void                      moveMetric(long step);   // the mark, `step` metrics on in the table
    void                      scrollMetrics(long rows);   // the table's rows, the mark where it is

    // ---- AppFilter.cpp
    [[nodiscard]] FilterSources            filterSources() const;
    [[nodiscard]] EntryStore::UcTimeWindow timeWindow() const;
    void                                   applyFilter(FilterState      next,
                                                       std::string_view said);
    void                                   filterAction(FilterItem const& item);
    void                                   filterByLine(Command command);

    // ---- AppDialog.cpp
    void openDialog(Modal modal);   // the palette or a combo box
    void fillDialog(bool again);    // the rows for the field's text (again: anew), those that fit
    void refreshRows(bool newText);
    [[nodiscard]] DialogRow        dialogRow(std::size_t i) const;
    [[nodiscard]] std::string      dialogCount() const;    // how many rows, which are shown
    [[nodiscard]] std::string_view questionHint() const;   // the keys a question's list takes
    [[nodiscard]] bool
    leavesForField() const;   // up from the first row is the field (a value of its own)
    [[nodiscard]] std::size_t dialogSize() const;   // its rows
    [[nodiscard]] std::size_t dialogRows() const;   // as many as fit
    void                      closeDialog();
    void pickRow(std::size_t index);   // a click or Enter: run, switch, answer or go into
    void
    acceptDialog(bool field);   // Enter (false), ✓ (true: a directory marked, the field's text)
    bool dialogEvent(::rtxui::Event const& event);

    // ---- AppQuestions.cpp
    void                              ask(Field       field,
                                          std::size_t index = 0);
    [[nodiscard]] std::string         valueOf(Field       field,
                                              std::size_t index) const;   // the answer now
    [[nodiscard]] std::vector<Choice> choicesFor(std::string const& text);
    bool answer(std::string const& text);   // false: what is wrong is in the dialog, it stays

    // ---- AppCommands.cpp, AppKeys.cpp, AppMouse.cpp
    void copyLine();
    void setRing(bool on);
    bool key(::rtxui::Event::Keyboard const& k);
    bool mouse(::rtxui::Event::Mouse const& m);
    void openMenu(int x,
                  int y);
};

}   // namespace uc_log::printer::rtx
