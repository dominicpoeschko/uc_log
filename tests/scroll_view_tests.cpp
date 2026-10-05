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

}   // namespace

int main() {
    frameAloneDoesNotScroll();
    wheelScrolls();
    keysScroll();
    showFollowsARow();
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
