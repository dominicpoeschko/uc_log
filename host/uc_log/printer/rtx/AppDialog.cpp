// The App: its one dialog - the command palette and the filter's combo boxes (modules, locations), and the list a
// question shares (AppQuestions.cpp): its rows for the field's text, the marked row and the rows that fit, a click or
// a key on them.

#include "uc_log/printer/rtx/App.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void App::openDialog(Modal modal) {
    auto& d             = dialogState_;
    d.modal             = modal;
    d.shownFor          = DialogState::nothingShown;
    dialog->open        = true;
    dialog->query       = {};
    dialog->error       = {};
    dialog->placeholder = "type to narrow";
    switch(modal) {
    case Modal::palette:
        dialog->title       = "⌨ Commands";
        dialog->about       = "every command by its name, its keys on the left";
        dialog->placeholder = "type to choose";
        dialog->hint        = "↑↓ choose · Enter or a click runs it";
        dialog->buttons     = {
          {  "", "", "Cancel"},
          {"go", "",  "▶ Run"}
        };
        break;
    case Modal::modules:
        dialog->title   = "🧩 Modules - which the log shows";
        dialog->about   = "✔ shown, ✘ hidden, (✘) hidden with the module above it";
        dialog->hint    = "↑↓ PgUp PgDn choose · Enter or a click switches one";
        dialog->buttons = {
          {"go", "", "✓ Done"}
        };
        break;
    case Modal::locations:
        dialog->title   = "📍 Locations - which the log shows";
        dialog->about   = "a click goes round: · shown, ✘ hidden, ✔ only these";
        dialog->hint    = "↑↓ PgUp PgDn choose · Enter or a click switches one";
        dialog->buttons = {
          {"go", "", "✓ Done"}
        };
        break;
    case Modal::question:
    case Modal::none:     break;
    }
    fillDialog(true);
}

std::size_t App::dialogSize() const {
    auto const& d = dialogState_;
    switch(d.modal) {
    case Modal::palette:   return d.commands.size();
    case Modal::modules:
    case Modal::locations: return d.picks.size();
    case Modal::question:  return d.choices.size();
    case Modal::none:      return 0;
    }
    return 0;
}

std::size_t App::dialogRows() const {
    // the window less the dialog's frame, head, about, field, error, count and foot (with their gaps)
    return static_cast<std::size_t>(std::clamp(height_ - 12, 3, 20));
}

bool App::leavesForField() const {
    auto const& d = dialogState_;
    return d.modal == Modal::question && !questionOf(d.asked).firstMarked;
}

void App::fillDialog(bool again) {
    auto& d = dialogState_;
    if(d.modal == Modal::none) { return; }
    auto const& text    = dialog->query;
    bool const  newText = text != d.shownFor;
    if(again || newText) {
        refreshRows(newText);
        if(newText) {
            // a new text starts at its first row - or, for a value of its own, at the field
            d.cursor.reset(leavesForField());
            if(d.modal == Modal::question && text != d.askedValue) { dialog->error = {}; }
        }
        d.shownFor = text;
    }
    d.cursor.fit(dialogSize(), dialogRows());

    std::vector<DialogRow> rows;
    for(std::size_t i = d.cursor.top; i < dialogSize() && i < d.cursor.top + dialogRows(); ++i) {
        rows.push_back(dialogRow(i));
    }
    if(rows != dialog->rows) { dialog->rows = std::move(rows); }
    auto count = dialogCount();
    if(dialog->count != count) { dialog->count = std::move(count); }
    if(d.modal == Modal::question) {
        if(auto const hint = questionHint(); dialog->hint != hint) { dialog->hint = hint; }
    }
    auto size = fmt::format("width: {}; max-height: {};", std::clamp(width_ - 4, 20, 100), height_);
    if(dialog->size != size) { dialog->size = std::move(size); }
}

void App::refreshRows(bool newText) {
    auto&       d    = dialogState_;
    auto const& text = dialog->query;
    switch(d.modal) {
    case Modal::palette: d.commands = matchCommands(text); break;
    case Modal::modules:
    case Modal::locations:
        {
            auto fresh = d.modal == Modal::modules ? modulePicks(filter_, filterSources(), text)
                                                   : locationPicks(filter_, filterSources(), text);
            if(!newText) {
                // while the box is open its rows keep their places (the counts change all the time) and the mark
                // stays on its row, by what the row is
                if(d.modal == Modal::locations) { fresh = keepPlaces(std::move(fresh), d.picks); }
                if(d.cursor.marked && *d.cursor.marked < d.picks.size()) {
                    if(auto const at = placeOf(fresh, d.picks[*d.cursor.marked].item)) {
                        d.cursor.marked = at;
                    }
                }
            }
            d.picks = std::move(fresh);
            break;
        }
    case Modal::question: d.choices = choicesFor(text); break;
    case Modal::none:     break;
    }
}

DialogRow App::dialogRow(std::size_t i) const {
    auto const& d   = dialogState_;
    std::string cls = d.cursor.marked == i ? "marked" : "";
    auto const  add = [&](std::string_view more) {
        if(!cls.empty()) { cls += ' '; }
        cls += more;
    };
    auto const after
      = [](std::string const& detail) { return detail.empty() ? std::string{} : "  " + detail; };
    switch(d.modal) {
    case Modal::palette:
        {
            auto const& info = infoOf(d.commands[i]);
            return {std::move(cls), fmt::format("{:<11} ", info.keys), std::string{info.name}, {}};
        }
    case Modal::modules:
    case Modal::locations:
        {
            auto const& r = d.picks[i];
            if(r.tone == Tone::warn) { add("off"); }
            if(r.tone == Tone::ok) { add("on"); }
            return {std::move(cls), fmt::format("{:<5}", r.mark), r.text, after(r.detail)};
        }
    case Modal::question:
        {
            auto const& c = d.choices[i];
            if(c.on) { add("on"); }
            if(c.into) { add("into"); }
            return {std::move(cls), c.on ? "✓ " : "  ", c.text, after(c.detail)};
        }
    case Modal::none: break;
    }
    return {};
}

