#pragma once

// A page of boxes (filter, settings, debugger, metrics, health, inspect): its head (title, hint, tools), then framed
// boxes of buttons and text side by side where they fit, in a box that scrolls. A neutral page (printer/PageForm.hpp) fills
// it; a click on one of its buttons runs that button's action here.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/PageForm.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/rtx/Look.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

/// One piece of a form box: a button, a text, or the end of a row.
struct Control {
    std::string cls;
    std::string text;
    std::string button;   // "1": a button
    std::string brk;      // "1": the row ends here

    bool operator==(Control const&) const = default;
};

void Fields(Control const&      control,
            ::rtxui::FieldSink& out);

namespace Detail {
    // the look of a page: framed boxes, buttons that look like buttons (filled, a pointer, hover and focus colours),
    // switches and choices as ✔ ✘ ◉ ○ (on: filled as the open tab, off: dim), chips for what is hidden
    inline constexpr char formHtml[] = R"html(
<div class="box {ring}">
  <div class="frame"><for each="{items}" as="c"><if condition="{c.button}"><button class="{c.cls}" onclick="Hit({$index})">{c.text}</button></if><elif condition="{c.brk}"><div class="brk"></div></elif><else><span class="{c.cls}">{c.text}</span></else></for></div>
  <div class="legend">{legend}</div>
  <if condition="{hasBar}"><div class="bar" style="{barStyle}"><for each="{bar}" as="c"><div class="cell" onclick="Bar({$index})">{c.text}</div></for></div></if>
</div>
<style>
  .box { position: relative; }
  .frame { display: flex; flex-wrap: wrap; align-items: center; border: round; border-color: var(--border);
           padding: 0 1; white-space: pre; overflow: hidden; color: var(--text); }
  .legend { position: absolute; top: 0; left: 2; z-index: 1; padding: 0 1; white-space: pre; font-weight: bold;
            background-color: var(--bg); color: var(--accent); user-select: none; }
  .brk { width: 100%; height: 0; }
  .bar { position: absolute; right: 1; z-index: 1; width: 1; white-space: pre; color: var(--faint);
         user-select: none; }
  .bar .cell { height: 1; cursor: pointer; }
  button { margin-right: 1; background-color: var(--action); color: var(--bright); opacity: 1; }
  button:hover { background-color: var(--hover); color: var(--ink); opacity: 1; }
  button.toggle { background-color: var(--raised); color: var(--dim); }
  button.toggle.on { background-color: var(--accent); color: var(--ink); font-weight: bold; }
  button.toggle:hover { background-color: var(--hover); color: var(--ink); }
  button.chip { background-color: var(--warn-fill); color: var(--warn-text); }
  button.chip.t1 { background-color: var(--ok-fill); color: var(--ok-text); }
  button.chip:hover { background-color: var(--hover); color: var(--ink); }
  button.danger { background-color: var(--danger); color: var(--bright); }
  button.go { background-color: var(--go); color: var(--bright); }
  button.danger:hover, button.go:hover { background-color: var(--hover); color: var(--ink); }
  button.row { width: 100%; margin-right: 0; padding: 0; background-color: var(--bg);
               color: var(--text); }
  button.row.on { background-color: var(--marked); color: var(--bright); }
  button.row:hover { background-color: var(--hover-row); color: var(--bright); }
  button.row.t2 { color: var(--accent); font-weight: bold; }
  .sel { background-color: var(--marked); }
  .ring button:focus-visible { background-color: var(--focus); color: var(--bright); font-weight: bold; opacity: 1; }
)html";

}   // namespace Detail

/// A box of a page: its title in the frame's top border, its rows of buttons and text, and - its rows a part of more
/// (PageScroll) - a scrollbar in the frame's last column beside them.
class FormBox : public ::rtxui::Component<FormBox> {
public:
    std::string                      legend;
    std::string                      ring;   // "ring": Tab moves the focus, it is painted
    std::vector<Control>             items;
    std::function<void(std::size_t)> hit;   // a click on items[i] (a button)
    bool                             hasBar{};
    std::string          barStyle;   // its top: under the frame's border and the rows that stay
    std::vector<Control> bar;        // a cell a scrolled row: │ the track, █ the thumb
    std::function<void(std::size_t)> barHit;   // a click on bar[i]

    FormBox();

    std::string_view view{Detail::viewOf<Detail::formHtml, Detail::runStyles, Detail::styleEnd>};
};

