#pragma once

// The tabs (printer/Tabs.hpp): each with its key, icon and name, as many as fit; a click picks one.

#include "uc_log/printer/Tabs.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Runs.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

namespace Detail {
    inline constexpr char tabBarHtml[] = R"html(
<div class="bar"><for each="{tabs}" as="t"><span class="tab {t.cls}" onclick="Pick({$index})"><span class="key {t.cls}">{t.key}</span><span>{t.text}</span></span></for></div>
<style>
  self { height: 1; flex-shrink: 0; overflow: hidden; }
  .bar { display: flex; height: 1; width: 100%; white-space: pre; overflow: hidden; padding-left: 1;
         color: var(--muted); user-select: none; }
  .tab { flex-shrink: 0; margin-right: 1; background-color: var(--raised); cursor: pointer; }
  .tab:hover { background-color: var(--hover-row); color: var(--bright); }
  .tab.on { background-color: var(--accent); color: var(--ink); font-weight: bold; }
  .key { color: var(--key); }
  .key.on { color: var(--ink); }
</style>
)html";
}   // namespace Detail

class TabBar : public ::rtxui::Component<TabBar> {
public:
    std::vector<Chip>        tabs;
    std::function<void(Tab)> picked;   // a click on a tab

    TabBar();
    /// The tabs for `width` cells: each with its key, icon and name; when that is too wide the names alone, then the
    /// icons alone (the open tab always with both).
    void show(Tab         open,
              std::size_t width);

    std::string_view view{Detail::viewOf<Detail::tabBarHtml>};
};

}   // namespace uc_log::printer::rtx
