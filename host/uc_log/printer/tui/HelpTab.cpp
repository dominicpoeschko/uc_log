// the Help tab.
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getHelpComponent() -> ftxui::Component {
        return scrollView([]() {
            return ftxui::vbox(
              {ftxui::text("❓ Help - Keyboard Shortcuts") | ftxui::bold
                 | ftxui::color(Theme::Header::primary()) | ftxui::center,
               ftxui::separator(),
               ftxui::text(""),
               printer::tui::heading("📑 Tab Navigation"),
               ftxui::text("  1       - Logs tab"),
               ftxui::text("  2       - Build tab"),
               ftxui::text("  3       - Filter tab"),
               ftxui::text("  4       - Settings tab (Display, File Logging, Network, Duplex)"),
               ftxui::text("  5       - Debugger tab"),
               ftxui::text("  6       - Metrics tab"),
               ftxui::text("  7       - Status tab"),
               ftxui::text("  8       - Health tab (sanitizer, halt, fault and panic records -"),
               ftxui::text("            and clearing them, stacks, raise a panic; statistics)"),
               ftxui::text("  9       - Inspect tab (trace rings, watches, profiler)"),
               ftxui::text("  0 / ?   - Help tab"),
               ftxui::text(""),
               printer::tui::heading("🔧 Actions"),
               ftxui::text("  q       - Quit application"),
               ftxui::text("  r       - Reset target"),
               ftxui::text("  f       - Flash target"),
               ftxui::text("  b       - Start build"),
               ftxui::text("  Shift+F - Build and flash"),
               ftxui::text("  /       - Show and focus the log search field"),
               ftxui::text("            (Esc hides it again when it is empty)"),
               ftxui::text(""),
               printer::tui::heading("📜 Log View"),
               ftxui::text("  ↓/↑, j / k or wheel    - Scroll down / up"),
               ftxui::text("  h / l or ←/→           - Scroll horizontally"),
               ftxui::text("  PageUp / PageDown      - Scroll a page"),
               ftxui::text("  Home / End             - Jump to first / last entry"),
               ftxui::text("                           (End re-enables follow mode)"),
               ftxui::text("  y                      - Copy selected line (OSC 52)"),
               ftxui::text("  Search: plain text is case-insensitive, prefix with re:"),
               ftxui::text("          for a regular expression; matches filter the view"),
               ftxui::text(""),
               printer::tui::heading("📝 in the status line"),
               ftxui::text("  ✘ N - N log lines were lost since the last flash because the"),
               ftxui::text("        host could not apply their format string; the Status tab"),
               ftxui::text("        says which field and why"),
               ftxui::text("  UB N - sanitizer reports since boot (Health tab: where)"),
               ftxui::text("  🚨 PANIC / 📕 FAULT - a record in RAM the firmware has not"),
               ftxui::text("        reported yet (Health tab; \"Clear records\" there empties"),
               ftxui::text("        them); ⏸ HALTED - the core is halted: r resets it (Go"),
               ftxui::text("        does not leave a panic's or fault's breakpoint loop)"),
               ftxui::text(""),
               ftxui::text("💡 Tips") | ftxui::bold | ftxui::color(Theme::Header::warning()),
               ftxui::text("  • Use Tab/Shift+Tab to navigate between UI elements"),
               ftxui::text("  • Use arrow keys to navigate lists and menus"),
               ftxui::text("  • Number keys work globally except when typing in text fields"),
               ftxui::text("")});
        });
    }

}}   // namespace uc_log::printer::tui
