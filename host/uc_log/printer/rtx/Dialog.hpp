#pragma once

// The window's dialog (one at a time): the command palette, a combo box, a question - its field, its rows, its
// buttons, over a backdrop a click on which closes it. The App fills it and takes its keys (App, AppDialog.cpp).

#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

/// A row of the dialog's list: a command, a module or location of the filter, a choice of a question.
struct DialogRow {
    std::string cls;    // "marked" on the cursor; "off", "on", "into" (a directory)
    std::string mark;   // the left column, padded: a command's keys, a module's ✔ ✘, a tick
    std::string text;
    std::string detail;   // dimmed after it, its gap in front

    bool operator==(DialogRow const&) const = default;
};

void Fields(DialogRow const&    row,
            ::rtxui::FieldSink& out);

namespace Detail {
    inline constexpr char dialogHtml[] = R"html(
<if condition="{open}">
  <div class="backdrop" onclick="Close" oncontextmenu="Close"></div>
  <div class="layer"><div class="box" style="{size}">
    <div class="head"><span class="title">{title}</span><span class="close" onclick="Close"> ✕ </span></div>
    <if condition="{hasAbout}"><div class="about">{about}</div></if>
    <input class="query" value="{query}" placeholder="{placeholder}" autofocus="" select_on_focus="true"/>
    <if condition="{hasError}"><div class="error">{error}</div></if>
    <if condition="{hasCount}"><div class="count">{count}</div></if>
    <div class="list"><for each="{rows}" as="r"><div class="row {r.cls}" onclick="Pick({$index})"><span class="mark">{r.mark}</span><span class="text">{r.text}</span><span class="detail">{r.detail}</span></div></for></div>
    <div class="foot"><span class="hint">{hint}</span><for each="{buttons}" as="b"><span class="button {b.cls}" onclick="Press({$index})">{b.text}</span></for></div>
  </div></div>
</if>
<style>
  self { position: absolute; top: 0; left: 0; }
  .backdrop { position: fixed; top: 0; left: 0; width: 100%; height: 100%; z-index: 100;
              background-color: var(--backdrop); }
  .layer { position: fixed; top: 0; left: 0; width: 100%; height: 100%; z-index: 101; display: flex;
           justify-content: center; align-items: center; pointer-events: none; }
  .box { pointer-events: auto; display: flex; flex-direction: column; border: round; padding: 0 1;
         border-color: var(--accent); background-color: var(--surface); color: var(--text); }
  .head { display: flex; height: 1; flex-shrink: 0; white-space: pre; user-select: none; }
  .title { flex-grow: 1; overflow: hidden; font-weight: bold; color: var(--accent); }
  .close { flex-shrink: 0; cursor: pointer; color: var(--muted); }
  .close:hover { background-color: var(--hover); color: var(--ink); }
  .about { flex-shrink: 0; color: var(--muted); }
  .query { flex-shrink: 0; width: 100%; margin-top: 1; background-color: var(--field); color: var(--bright); }
  .error { flex-shrink: 0; color: var(--bad); }
  .count { flex-shrink: 0; margin-top: 1; color: var(--dim); }
  .list { flex-shrink: 1; min-height: 0; overflow: hidden; }
  .row { display: flex; height: 1; white-space: pre; overflow: hidden; user-select: none; cursor: pointer; }
  .row:hover { background-color: var(--hover-row); }
  .row.marked { background-color: var(--marked); }
  .row.off { color: var(--warn); }
  .row.on { color: var(--ok-text); }
  .mark { flex-shrink: 0; color: var(--warn); }
  .text { flex-shrink: 1; overflow: hidden; }
  .detail { flex-shrink: 0; color: var(--dim); }
  .foot { display: flex; height: 1; flex-shrink: 0; margin-top: 1; white-space: pre; user-select: none; }
  .hint { flex-grow: 1; overflow: hidden; color: var(--dim); }
  .button { flex-shrink: 0; margin-left: 1; padding: 0 1; cursor: pointer; background-color: var(--action);
            color: var(--bright); }
  .button.go { background-color: var(--go); }
  .button:hover { background-color: var(--hover); color: var(--ink); }
</style>
)html";
}   // namespace Detail

/// The window's dialog, one at a time: its title and what it is about, a field, a line for what is wrong with the
/// field, the rows the field matches (a click hands rows[i] to `picked`), a hint and buttons. Over a backdrop that
/// dims the window: a click (either button) on it, or on the ✕, hands `closed` the word. The App fills it and takes
/// its keys; the field is two-way bound (it changes without telling anybody: the App compares).
class Dialog : public ::rtxui::Component<Dialog> {
public:
    bool                             open{false};
    std::string                      title;
    std::string                      about;
    std::string                      query;   // two-way bound to the field
    std::string                      placeholder;
    std::string                      error;
    std::string                      count;   // how many rows match, which are shown
    std::vector<DialogRow>           rows;
    std::string                      hint;
    std::vector<Chip>                buttons;   // cls: "go" (the answer), "" (cancel, done)
    std::string                      size{"width: 64;"};
    std::function<void(std::size_t)> picked;    // a click on rows[i]
    std::function<void(std::size_t)> pressed;   // a click on buttons[i]
    std::function<void()>            closed;    // a click beside the box, or its ✕

    Dialog();

    /// The lines that only show when they say something.
    bool Digest() override;

    std::string_view view{Detail::viewOf<Detail::dialogHtml>};

private:
    bool hasAbout{false};
    bool hasError{false};
    bool hasCount{false};
};

}   // namespace uc_log::printer::rtx
