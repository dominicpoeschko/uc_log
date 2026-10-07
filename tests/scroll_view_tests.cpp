// Unit test for uc_log::FTXUIGui::ScrollView: a view of an element built anew every frame that
// scrolls by wheel and by key - and for the reason it exists: the same element in a Renderer
// with ftxui::frame takes no event and never moves.
#include "uc_log/FTXUI_Utils.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

namespace {

constexpr int Width  = 12;
constexpr int Height = 5;

ftxui::Element rows(int count) {
    std::vector<ftxui::Element> out;
    for(int i = 0; i != count; ++i) { out.push_back(ftxui::text("row " + std::to_string(i))); }
    return ftxui::vbox(std::move(out));
}

// the text of the screen's first line, as the component draws it into a Width x Height screen
std::string firstLine(ftxui::Component const& component) {
    auto screen
      = ftxui::Screen::Create(ftxui::Dimension::Fixed(Width), ftxui::Dimension::Fixed(Height));
    ftxui::Render(screen, component->Render());
    std::string line;
    for(int x = 0; x != 6; ++x) { line += screen.PixelAt(x, 0).character; }
    while(!line.empty() && line.back() == ' ') { line.pop_back(); }
    return line;
}

ftxui::Event wheel(bool down,
                   int  x = 2,
                   int  y = 2) {
    ftxui::Mouse mouse{};
    mouse.button = down ? ftxui::Mouse::WheelDown : ftxui::Mouse::WheelUp;
    mouse.motion = ftxui::Mouse::Pressed;
    mouse.x      = x;
    mouse.y      = y;
    return ftxui::Event::Mouse("", mouse);
}

void frameAloneDoesNotScroll() {
    ftxui::Component plain = ftxui::Renderer(
      []() { return rows(20) | ftxui::vscroll_indicator | ftxui::frame | ftxui::flex; });
    CHECK(firstLine(plain) == "row 0", "frame: starts at the top");
    CHECK(!plain->OnEvent(wheel(true)), "frame: a Renderer does not take the wheel");
    CHECK(!plain->OnEvent(ftxui::Event::ArrowDown), "frame: nor a key");
    CHECK(firstLine(plain) == "row 0", "frame: and nothing in it is focused, so it stays put");
}

void wheelScrolls() {
    int              count = 20;
    ftxui::Component view  = uc_log::FTXUIGui::ScrollView([&]() { return rows(count); });
    CHECK(!view->Focusable(), "before the first render there is nothing to scroll");
    CHECK(firstLine(view) == "row 0", "starts at the top");
    CHECK(view->Focusable(), "20 rows in 5: takes the focus");

    CHECK(view->OnEvent(wheel(true)), "wheel down over the view is handled");
    CHECK(firstLine(view) == "row 3", "wheel down: three rows");
    CHECK(view->OnEvent(wheel(false)), "wheel up over the view is handled");
    CHECK(firstLine(view) == "row 0", "wheel up: back");
    CHECK(view->OnEvent(wheel(false)), "wheel up at the top is still taken, not a focus move");

    CHECK(!view->OnEvent(wheel(true, 2, Height + 3)), "the wheel outside the view is not its");
    CHECK(firstLine(view) == "row 0", "and does not move it");

    for(int i = 0; i != 10; ++i) { CHECK(view->OnEvent(wheel(true)), "wheel down to the end"); }
    CHECK(firstLine(view) == "row 15", "stops with the last row at the bottom");

    count = 3;
    CHECK(firstLine(view) == "row 0", "content that got shorter is shown from its top");
    CHECK(!view->Focusable(), "3 rows in 5: nothing to scroll, no focus");
}

void keysScroll() {
    ftxui::Component view = uc_log::FTXUIGui::ScrollView([]() { return rows(20); });
    (void)firstLine(view);
    CHECK(!view->OnEvent(ftxui::Event::ArrowUp), "Up on the first row: left to the container");
    CHECK(view->OnEvent(ftxui::Event::ArrowDown), "Down");
    CHECK(firstLine(view) == "row 1", "Down: one row");
    CHECK(view->OnEvent(ftxui::Event::Character('j')), "j");
    CHECK(view->OnEvent(ftxui::Event::Character('k')), "k");
    CHECK(firstLine(view) == "row 1", "j then k: where it was");
    CHECK(view->OnEvent(ftxui::Event::PageDown), "PageDown");
    CHECK(firstLine(view) == "row 5", "PageDown: a page less one row");
    CHECK(view->OnEvent(ftxui::Event::End), "End");
    CHECK(firstLine(view) == "row 15", "End: the last page");
    CHECK(!view->OnEvent(ftxui::Event::ArrowDown), "Down on the last page: not handled");
    CHECK(view->OnEvent(ftxui::Event::PageUp), "PageUp");
    CHECK(firstLine(view) == "row 11", "PageUp");
    CHECK(view->OnEvent(ftxui::Event::Home), "Home");
    CHECK(firstLine(view) == "row 0", "Home: the top");
    CHECK(!view->OnEvent(ftxui::Event::Character('x')), "another key is not handled");
}

void showFollowsARow() {
    auto             view = uc_log::FTXUIGui::ScrollView([]() { return rows(20); });
    ftxui::Component base = view;
    (void)firstLine(base);
    view->Show(2, 3);
    CHECK(firstLine(base) == "row 0", "rows in view: nothing moves");
    view->Show(8, 9);
    CHECK(firstLine(base) == "row 5", "rows below: scrolled until the last of them shows");
    view->Show(2, 3);
    CHECK(firstLine(base) == "row 2", "rows above: scrolled until the first of them shows");
    view->Show(18, 19);
    CHECK(firstLine(base) == "row 15", "the last rows");
    CHECK(base->OnEvent(wheel(false)), "the wheel after Show()");
    CHECK(firstLine(base) == "row 12", "moves the view alone: Show() applies once");
}

ftxui::Event mouseAt(int                  x,
                     int                  y,
                     ftxui::Mouse::Button button,
                     ftxui::Mouse::Motion motion) {
    ftxui::Mouse mouse{};
    mouse.button = button;
    mouse.motion = motion;
    mouse.x      = x;
    mouse.y      = y;
    return ftxui::Event::Mouse("", mouse);
}

ftxui::Event click(int x,
                   int y) {
    return mouseAt(x, y, ftxui::Mouse::Left, ftxui::Mouse::Pressed);
}

ftxui::Event hover(int x,
                   int y) {
    return mouseAt(x, y, ftxui::Mouse::None, ftxui::Mouse::Moved);
}

struct Picked {
    int count{20};
    int selected{0};
    int clicked{-1};
    int clickedLine{-1};
    int clickedColumn{-1};
    int entered{-1};
    int deleted{-1};