std::string App::dialogCount() const {
    auto const& d     = dialogState_;
    auto const  n     = dialogSize();
    auto const  shown = dialogRows();
    auto const  range
      = n > shown
        ? fmt::format(" · {}-{} shown", d.cursor.top + 1, std::min(n, d.cursor.top + shown))
        : std::string{};
    if(d.modal == Modal::modules || d.modal == Modal::locations) {
        auto const all  = d.modal == Modal::modules ? moduleNodes(filterSources()).size()
                                                    : filterSources().locations.size();
        auto const what = d.modal == Modal::modules ? "modules" : "locations";
        return (n == all ? fmt::format("{} {}", all, what) : fmt::format("{} of {} match", n, all))
             + range;
    }
    if(d.modal == Modal::question && d.asked == Field::watch) {
        if(dialog->query.empty()) { return "the data symbols of the map: type part of a name"; }
        if(d.symbols.total == 0) { return "no data symbol of the map matches"; }
        return fmt::format("{} match{}{}",
                           d.symbols.total,
                           d.symbols.total == 1 ? "" : "es",
                           d.symbols.total > n ? fmt::format(", the best {} listed", n)
                                               : std::string{})
             + range;
    }
    if(n > shown) {
        return fmt::format("{} {}", n, d.modal == Modal::palette ? "commands" : "choices") + range;
    }
    return {};
}

std::string_view App::questionHint() const {
    if(dialogSize() == 0) { return "Enter uses it · Esc cancels"; }
    if(!leavesForField()) { return "↑↓ choose · Enter or a click watches it"; }
    return "↑↓ a choice · Tab takes it · Enter or a click uses it";
}

void App::closeDialog() {
    dialogState_.close();
    dialog->open = false;
}

void App::pickRow(std::size_t index) {
    auto& d = dialogState_;
    if(index >= dialogSize()) { return; }
    d.cursor.marked = index;
    switch(d.modal) {
    case Modal::palette:
        {
            auto const command = d.commands[index];
            closeDialog();
            run(command);
            return;
        }
    case Modal::modules:
    case Modal::locations:
        {
            auto const& row  = d.picks[index];
            auto const  name = std::string{row.text.substr(row.text.find_first_not_of(' '))};
            auto        next = switchFilterItem(filter_, row.item, filterSources());
            applyFilter(
              std::move(next),
              fmt::format("{} {}", name, d.modal == Modal::modules ? "switched" : "goes round"));
            fillDialog(true);
            return;
        }
    case Modal::question:
        {
            auto const choice = d.choices[index];   // the answer may close the dialog
            if(choice.into) {
                // a directory: its entries next, the dialog stays
                dialog->query = choice.value;
                fillDialog(false);
                return;
            }
            answer(choice.value);
            return;
        }
    case Modal::none: return;
    }
}

void App::acceptDialog(bool field) {
    // Enter: the marked row (a directory: into it), else the field's text; ✓: the field's text, unless a choice
    // that answers is marked
    auto const& d      = dialogState_;
    auto const  marked = d.cursor.marked;
    if(d.modal == Modal::question
       && (!marked || *marked >= d.choices.size() || (field && d.choices[*marked].into)))
    {
        answer(dialog->query);
        return;
    }
    if(marked) { pickRow(*marked); }
}

bool App::dialogEvent(::rtxui::Event const& event) {
    using Keyboard = ::rtxui::Event::Keyboard;
    using Mouse    = ::rtxui::Event::Mouse;
    auto& d        = dialogState_;
    if(auto const k = event.get_if<Keyboard>()) {
        auto const special = k->special();
        if(special == Keyboard::Escape) {
            closeDialog();
            relayout();
            return true;
        }
        fillDialog(false);   // what was typed in this burst
        if(special == Keyboard::Return) {
            acceptDialog(false);
            relayout();
            return true;
        }
        if(special == Keyboard::Tab || special == Keyboard::TabReverse) {
            // the focus stays in the field (the screen would move it to the page behind); Tab takes the marked
            // choice into it
            if(d.modal == Modal::question && d.cursor.marked && *d.cursor.marked < d.choices.size())
            {
                dialog->query = d.choices[*d.cursor.marked].value;
                fillDialog(false);
            }
            return true;
        }
        long step = 0;
        if(special == Keyboard::ArrowUp) { step = -1; }
        if(special == Keyboard::ArrowDown) { step = 1; }
        if(special == Keyboard::PageUp) { step = -10; }
        if(special == Keyboard::PageDown) { step = 10; }
        if(step != 0) {
            d.cursor.move(step, dialogSize(), leavesForField());
            fillDialog(false);
            return true;
        }
    }
    if(auto const m = event.get_if<Mouse>();
       m && (m->button() == Mouse::WheelUp || m->button() == Mouse::WheelDown))
    {
        d.cursor.move(m->button() == Mouse::WheelUp ? -3 : 3, dialogSize(), leavesForField());
        fillDialog(false);
        return true;
    }
    // typing into the field, a press on it; the rows, the buttons and the backdrop take their own clicks
    return ComponentBase::OnEvent(event);
}

}   // namespace uc_log::printer::rtx
