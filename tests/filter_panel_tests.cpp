// The front-end-neutral filter panel (FilterPanel.hpp, FilterPicks.hpp): its rows and items, what switching an item
// does to a FilterState (levels, channels, the module tree, a location's round, the noisy locations), the cursor and a
// click on an item, the combo boxes' rows kept in place while the counts change, the summary a window shows, the
// firmware's compiled filter beside a module. No window, no clock.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/LogFilter.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/FilterPanel.hpp"
#include "uc_log/printer/FilterPicks.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace uc_log::printer;

namespace {

FilterSources sources() {
    FilterSources s;
    s.modules   = {"i2c.bus", "i2c.scanner", "usb"};
    s.locations = {
      {{"a.cpp", 1},  5},
      {{"b.cpp", 2}, 50},
      {{"c.cpp", 3},  7}
    };
    return s;
}

// the item a click on the first button of `form` (its tools, then its boxes) whose text holds `text` switches, if any
std::optional<FilterItem> itemOf(PageForm<FilterItem> const& form,
                                 std::string_view            text) {
    for(auto const& part : form.tools) {
        if(part.action != 0 && part.text.contains(text)) {
            return form.actions.at(part.action - 1U);
        }
    }
    for(auto const& box : form.boxes) {
        for(auto const& row : box.rows) {
            for(auto const& part : row) {
                if(part.action != 0 && part.text.contains(text)) {
                    return form.actions.at(part.action - 1U);
                }
            }
        }
    }
    return std::nullopt;
}

void levelsAndChannels() {
    auto const  s = sources();
    FilterState f;
    auto        form  = filterForm(f, s);
    auto const  trace = itemOf(form, "✔ trace");
    CHECK(trace && form.boxes[0].legend == "📊 Levels" && form.boxes[0].rows[0].size() == 6,
          "six levels, all on");
    if(!trace) { return; }
    f = switchFilterItem(f, *trace, s);
    CHECK(f.enabledLogLevels.size() == 5 && !f.enabledLogLevels.contains(uc_log::LogLevel::trace),
          "switching one off shows the other five");
    form = filterForm(f, s);
    CHECK(itemOf(form, "✘ trace").has_value(), "and its switch says it");
    f = switchFilterItem(f, itemOf(form, "✘ trace").value_or(FilterItem{}), s);
    CHECK(f.enabledLogLevels.empty(), "back on: every level again (an empty set)");
    auto const channel = itemOf(form, "✔ 3");
    CHECK(channel && switchFilterItem(f, *channel, s).enabledChannels.size() == filterChannels - 1,
          "a channel off: the other five");
}

// the picks' row of `text` (its indent aside), if any
PickRow const* pickNamed(std::vector<PickRow> const& rows UC_LOG_LIFETIMEBOUND,
                         std::string_view                 text) {
    for(auto const& r : rows) {
        if(r.text.substr(r.text.find_first_not_of(' ')) == text) { return &r; }
    }
    return nullptr;
}

void moduleTree() {
    auto const s     = sources();
    auto const nodes = moduleNodes(s);
    CHECK_EQ(nodes.size(), std::size_t{4}, "i2c (a prefix), i2c.bus, i2c.scanner, usb");
    FilterState f;
    auto const  item = [](std::string const& node) {
        FilterItem i;
        i.action = FilterAction::module;
        i.name   = node;
        return i;
    };
    auto picks = modulePicks(f, s, "");
    CHECK(picks.size() == 4 && picks[1].text == "  i2c.bus" && picks[1].mark == "✔",
          "the combo box: the tree, a submodule indented, all shown");
    f = switchFilterItem(f, item("i2c.bus"), s);
    f = switchFilterItem(f, item("i2c"), s);
    std::set<std::string> const justI2c{"i2c"};
    CHECK(f.excludedModules == justI2c, "hiding a module drops its hidden submodules");
    picks = modulePicks(f, s, "");
    CHECK(pickNamed(picks, "i2c")->mark == "✘" && pickNamed(picks, "i2c.scanner")->mark == "(✘)",
          "a hidden module, and one its parent hides");
    auto const form = filterForm(f, s);
    CHECK(itemOf(form, "Choose … (4 modules)").has_value() && itemOf(form, "i2c ✕").has_value(),
          "the page: the combo box's chip, a chip for the hidden one");
    f = switchFilterItem(f, item("i2c.scanner"), s);
    CHECK(f.excludedModules.empty(),
          "a submodule switched on shows the ancestor that hid it again");
    CHECK(modulePicks(f, s, "SCAN").size() == 1 && modulePicks(f, s, "i2c bus").size() == 1,
          "the search: each word, case aside");
    f = switchFilterItem(f, item("usb"), s);
    f = switchFilterItem(f, itemOf(filterForm(f, s), "usb ✕").value_or(FilterItem{}), s);
    CHECK(f.excludedModules.empty(), "a click on a hidden module's chip shows it again");
}

void locationsCycle() {
    auto const  s = sources();
    FilterState f;
    auto        picks = locationPicks(f, s, "");
    CHECK(picks.size() == 3 && picks[0].text == "b.cpp:2" && picks[0].detail == "×50"
            && picks[0].mark == " · ",
          "the busiest first, its count");
    auto const busiest = picks[0].item;
    f                  = switchFilterItem(f, busiest, s);
    CHECK(f.excludedLocations.contains(SourceLocation{"b.cpp", 2})
            && locationPicks(f, s, "")[0].mark == " ✘ ",
          "hidden");
    f = switchFilterItem(f, busiest, s);
    CHECK(f.includedLocations.contains(SourceLocation{"b.cpp", 2}) && f.excludedLocations.empty()
            && locationPicks(f, s, "")[0].mark == " ✔ "
            && itemOf(filterForm(f, s), "only b.cpp:2 ✕").has_value(),
          "then only this one, a chip on the page");
    f = switchFilterItem(f, busiest, s);
    CHECK(f == FilterState{}, "then shown again");
    // a location by what it is: the counts change while the log runs, and with them the order
    auto busier         = s;
    busier.locations[2] = {
      {"c.cpp", 3},
      500
    };   // now ahead of b.cpp:2
    f = switchFilterItem(f, busiest, busier);
    std::set<SourceLocation> const justB{
      SourceLocation{"b.cpp", 2}
    };
    CHECK(f.excludedLocations == justB, "the location the row was, not the one in its place now");
    f = FilterState{};
    f.excludedLocations.insert(SourceLocation{"c.cpp", 3});
    f = switchFilterItem(f, itemOf(filterForm(f, s), "c.cpp:3 ✕").value_or(FilterItem{}), s);
    CHECK(f == FilterState{}, "a click on a hidden location's chip shows it");
}

void noisyLocations() {
    FilterSources s;
    for(int i = 0; i < 8; ++i) {
        s.locations.push_back({
          {"q.cpp", static_cast<std::size_t>(i + 1)},
          10
        });
    }
    s.locations.push_back({
      {"loud.cpp", 1},
      10'000
    });
    auto const f
      = switchFilterItem({}, FilterItem{FilterAction::hideNoisy, 0, {}, false, Tone::plain}, s);
    std::set<SourceLocation> const loud{
      SourceLocation{"loud.cpp", 1}
    };
    CHECK(f.excludedLocations == loud, "the one far above the rest");
}

void formBoxes() {
    auto const               s    = sources();
    auto const               form = filterForm({}, s);
    std::vector<std::string> legends;
    legends.reserve(form.boxes.size());
    for(auto const& box : form.boxes) { legends.push_back(box.legend); }
    std::vector<std::string> const expected{"📊 Levels",
                                            "📡 Channels",
                                            "⏱ Target time",
                                            "🧩 Modules",
                                            "📍 Locations"};
    CHECK(legends == expected, "the boxes, in order");
    CHECK(!form.boxes[0].wide && !form.boxes[2].wide && form.boxes[3].wide && form.boxes[4].wide,
          "levels, channels and the time side by side; the combo boxes the whole width");
    auto const all = itemOf(form, "◉ all");
    auto const ten = itemOf(form, "○ last 10 s");
    CHECK(all && all->action == FilterAction::timeAll && ten
            && ten->action == FilterAction::timeLast10,
          "the time: one choice chosen");
    auto const show = itemOf(form, "Show everything");
    CHECK(show && show->action == FilterAction::showAll && !form.tools.empty()
            && buttonLook(form.tools[0].detail) == ButtonLook::danger && form.tools.size() == 4,
          "the whole filter's actions are the page's tools: show everything a danger one, hide the "
          "noisy, save, load");
    CHECK(itemOf(form, "all shown").has_value() == false
            && itemOf(form, "Choose … (3 locations)").has_value(),
          "nothing hidden: no chips, the combo boxes' buttons");
}

void summaryAndCompiled() {
    FilterState f;
    CHECK(filterSummary(f, {}).empty(), "nothing hidden: nothing to say");
    f.enabledLogLevels = {uc_log::LogLevel::warn, uc_log::LogLevel::error};
    f.excludedModules  = {"usb"};
    EntryStore::UcTimeWindow t;
    t.enabled        = true;
    t.live           = true;
    t.liveWindowSecs = 10;
    CHECK_EQ(filterSummary(f, t),
             std::string{"2 of 6 levels · 1 module hidden · the last 10 s"},
             "what is hidden");

    uc_log::Detail::FilterTable table;
    table.count = 1;
    std::string_view const name{"i2c"};
    std::ranges::copy(name, table.rules[0].module.begin());
    table.rules[0].size  = 3;
    table.rules[0].level = 3;   // warn
    auto s               = sources();
    s.compiled           = &table;
    auto const  form     = filterForm({}, s);
    auto const  picks    = modulePicks({}, s, "");
    auto const* i2c      = pickNamed(picks, "i2c");
    CHECK(i2c != nullptr && i2c->detail == "(compiled ≥ warn)"
            && itemOf(form, "Choose … (4 modules)").has_value(),
          "what the firmware compiled, beside the module in the combo box");
}

void picksKeepTheirPlaces() {
    auto const  s = sources();
    FilterState f;
    auto const  shown  = locationPicks(f, s, "");   // b.cpp:2 (50), c.cpp:3 (7), a.cpp:1 (5)
    auto        busier = s;
    busier.locations[0].second = 500;   // a.cpp:1 the busiest now
    busier.locations.push_back({
      {"d.cpp", 4},
      1
    });
    auto const fresh = locationPicks(f, busier, "");
    CHECK(fresh.size() == 4 && fresh[0].text == "a.cpp:1", "counted anew: the busiest first");
    auto const kept = keepPlaces(fresh, shown);
    CHECK(kept.size() == 4 && kept[0].text == "b.cpp:2" && kept[1].text == "c.cpp:3"
            && kept[2].text == "a.cpp:1" && kept[2].detail == "×500" && kept[3].text == "d.cpp:4",
          "kept in the order shown, with the new counts; a location seen since after them");
    CHECK(placeOf(kept, shown[2].item) == 2U && placeOf(fresh, shown[2].item) == 0U,
          "a row found by what it is, wherever it stands");
    FilterItem relabelled = shown[0].item;
    relabelled.text       = "something else";
    relabelled.on         = true;
    CHECK(sameItem(relabelled, shown[0].item) && !sameItem(shown[0].item, shown[1].item),
          "the same item: what it switches and which, not how it reads");
    FilterItem module;
    module.action = FilterAction::module;
    module.name   = "usb";
    CHECK(!placeOf(kept, module).has_value(), "a module is no location");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {   "levelsAndChannels",    levelsAndChannels},
                               {          "moduleTree",           moduleTree},
                               {      "locationsCycle",       locationsCycle},
                               {      "noisyLocations",       noisyLocations},
                               {           "formBoxes",            formBoxes},
                               {  "summaryAndCompiled",   summaryAndCompiled},
                               {"picksKeepTheirPlaces", picksKeepTheirPlaces},
    });
}