    uc_log::FTXUIGui::PickListOption option(int rowsPerItem) {
        uc_log::FTXUIGui::PickListOption o;
        o.count = [this]() { return count; };
        o.row   = [rowsPerItem](int index, uc_log::FTXUIGui::PickListOption::RowState s) {
            std::vector<ftxui::Element> lines{ftxui::text(std::string{s.selected  ? ">"
                                                                      : s.hovered ? "~"
                                                                                  : " "}
                                                          + std::to_string(index))};
            for(int i = 1; i < rowsPerItem; ++i) { lines.push_back(ftxui::text("  value")); }
            return ftxui::vbox(std::move(lines));
        };
        o.rowsPerItem = rowsPerItem;
        o.selected    = &selected;
        o.onClick     = [this](int index, int line, int column) {
            clicked       = index;
            clickedLine   = line;
            clickedColumn = column;
        };
        o.onEnter  = [this](int index) { entered = index; };
        o.onDelete = [this](int index) { deleted = index; };
        return o;
    }
};

void pickListKeys() {
    Picked           p;
    ftxui::Component list = uc_log::FTXUIGui::PickList(p.option(1));
    CHECK(list->Focusable(), "pick list: items make it focusable");
    CHECK(firstLine(list) == ">0", "pick list: the first item is picked and marked");
    CHECK(!list->OnEvent(ftxui::Event::ArrowUp), "pick list: Up on the first item is not its");
    CHECK(list->OnEvent(ftxui::Event::ArrowDown) && p.selected == 1, "pick list: Down");
    CHECK(firstLine(list) == " 0", "pick list: the view stays while the item is in it");
    for(int i = 0; i != 5; ++i) { (void)list->OnEvent(ftxui::Event::Character('j')); }
    CHECK(p.selected == 6 && firstLine(list) == " 2", "pick list: the view follows the item down");
    CHECK(list->OnEvent(ftxui::Event::End) && p.selected == 19, "pick list: End");
    CHECK(firstLine(list) == " 15", "pick list: the last page");
    CHECK(!list->OnEvent(ftxui::Event::ArrowDown), "pick list: Down on the last is not its");
    CHECK(list->OnEvent(ftxui::Event::PageUp) && p.selected == 15, "pick list: PageUp");
    CHECK(list->OnEvent(ftxui::Event::Home) && p.selected == 0, "pick list: Home");
    CHECK(firstLine(list) == ">0", "pick list: back at the top");
    CHECK(list->OnEvent(ftxui::Event::Return) && p.entered == 0, "pick list: Return");
    (void)list->OnEvent(ftxui::Event::ArrowDown);
    CHECK(list->OnEvent(ftxui::Event::Delete) && p.deleted == 1, "pick list: Delete");

    p.selected = 12;   // moved from outside
    CHECK(firstLine(list) == " 8", "pick list: the view follows a selection set from outside");
    p.count = 3;
    CHECK(firstLine(list) == " 0" && p.selected == 2, "pick list: a shorter list clamps it");
    p.count = 0;
    (void)firstLine(list);
    CHECK(!list->Focusable() && !list->OnEvent(ftxui::Event::ArrowDown) && p.selected == 0,
          "pick list: empty, it takes neither focus nor keys");
    CHECK(!list->OnEvent(click(2, 1)), "pick list: nor a click where its items were");
}

void pickListMouse() {
    Picked p;
    auto   base = uc_log::FTXUIGui::PickList(p.option(2));   // 20 items of 2 rows in 5 rows
    ftxui::Component list = base;
    (void)firstLine(list);
    CHECK(list->OnEvent(click(4, 2)), "pick list: a click on an item is taken");
    CHECK(p.selected == 1 && p.clicked == 1 && p.clickedLine == 0 && p.clickedColumn == 4,
          "pick list: row 2 of 2-row items is item 1's first, the column is passed on");
    CHECK(list->OnEvent(click(0, 3)) && p.selected == 1 && p.clickedLine == 1,
          "pick list: its second row too");
    CHECK(!list->OnEvent(click(2, Height + 2)) && p.clicked == 1,
          "pick list: not a click below it");
    CHECK(!list->OnEvent(hover(2, 4)) && base->Hovered() == 2 && p.selected == 1,
          "pick list: the pointer over an item marks it, the selection stays");
    CHECK(firstLine(list) == " 0", "pick list: nothing scrolled yet");
    CHECK(list->OnEvent(wheel(true)), "pick list: the wheel over it");
    CHECK(firstLine(list) == "  valu" && p.selected == 1,
          "pick list: the wheel moves the view by 3 rows, not the selection");
    CHECK(base->Hovered() == 2, "pick list: and another item is under the pointer now");
    CHECK(list->OnEvent(click(2, 0)) && p.selected == 1 && p.clicked == 1,
          "pick list: the row at the top is item 1's second after the wheel");
    CHECK(list->OnEvent(click(2, 1)) && p.selected == 2, "pick list: and the next row item 2");
    (void)list->OnEvent(hover(2, Height + 2));
    CHECK(base->Hovered() == -1, "pick list: the pointer gone, nothing is marked");

    Picked hovering;
    auto   option           = hovering.option(1);
    option.hoverSelects     = true;
    option.focusable        = false;
    ftxui::Component follow = uc_log::FTXUIGui::PickList(std::move(option));
    (void)firstLine(follow);
    CHECK(!follow->Focusable(), "pick list: one fed by another component's keys has no focus");
    (void)follow->OnEvent(hover(3, 3));
    CHECK(hovering.selected == 3 && firstLine(follow) == " 0",
          "pick list: hoverSelects picks under the pointer and leaves the view");
    CHECK(follow->OnEvent(click(3, 4)) && hovering.clicked == 4, "pick list: and still clicks");
}

}   // namespace

int main() {
    frameAloneDoesNotScroll();
    wheelScrolls();
    keysScroll();
    showFollowsARow();
    pickListKeys();
    pickListMouse();
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
