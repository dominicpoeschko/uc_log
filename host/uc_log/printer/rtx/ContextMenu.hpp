#pragma once

// The log's right-click menu: what can be done with the line, where the mouse is.

#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

/// An item of the menu as its template shows it.
struct MenuItem {
    std::string text;
    std::string key;   // the key that does the same

    bool operator==(MenuItem const&) const = default;
};

void Fields(MenuItem const&     item,
            ::rtxui::FieldSink& out);

namespace Detail {
    inline constexpr char contextMenuHtml[] = R"html(
<if condition="{open}">
  <div class="backdrop" style="{area}" onclick="Close" oncontextmenu="Close"></div>
  <div class="menu" style="{place}">
    <div class="title">{title}</div>
    <for each="{items}" as="i"><div class="item" onclick="Pick({$index})"><span class="text">{i.text}</span><span class="key">{i.key}</span></div></for>
  </div>
</if>
<style>
  self { position: absolute; top: 0; left: 0; }
  .backdrop { position: absolute; top: 0; left: 0; z-index: 99; }
  .menu { position: absolute; z-index: 100; width: 40; border: round; border-color: var(--accent);
          background-color: var(--surface); color: var(--text); user-select: none; }
  .title { padding: 0 1; color: var(--dim); white-space: pre; overflow: hidden; }
  .item { display: flex; padding: 0 1; cursor: pointer; white-space: pre; }
  .item:hover { background-color: var(--hover); color: var(--ink); }
  .text { flex-grow: 1; }
  .key { color: var(--key); }
  .item:hover .key { color: var(--ink); }
</style>
)html";
}   // namespace Detail

/// An item to offer: its text, the key that does the same, what a click on it does.
struct MenuEntry {
    std::string           text;
    std::string           key;
    std::function<void()> action;
};

/// What a right click on a log row offers: a menu where the mouse is, closed by a click (either button) beside it.
class ContextMenu : public ::rtxui::Component<ContextMenu> {
public:
    bool                  open{false};
    std::string           place{"top: 0; left: 0;"};
    std::string           area{"width: 80; height: 24;"};   // the backdrop: the whole window
    std::string           title;
    std::vector<MenuItem> items;
    std::function<void()> changed;   // an item ran its action: the window lays out anew

    ContextMenu();

    /// The menu with `entries` under `heading`, its corner at the mouse (`x`, `y`: 1-based cells) as far as it fits a
    /// window of `width` x `height`.
    void openAt(std::string            heading,
                std::vector<MenuEntry> entries,
                int                    x,
                int                    y,
                int                    width,
                int                    height);

    std::string_view view{Detail::viewOf<Detail::contextMenuHtml>};

private:
    std::vector<std::function<void()>> actions_;
};

}   // namespace uc_log::printer::rtx
