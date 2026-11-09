#include "uc_log/printer/rtx/Form.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void Fields(Control const&      control,
            ::rtxui::FieldSink& out) {
    out("cls", control.cls);
    out("text", control.text);
    out("button", control.button);
    out("brk", control.brk);
}

namespace {
    // a button's classes: its look, on or not, its tone, on the marked row
    std::string buttonClass(PanelPart const& part) {
        std::string cls;
        switch(buttonLook(part.detail)) {
        case ButtonLook::action: break;
        case ButtonLook::toggle: cls = "toggle"; break;
        case ButtonLook::chip:   cls = "chip"; break;
        case ButtonLook::danger: cls = "danger"; break;
        case ButtonLook::go:     cls = "go"; break;
        case ButtonLook::row:    cls = "row"; break;
        }
        if(buttonOn(part.detail)) { cls += " on"; }
        if(auto const tone = buttonTone(part.detail); tone != Tone::plain) {
            cls += fmt::format(" t{}", static_cast<unsigned>(tone));
        }
        if(part.selected) { cls += " sel"; }
        return cls;
    }

    // a tool of the head: an action (blue), go (green) or danger (red), on (filled)
    std::string_view toolClass(PanelPart const& part) {
        if(buttonOn(part.detail)) { return "on"; }
        switch(buttonLook(part.detail)) {
        case ButtonLook::danger: return "danger";
        case ButtonLook::go:     return "go";
        case ButtonLook::action:
        case ButtonLook::toggle:
        case ButtonLook::chip:
        case ButtonLook::row:    return {};
        }
        return {};
    }

    // a box's rows as the template's items, and each item's action (1-based; 0: not a button)
    void itemsOf(PageBox const&              box,
                 std::vector<Control>&       items,
                 std::vector<std::uint16_t>& ids) {
        for(std::size_t r = 0; r < box.rows.size(); ++r) {
            if(r != 0) {
                items.push_back({"", "", "", "1"});
                ids.push_back(0);
            }
            for(auto const& part : box.rows[r]) {
                if(part.text.empty()) { continue; }
                if(part.action != 0) {
                    items.push_back({buttonClass(part), part.text, "1", ""});
                    ids.push_back(part.action);
                } else {
                    ScreenRun const run{part.kind,
                                        part.detail,
                                        part.selected,
                                        false,
                                        part.style,
                                        part.text};
                    items.push_back({runClass(run), part.text, "", ""});
                    ids.push_back(0);
                }
            }
        }
    }
}   // namespace

FormBox::FormBox() {
    Import("legend", &FormBox::legend);
    Import("ring", &FormBox::ring);
    Import("items", &FormBox::items);
    Import("hasBar", &FormBox::hasBar);
    Import("barStyle", &FormBox::barStyle);
    Import("bar", &FormBox::bar);
    Import("Hit", [this](std::string const& arg) {
        if(hit) { hit(argIndex(arg)); }
    });
    Import("Bar", [this](std::string const& arg) {
        if(barHit) { barHit(argIndex(arg)); }
    });
}

FormPage::FormPage() {
    Import("title", &FormPage::title);
    Import("hint", &FormPage::hint);
    Import("ring", &FormPage::ring);
    Import("height", &FormPage::height);
    Import("tools", &FormPage::tools);
    Import("Press", [this](std::string const& arg) {
        if(pressed) { pressed(argIndex(arg)); }
    });
    for(std::size_t b = 0; b < maxBoxes; ++b) {
        boxes[b]      = ::rtxui::Ref<FormBox>::New();
        boxes[b]->hit = [this, b](std::size_t item) {
            if(item >= ids_[b].size() || ids_[b][item] == 0) { return; }
            run(ids_[b][item]);
            if(changed) { changed(); }
        };
        boxes[b]->barHit = [this, b](std::size_t cell) {
            // that far into all of its rows, as a text panel's scrollbar
            auto const& s     = scrolls_[b];
            auto const  shown = boxes[b]->bar.size();
            if(!s || shown == 0 || cell >= shown) { return; }
            if(scrolledTo) { scrolledTo(b, s->total * cell / shown); }
        };
    }
    // the boxes are the page's own: each tag reuses its instance
    static constexpr std::array<char const*, maxBoxes>
      tags{"box-0", "box-1", "box-2", "box-3", "box-4", "box-5", "box-6", "box-7"};
    for(std::size_t i = 0; i < maxBoxes; ++i) {
        Import(tags[i], ::rtxui::ComponentFactory{[c = boxes[i]] {
                   return ::rtxui::Ref<::rtxui::ComponentBase>{c};
               }});
    }
    Import("use0", &FormPage::use0);
    Import("use1", &FormPage::use1);
    Import("use2", &FormPage::use2);
    Import("use3", &FormPage::use3);
    Import("use4", &FormPage::use4);
    Import("use5", &FormPage::use5);
    Import("use6", &FormPage::use6);
    Import("use7", &FormPage::use7);
    Import("cls0", &FormPage::cls0);
    Import("cls1", &FormPage::cls1);
    Import("cls2", &FormPage::cls2);
    Import("cls3", &FormPage::cls3);
    Import("cls4", &FormPage::cls4);
    Import("cls5", &FormPage::cls5);
    Import("cls6", &FormPage::cls6);
    Import("cls7", &FormPage::cls7);
}

