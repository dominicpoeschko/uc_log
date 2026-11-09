#include "uc_log/printer/rtx/Panel.hpp"

#include <string>

namespace uc_log::printer::rtx {

Panel::Panel() {
    Import("title", &Panel::title);
    Import("hint", &Panel::hint);
    Import("legend", &Panel::legend);
    Import("ring", &Panel::ring);
    Import("scrollbar", &Panel::scrollbar);
    Import("height", &Panel::height);
    Import("spans", &Panel::spans);
    Import("buttons", &Panel::buttons);
    Import("Press", [this](std::string const& arg) {
        if(pressed) { pressed(argIndex(arg)); }
    });
}

}   // namespace uc_log::printer::rtx
