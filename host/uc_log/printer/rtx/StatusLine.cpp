#include "uc_log/printer/rtx/StatusLine.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

namespace {
    std::string_view toneClass(Tone tone) {
        switch(tone) {
        case Tone::plain: return "plain";
        case Tone::ok:    return "ok";
        case Tone::info:  return "info";
        case Tone::warn:  return "warn";
        case Tone::bad:   return "bad";
        }
        return "plain";
    }
}   // namespace

void Fields(BadgeChip const&    chip,
            ::rtxui::FieldSink& out) {
    out("cls", chip.cls);
    out("text", chip.text);
    out("click", chip.click);
}

StatusLine::StatusLine() {
    Import("badges", &StatusLine::badges);
    Import("Pick", [this](std::string const& arg) {
        std::size_t i{};
        std::from_chars(std::to_address(arg.begin()), std::to_address(arg.end()), i);
        if(i < shown_.size() && shown_[i].command && chosen) { chosen(*shown_[i].command); }
    });
}

void StatusLine::show(std::vector<Badge> const& next) {
    if(next == shown_) { return; }
    shown_ = next;
    std::vector<BadgeChip> chips;
    chips.reserve(next.size());
    for(auto const& b : next) {
        auto cls = std::string{toneClass(b.tone)};
        if(b.strong) { cls += " strong"; }
        if(b.command) { cls += " click"; }
        chips.push_back({std::move(cls), b.text, b.command ? "1" : ""});
    }
    badges = std::move(chips);
}

}   // namespace uc_log::printer::rtx