void FormPage::fill(std::vector<PanelPart> const&      ownTools,
                    std::vector<PageBox>               given,
                    std::vector<std::function<void()>> actions) {
    // more boxes than the page has: the rest go into the last one, each under its title
    if(given.size() > maxBoxes) {
        auto& last = given[maxBoxes - 1];
        for(std::size_t i = maxBoxes; i < given.size(); ++i) {
            last.rows.push_back({toned(Tone::info, given[i].legend)});
            for(auto& r : given[i].rows) { last.rows.push_back(std::move(r)); }
        }
        last.wide = true;
        given.resize(maxBoxes);
    }
    actions_ = std::move(actions);
    formTools_.clear();
    formToolIds_.clear();
    for(auto const& t : ownTools) {
        formTools_.push_back({std::string{toolClass(t)}, "", t.text});
        formToolIds_.push_back(t.action);
    }
    for(std::size_t b = 0; b < maxBoxes; ++b) {
        ids_[b].clear();
        if(b >= given.size()) {
            scrolls_[b].reset();
            showBox(b, false, {});
            continue;
        }
        std::vector<Control> items;
        itemsOf(given[b], items, ids_[b]);
        auto& target = *boxes[b];
        if(target.legend != given[b].legend) { target.legend = given[b].legend; }
        if(target.items != items) { target.items = std::move(items); }
        showBar(b, given[b]);
        showBox(b, true, given[b].wide ? "wide" : "");
    }
}

void FormPage::pressFormTool(std::size_t i) {
    if(i < formToolIds_.size()) { run(formToolIds_[i]); }
}

void FormPage::run(std::uint16_t id) {
    if(id == 0 || id > actions_.size()) { return; }
    auto const action = actions_[id - 1U];   // it fills the page again, and so replaces them
    action();
}

void FormPage::showBar(std::size_t    b,
                       PageBox const& box) {
    auto& target = *boxes[b];
    scrolls_[b]  = box.scroll;
    bool const has
      = box.scroll.has_value() && box.rows.size() > box.scroll->skip && box.scroll->total > 0;
    if(target.hasBar != has) { target.hasBar = has; }
    if(!has) { return; }
    auto const&          s     = *box.scroll;
    auto const           shown = box.rows.size() - s.skip;
    auto const           text  = scrollbarText(shown, s.first, shown, s.total);
    std::vector<Control> cells;
    std::size_t          start = 0;
    while(start <= text.size()) {
        auto const end = text.find('\n', start);
        cells.push_back(
          {"",
           text.substr(start, end == std::string::npos ? std::string::npos : end - start),
           "",
           ""});
        if(end == std::string::npos) { break; }
        start = end + 1;
    }
    auto style = fmt::format("top: {};",
                             1 + s.skip);   // under the frame's top border and the rows that stay
    if(target.barStyle != style) { target.barStyle = std::move(style); }
    if(target.bar != cells) { target.bar = std::move(cells); }
}

void FormPage::showBox(std::size_t i,
                       bool        used,
                       std::string cls) {
    std::array<bool*, maxBoxes> const uses{&use0, &use1, &use2, &use3, &use4, &use5, &use6, &use7};
    std::array<std::string*, maxBoxes> const
      classes{&cls0, &cls1, &cls2, &cls3, &cls4, &cls5, &cls6, &cls7};
    if(i >= maxBoxes) { return; }
    if(*uses[i] != used) { *uses[i] = used; }
    if(*classes[i] != cls) { *classes[i] = std::move(cls); }
}

}   // namespace uc_log::printer::rtx
