#pragma once

// The mode line at the bottom: where the log view stands, a short notice, the key hints (a click runs one).

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
    inline constexpr char modeLineHtml[] = R"html(
<div class="line">
  <span class="position">{position}</span>
  <span class="notice">{notice}</span>
  <span class="hints"><for each="{hints}" as="h"><span class="hint" onclick="Hint({$index})"><span class="key">{h.key}</span><span>{h.text}</span></span></for></span>
</div>
<style>
  self { height: 1; flex-shrink: 0; }
  .line { display: flex; height: 1; white-space: pre; overflow: hidden; background-color: var(--surface); }
  .position { color: var(--accent); flex-shrink: 0; }
  .notice { color: var(--warn); flex-shrink: 0; }
  .hints { color: var(--dim); flex-grow: 1; text-align: right; overflow: hidden; }
  .hint { user-select: none; cursor: pointer; margin-left: 1; }
  .hint:hover { background-color: var(--hover-row); color: var(--bright); }
  .key { color: var(--ink); background-color: var(--key); padding: 0 1; }
</style>
)html";
}   // namespace Detail

class ModeLine : public ::rtxui::Component<ModeLine> {
public:
    std::string       position;   // "follow · 12 345 lines" / "line 12 / 345"
    std::string       notice;     // a short result: copied, exported, no line at that time
    std::vector<Chip> hints;
    std::function<void(std::size_t)> hinted;   // a click on hints[i]

    ModeLine();

    std::string_view view{Detail::viewOf<Detail::modeLineHtml>};
};

}   // namespace uc_log::printer::rtx
