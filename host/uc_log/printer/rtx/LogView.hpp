#pragma once

// The log: its head (what it shows, its tools), its lines in a frame with a scrollbar. The lines are formatted only
// when what they show changed (printer/LogScreen.hpp; the store itself is never bound) and are their own component
// (LogLines), so a frame of new lines renders them alone.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/LogViewport.hpp"
#include "uc_log/printer/rtx/Look.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

namespace Detail {
    // the log as every tab: its head (what it shows, its tools), then its lines framed
    inline constexpr char logViewHtml[] = R"html(
<div class="page {ring}">
  <div class="head"><div class="title">{title}</div><div class="hint">{hint}</div><div class="fill"></div><for each="{tools}" as="b"><button class="tool {b.cls}" onclick="Press({$index})">{b.text}<span class="key">{b.key}</span></button></for></div>
  <div class="box">
    <div class="frame">
      <log-lines/>
      <div class="scrollbar">{scrollbar}</div>
    </div>
    <div class="legend">{legend}</div>
  </div>
</div>
<style>
  self { flex-grow: 1; flex-basis: 0; overflow: hidden; }
  .page { height: 100%; }
)html";

    // the log's lines: the rows' runs in one flow, each a plain element (LogLines)
    inline constexpr char logLinesHtml[] = R"html(
<div class="lines"><for each="{spans}" as="s"><td class="{s.cls}" style="{s.style}">{s.text}</td></for></div>
<style>
  self { flex-grow: 1; height: 100%; }
  .lines { height: 100%; overflow: hidden; white-space: pre; color: var(--text); }
  td { display: inline; }
)html";
}   // namespace Detail

/// The log's lines, their own component (a frame's new lines render this alone, not the log's head and tools): the
/// rows in one flow, each run a plain element - a td, one of RTXUI's layout tags, shown inline. A span is a
/// component, and RTXUI renders, styles and cross-checks every component that re-rendered: ~400 of them every frame
/// of a flood, the cross-check quadratic (half a frame's instructions went there).
class LogLines : public ::rtxui::Component<LogLines> {
public:
    std::vector<Span> spans;   // the rows, left to right

    LogLines();

    std::string_view view{
      Detail::viewOf<Detail::logLinesHtml, Detail::runStyles, Detail::styleEnd>};
};

class LogView : public ::rtxui::Component<LogView> {
public:
    ::rtxui::Ref<LogLines>           lines;
    std::string                      scrollbar;
    std::string                      legend{"📜 The lines"};   // in the frame's top border
    std::string                      title{"📄 Logs"};         // the head, as every tab has
    std::string                      hint;
    std::string                      ring;   // "ring": Tab moves the focus, it is painted
    std::vector<Chip>                tools;
    std::function<void(std::size_t)> pressed;   // a click on tools[i]

    LogView();

    /// The rows of the view for `rows` x `width` cells over the mirror (the last column the scrollbar): formatted
    /// again only when the frame changed.
    void show(EntryStore::Mirror const& mirror,
              std::size_t               rows,
              std::size_t               width);

    LogViewport& viewport() UC_LOG_LIFETIMEBOUND { return viewport_; }

    LogColumns& columns() UC_LOG_LIFETIMEBOUND { return columns_; }

    std::size_t& hOffset() UC_LOG_LIFETIMEBOUND { return hOffset_; }

    [[nodiscard]] std::size_t rows() const { return rows_; }

    [[nodiscard]] std::size_t formattedRows() const { return screen_.formattedRows(); }

    /// The line on the marked row (or the newest one while following), for the copy and the details.
    [[nodiscard]] std::optional<std::size_t> current(EntryStore::Mirror const& mirror) const;

    std::string_view view{Detail::viewOf<Detail::logViewHtml,
                                         Detail::headStyles,
                                         Detail::toolLooks,
                                         Detail::frameStyles,
                                         Detail::panelTail,
                                         Detail::styleEnd>};

private:
    LogScreen<RtxuiWidth> screen_{RtxuiWidth{}};
    LogViewport           viewport_;
    LogColumns            columns_;
    std::size_t           hOffset_{0};
    std::size_t           rows_{1};
    std::size_t           width_{80};
};

}   // namespace uc_log::printer::rtx
