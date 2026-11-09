#pragma once

// The status line: the badges (printer/StatusBadges.hpp) as pills coloured by their tone; a click on one with a
// command runs it.

#include "uc_log/printer/Commands.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/rtx/Rtxui.hpp"
#include "uc_log/printer/rtx/Theme.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

/// A status badge as the status line draws it: a pill coloured by its tone, clickable when it has a command.
struct BadgeChip {
    std::string cls;
    std::string text;
    std::string click;   // "1": a click runs its command

    bool operator==(BadgeChip const&) const = default;
};

void Fields(BadgeChip const&    chip,
            ::rtxui::FieldSink& out);

namespace Detail {
    inline constexpr char statusLineHtml[] = R"html(
<div class="bar"><for each="{badges}" as="b"><if condition="{b.click}"><span class="badge {b.cls}" onclick="Pick({$index})">{b.text}</span></if><else><span class="badge {b.cls}">{b.text}</span></else></for></div>
<style>
  self { height: 1; flex-shrink: 0; }
  .bar { display: flex; height: 1; white-space: pre; padding-left: 1; user-select: none; }
  .badge { padding: 0 1; margin-right: 1; background-color: var(--raised); color: var(--muted); }
  .badge.ok { background-color: var(--ok-fill); color: var(--ok-text); }
  .badge.info { background-color: var(--info-fill); color: var(--info-text); }
  .badge.warn { background-color: var(--warn-fill); color: var(--warn-text); }
  .badge.bad { background-color: var(--danger); color: var(--bright); }
  .badge.strong { font-weight: bold; }
  .badge.click { cursor: pointer; }
  .badge.click:hover { background-color: var(--hover); color: var(--ink); }
</style>
)html";
}   // namespace Detail

class StatusLine : public ::rtxui::Component<StatusLine> {
public:
    std::vector<BadgeChip>       badges;
    std::function<void(Command)> chosen;   // a click on a badge

    StatusLine();
    void show(std::vector<Badge> const& next);

    std::string_view view{Detail::viewOf<Detail::statusLineHtml>};

private:
    std::vector<Badge> shown_;
};

}   // namespace uc_log::printer::rtx