namespace Detail {
    inline constexpr char formPageHtml[] = R"html(
<div class="page {ring}" style="{height}">
  <div class="head"><div class="title">{title}</div><div class="hint">{hint}</div><div class="fill"></div><for each="{tools}" as="b"><button class="tool {b.cls}" onclick="Press({$index})">{b.text}<span class="key">{b.key}</span></button></for></div>
  <div class="scroll"><div class="flow">
    <if condition="{use0}"><div class="slot {cls0}"><box-0/></div></if>
    <if condition="{use1}"><div class="slot {cls1}"><box-1/></div></if>
    <if condition="{use2}"><div class="slot {cls2}"><box-2/></div></if>
    <if condition="{use3}"><div class="slot {cls3}"><box-3/></div></if>
    <if condition="{use4}"><div class="slot {cls4}"><box-4/></div></if>
    <if condition="{use5}"><div class="slot {cls5}"><box-5/></div></if>
    <if condition="{use6}"><div class="slot {cls6}"><box-6/></div></if>
    <if condition="{use7}"><div class="slot {cls7}"><box-7/></div></if>
  </div></div>
</div>
<style>
  self { flex-shrink: 0; }
  .scroll { flex-grow: 1; overflow-y: scroll; }
  .flow { display: flex; flex-wrap: wrap; align-items: flex-start; }
  .slot { flex-grow: 1; flex-basis: 36; min-width: 0; }
  .slot.wide { flex-basis: 100%; }
)html";

}   // namespace Detail

/// A page (filter, settings, debugger, metrics, health, inspect): its title and tools, then its boxes side by side
/// where they fit, in a box that scrolls.
class FormPage : public ::rtxui::Component<FormPage> {
public:
    static constexpr std::size_t maxBoxes = 8;

    std::string       title;   // the tab and what it shows
    std::string       hint;    // dimmed after it: the keys it has
    std::string       ring;    // "ring": Tab moves the focus, it is painted
    std::string       height{"height: 10;"};
    std::vector<Chip> tools;   // its head's: the window puts the page's own (formTools) among them
    std::function<void(std::size_t)> pressed;   // a click on tools[i]
    std::function<void()> changed;   // a box's button ran its action: the window lays out anew
    std::function<void(std::size_t, std::size_t)>
      scrolledTo;   // a click on box i's scrollbar: its rows from the one given on
    std::array<::rtxui::Ref<FormBox>, maxBoxes> boxes;
    // which boxes are there, and "wide" for one that takes the whole width (a template binds members, not arrays)
    bool        use0{}, use1{}, use2{}, use3{}, use4{}, use5{}, use6{}, use7{};
    std::string cls0, cls1, cls2, cls3, cls4, cls5, cls6, cls7;

    FormPage();

    /// `form` on the page: its boxes (more than maxBoxes: the rest in the last one, each under its title), a click on
    /// a button runs `run` with the button's action; the form's own tools are formTools, for the head.
    template<typename Action,
             typename Run>
    void fill(PageForm<Action> form,
              Run const&       run) {
        std::vector<std::function<void()>> actions;
        actions.reserve(form.actions.size());
        for(auto& a : form.actions) {
            actions.emplace_back([run, action = std::move(a)] { run(action); });
        }
        fill(form.tools, std::move(form.boxes), std::move(actions));
    }

    void fill(std::vector<PanelPart> const&      ownTools,
              std::vector<PageBox>               given,
              std::vector<std::function<void()>> actions);

    /// The form's own tools (the last fill's), and a click on one.
    [[nodiscard]] std::vector<Chip> const& formTools() const UC_LOG_LIFETIMEBOUND {
        return formTools_;
    }

    void pressFormTool(std::size_t i);

    std::string_view view{
      Detail::
        viewOf<Detail::formPageHtml, Detail::headStyles, Detail::toolLooks, Detail::styleEnd>};

private:
    std::vector<std::function<void()>> actions_;   // the last fill's
    std::array<std::vector<std::uint16_t>, maxBoxes>
      ids_;   // each box's items' actions (1-based; 0: no button)
    std::array<std::optional<PageScroll>, maxBoxes> scrolls_;   // each box's scrolled rows, if any
    std::vector<Chip>                               formTools_;
    std::vector<std::uint16_t>                      formToolIds_;

    void run(std::uint16_t id);
    void showBar(std::size_t    b,
                 PageBox const& box);
    void showBox(std::size_t i,
                 bool        used,
                 std::string cls);
};

}   // namespace uc_log::printer::rtx
