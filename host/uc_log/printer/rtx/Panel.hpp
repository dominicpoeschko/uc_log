#pragma once

// A text panel: its head (title, hint, tools) and its rows as styled runs in a frame with a scrollbar; only the rows
// it shows are built (printer/PanelText.hpp).

#include "uc_log/printer/rtx/Look.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

namespace Detail {
    // a text panel (build output, messages, the line, help) as a page with one box: the head, its rows framed
    inline constexpr char panelHtml[] = R"html(
<div class="page {ring}" style="{height}">
  <div class="head"><div class="title">{title}</div><div class="hint">{hint}</div><div class="fill"></div><for each="{buttons}" as="b"><button class="tool {b.cls}" onclick="Press({$index})">{b.text}<span class="key">{b.key}</span></button></for></div>
  <div class="box">
    <div class="frame"><div class="body"><for each="{spans}" as="s"><td class="{s.cls}" style="{s.style}">{s.text}</td></for></div><div class="scrollbar">{scrollbar}</div></div>
    <div class="legend">{legend}</div>
  </div>
</div>
<style>
  self { flex-shrink: 0; }
  .body { flex-grow: 1; height: 100%; padding-left: 1; white-space: pre; overflow: hidden; color: var(--text); }
)html";
}   // namespace Detail

/// A text panel (build output, messages, the marked line, help): its head and its rows in one framed box.
class Panel : public ::rtxui::Component<Panel> {
public:
    std::string       title;    // the head: the tab and what it shows
    std::string       hint;     // dimmed after it: the keys it has
    std::string       legend;   // in its box's frame: what the rows are
    std::string       ring;     // "ring": Tab moves the focus, it is painted
    std::string       height{"height: 6;"};
    std::vector<Span> spans;
    std::string       scrollbar;                // the last column: where the shown rows are in all
    std::vector<Chip> buttons;                  // in the title row, on its right
    std::function<void(std::size_t)> pressed;   // a click on buttons[i]

    Panel();

    std::string_view view{Detail::viewOf<Detail::panelHtml,
                                         Detail::headStyles,
                                         Detail::toolLooks,
                                         Detail::frameStyles,
                                         Detail::panelTail,
                                         Detail::runStyles,
                                         Detail::styleEnd>};
};

}   // namespace uc_log::printer::rtx
