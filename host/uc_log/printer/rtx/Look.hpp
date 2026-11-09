#pragma once

// The RTXUI window's one look, as style sheet parts the components join into their templates (Theme.hpp's viewOf):
// the classes of a log or panel run, the frame with its title in the border, the head every tab has, the tools in
// it. Colours by name (Theme.hpp).

namespace uc_log::printer::rtx::Detail {

// the classes of a run (Runs.hpp's runClass): its kind, level, channel or tone, the marked row, a search match
inline constexpr char runStyles[] = R"css(
  .sel { background-color: var(--marked); }
  .hit { font-weight: bold; text-decoration: underline; }
  .hostTime { color: var(--host-time); }
  .targetTime { color: var(--target-time); }
  .sep { color: var(--faint); }
  .ch0 { color: var(--ink); background-color: var(--ch0); }
  .ch1 { color: var(--ch1); }
  .ch2 { color: var(--ch2); }
  .ch3 { color: var(--ch3); }
  .ch4 { color: var(--ch4); }
  .ch5 { color: var(--ch5); }
  .lv0 { color: var(--lv-trace); }
  .lv1 { color: var(--lv-debug); }
  .lv2 { color: var(--lv-info); }
  .lv3 { color: var(--lv-warn); }
  .lv4 { color: var(--lv-error); font-weight: bold; }
  .lv5 { color: var(--bright); background-color: var(--lv-crit); font-weight: bold; }
  .unparsed { color: var(--bad); font-weight: bold; }
  .cut { color: var(--faint); }
  .module { color: var(--module); }
  .function { color: var(--function); }
  .location { color: var(--dim); }
  .label { color: var(--dim); }
  .t0 { color: var(--text); }
  .t1 { color: var(--ok); }
  .t2 { color: var(--accent); }
  .t3 { color: var(--warn); }
  .t4 { color: var(--bad); }
)css";

// a rounded frame with its title in the top border (as RTXUI's fieldset draws it), a scrollbar in the column left
// of the border. No background of its own: the window's is painted once (App, `.app`) - an opaque fill resets
// every cell it covers, and nested ones cost a frame millions of instructions
inline constexpr char frameStyles[] = R"css(
  .box { position: relative; height: 100%; }
  .frame { display: flex; height: 100%; border: round; border-color: var(--border); }
  .legend { position: absolute; top: 0; left: 2; z-index: 1; padding: 0 1; white-space: pre; font-weight: bold;
            background-color: var(--bg); color: var(--accent); user-select: none; }
  .scrollbar { width: 1; flex-shrink: 0; white-space: pre; overflow: hidden; color: var(--faint); }
)css";

// the head every tab has: its title and what it shows, a hint, its tools on the right. A tool that is on is
// filled as the open tab is; the focus is painted only while Tab moves it (`ring`)
inline constexpr char headStyles[] = R"css(
  .page { display: flex; flex-direction: column; overflow: hidden; }
  .head { display: flex; height: 1; flex-shrink: 0; padding: 0 1; white-space: pre; user-select: none;
          background-color: var(--surface); }
  .title { flex-shrink: 1; min-width: 0; overflow: hidden; color: var(--accent); font-weight: bold; }
  .hint { flex-shrink: 100; min-width: 0; overflow: hidden; color: var(--dim); }
  .fill { flex-grow: 1; }
  .tool { flex-shrink: 0; white-space: pre; margin-left: 1; background-color: var(--action); color: var(--bright);
          opacity: 1; }
  .tool:hover { background-color: var(--hover); color: var(--ink); opacity: 1; }
  .tool.on { background-color: var(--accent); color: var(--ink); font-weight: bold; }
  .tool .key { color: var(--key); }
  .tool.on .key, .tool:hover .key { color: var(--ink); }
  .ring .tool:focus-visible { background-color: var(--focus); color: var(--bright); font-weight: bold; opacity: 1; }
)css";

// a tool that starts or destroys, and the gap before a group of tools
inline constexpr char toolLooks[] = R"css(
  .tool.go { background-color: var(--go); }
  .tool.danger { background-color: var(--danger); }
  .tool.go:hover, .tool.danger:hover { background-color: var(--hover); }
  .tool.gap { margin-left: 3; }
)css";

// a page of one framed box under its head (the log, a text panel): the box takes the rest; runs are plain
// elements shown inline (Runs.hpp)
inline constexpr char panelTail[] = R"css(
  .box { height: auto; flex-grow: 1; min-height: 0; }
  td { display: inline; }
)css";

// the last part of a template whose own text left its style open
inline constexpr char styleEnd[] = "</style>\n";

}   // namespace uc_log::printer::rtx::Detail
