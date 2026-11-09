#include "uc_log/printer/rtx/Dialog.hpp"

#include <string>

namespace uc_log::printer::rtx {

void Fields(DialogRow const&    row,
            ::rtxui::FieldSink& out) {
    out("cls", row.cls);
    out("mark", row.mark);
    out("text", row.text);
    out("detail", row.detail);
}

Dialog::Dialog() {
    Import("open", &Dialog::open);
    Import("title", &Dialog::title);
    Import("about", &Dialog::about);
    Import("query", &Dialog::query);
    Import("placeholder", &Dialog::placeholder);
    Import("error", &Dialog::error);
    Import("count", &Dialog::count);
    Import("rows", &Dialog::rows);
    Import("hint", &Dialog::hint);
    Import("buttons", &Dialog::buttons);
    Import("size", &Dialog::size);
    Import("hasAbout", &Dialog::hasAbout);
    Import("hasError", &Dialog::hasError);
    Import("hasCount", &Dialog::hasCount);
    Import("Pick", [this](std::string const& arg) {
        if(picked) { picked(argIndex(arg)); }
    });
    Import("Press", [this](std::string const& arg) {
        if(pressed) { pressed(argIndex(arg)); }
    });
    Import("Close", [this] {
        if(closed) { closed(); }
    });
}

bool Dialog::Digest() {
    if(hasAbout == about.empty()) { hasAbout = !about.empty(); }
    if(hasError == error.empty()) { hasError = !error.empty(); }
    if(hasCount == count.empty()) { hasCount = !count.empty(); }
    return Component<Dialog>::Digest();
}

}   // namespace uc_log::printer::rtx
