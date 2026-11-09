// The RTXUI window's dialog, drawn headless (support/RtxWindow.hpp): the search's question and the answers before,
// the command palette, the filter's combo boxes and their rows that stay put while the counts change, a click beside
// the dialog that closes it, a directory question that goes into a directory.
#include "support/Check.hpp"
#include "support/RtxWindow.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fmt/format.h>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace uc_log::test::rtx_window;

namespace {

void searchDialog() {
    Window w{10};
    w.resize(100, 30);
    w.screen->input("/");
    CHECK(w.app->asking() == Field::search && contains(w.text(), "🔎 Search the log")
            && contains(w.text(), "only the lines that hold it"),
          "/ opens the search's dialog: what it asks, what it does");
    w.screen->input("line 3");
    w.screen->input("\r");
    CHECK(w.searched == "line 3", "Enter hands the text over");
    CHECK(!w.app->dialog->open && !contains(w.text(), "Search the log"), "and closes the dialog");
    w.screen->input("/");
    w.screen->input("zzz");
    w.screen->input("\x1b");
    w.screen->input("");
    CHECK(w.searched == "line 3" && !w.app->dialog->open, "Escape hands nothing over");
    w.screen->input("/");
    CHECK(contains(w.text(), "line 3  before"), "the search before is a choice");
    w.searched.clear();
    CHECK(clickText(w, "line 3  before") && w.searched == "line 3" && !w.app->dialog->open,
          "a click on it searches it again");
}

void palette() {
    Window w{100};
    w.screen->input(":");
    CHECK(contains(w.text(), "⌨ Commands") && contains(w.text(), "go to the oldest line"),
          ": opens the palette");
    w.screen->input("flash the");
    CHECK(contains(w.text(), "flash the target") && !contains(w.text(), "go to the oldest line"),
          "typing narrows it");
    w.screen->input("\r");
    CHECK(w.flashes == 1, "Enter runs the marked command");
    CHECK(!contains(w.text(), "go to the oldest line") && !contains(w.text(), "flash the target"),
          "and closes the palette");
    w.screen->input(":");
    w.screen->input("\x1b");
    w.screen->input("");
    CHECK(!contains(w.text(), "go to the oldest line") && w.flashes == 1,
          "Escape closes it, nothing run");
    w.screen->input(":");
    CHECK(contains(w.text(), "Cancel") && contains(w.text(), "▶ Run"),
          "its buttons, as every dialog's");
    CHECK(clickText(w, "Cancel") && !w.app->dialog->open && w.flashes == 1,
          "Cancel closes it, nothing run");
    w.screen->input(":");
    w.screen->input("flash the");
    CHECK(clickText(w, "▶ Run") && !w.app->dialog->open && w.flashes == 2,
          "▶ Run runs the marked command");
}

void filterComboBox() {
    Window w{10};
    w.resize(140, 40);
    for(auto const* name : {"i2c.bus", "i2c.scanner", "usb"}) {
        auto e   = entry(std::string{"from "} + name);
        e.module = name;
        w.store.addEntry(std::chrono::system_clock::time_point{}, e);
    }
    w.frame();
    w.screen->input("3");
    CHECK(contains(w.text(), "Choose … (4 modules)") && contains(w.text(), "Choose … (1 location)"),
          "the filter page: a chip for each combo box (i2c, i2c.bus, i2c.scanner, usb)");
    CHECK(clickText(w, "Choose … (4 modules)") && w.app->dialog->open
            && contains(w.text(), "  i2c.scanner"),
          "a click opens the modules' combo box: the tree");
    w.screen->input("bus");
    CHECK(contains(w.text(), "1 of 4 match") && !contains(w.text(), "i2c.scanner"),
          "typing narrows it");
    w.screen->input("\r");
    CHECK(w.lastFilter.excludedModules == std::set<std::string>{"i2c.bus"}
            && contains(w.text(), "✘      i2c.bus"),
          "Enter switches the marked one: hidden, and the box says so");
    w.screen->input("\x1b");
    CHECK(!w.app->dialog->open && contains(w.text(), "i2c.bus ✕"),
          "Esc closes it; the page has a chip for it");
    CHECK(clickText(w, "i2c.bus ✕") && w.lastFilter.excludedModules.empty(),
          "a click on the chip shows it again");
    CHECK(clickText(w, "Choose … (1 location)") && contains(w.text(), "main.cpp:7"),
          "the locations' combo box");
    CHECK(clickText(w, "main.cpp:7") && w.lastFilter.excludedLocations.size() == 1,
          "a click on a row switches it (shown -> hidden)");
}

void comboBoxRowsStayPut() {
    Window w{10};   // ten lines at main.cpp:7
    w.resize(140, 40);
    auto const elsewhere = [&](int n) {
        for(int i = 0; i < n; ++i) {
            auto e     = entry("elsewhere");
            e.fileName = "other.cpp";
            e.line     = 1;
            w.store.addEntry(std::chrono::system_clock::time_point{}, e);
        }
        w.frame();
        w.screen->input("");
    };
    elsewhere(5);
    w.screen->input("3");
    CHECK(clickText(w, "Choose … (2 locations)"), "the locations' combo box");
    auto rows = rowsOf(w.text());
    CHECK(rowOf(rows, "main.cpp:7  ×") < rowOf(rows, "other.cpp:1  ×"),
          "the busiest first: main.cpp:7");
    elsewhere(20);   // other.cpp:1 the busiest now, while the box is open
    std::set<SourceLocation> const other{
      SourceLocation{"other.cpp", 1}
    };
    CHECK(clickText(w, "other.cpp:1  ×") && w.lastFilter.excludedLocations == other,
          "a click switches the row clicked, though the counts changed under it");
    rows = rowsOf(w.text());
    CHECK(rowOf(rows, "main.cpp:7  ×") < rowOf(rows, "other.cpp:1  ×"),
          "and the rows stay where they were");
    w.screen->input("\r");   // Enter: the marked row - the one clicked - goes round
    CHECK(w.lastFilter.includedLocations == other && w.lastFilter.excludedLocations.empty(),
          "the cursor stays on it: Enter, and it is the only one");
}

void dialogClosesBeside() {
    Window w{100};
    w.resize(100, 30);
    w.frame(2);
    w.screen->input(":");
    CHECK(w.app->dialog->open, "the palette");
    auto const badge = cellOf(rowsOf(w.text())[0], "⚠ 2");
    w.screen->click(static_cast<int>(badge), 0);
    CHECK(!w.app->dialog->open && w.app->panel() == Tab::logs,
          "a click beside it closes it - and does nothing else (the errors badge under it)");
    w.screen->input("/");
    auto rows  = rowsOf(w.text());
    auto title = rowOf(rows, "🔎 Search the log");
    CHECK(title.has_value(), "the search's dialog");
    if(!title) { return; }
    w.screen->click(static_cast<int>(cellOf(rows[*title], "Search the log")),
                    static_cast<int>(*title));
    CHECK(w.app->dialog->open, "a click on its title keeps it");
    CHECK(clickText(w, " ✕ ") && !w.app->dialog->open, "its ✕ closes it");
    w.screen->input("/");
    w.screen->input(sgrMouse(2, 2, 28));   // a right click beside it
    CHECK(!w.app->dialog->open && !w.app->menu->open,
          "a right click beside it closes it too, no menu");
    w.screen->input("3");
    CHECK(clickText(w, "Choose … (1 location)") && w.app->dialog->open, "the locations' box");
    w.screen->click(1, 28);
    CHECK(!w.app->dialog->open && w.app->panel() == Tab::filter,
          "closed by a click beside it, the filter page still open");
    CHECK(clickText(w, "to …") && w.app->asking() == Field::timeTo
            && contains(w.text(), "the newest line"),
          "to …: a target time, the log's times to choose");
    CHECK(clickText(w, "Cancel") && !w.app->dialog->open && !w.lastTime, "Cancel: nothing set");
}

void directoryDialog() {
    namespace fs    = std::filesystem;
    auto const root = fs::temp_directory_path() / fmt::format("uc_log_rtx_dialog_{}", ::getpid());
    fs::remove_all(root);
    fs::create_directories(root / "logs" / "sub" / "deeper");
    Window w{10};
    w.resize(140, 40);
    w.app->printer.model.logFilePath = (root / "logs" / "a.rttlog").string();
    w.screen->input("4");
    CHECK(clickText(w, "📁 Directory …") && contains(w.text(), "📁 The log file's directory")
            && contains(w.text(), "📁 sub/"),
          "the log's directory: a dialog, the directories in it");
    CHECK(clickText(w, "📁 sub/") && w.app->dialog->open
            && w.app->dialog->query == (root / "logs" / "sub").string() + "/"
            && contains(w.text(), "📁 deeper/"),
          "a click on a directory goes into it: the field, its directories");
    CHECK(clickText(w, "✓ Use") && !w.app->dialog->open && lastAct<action::LogDir>(w) != nullptr
            && lastAct<action::LogDir>(w)->dir == (root / "logs" / "sub").string() + "/",
          "✓ Use: that directory");
    std::error_code ec;
    fs::remove_all(root, ec);
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {       "searchDialog",        searchDialog},
                               {            "palette",             palette},
                               {     "filterComboBox",      filterComboBox},
                               {"comboBoxRowsStayPut", comboBoxRowsStayPut},
                               { "dialogClosesBeside",  dialogClosesBeside},
                               {    "directoryDialog",     directoryDialog}
    });
}
