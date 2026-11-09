#include "uc_log/printer/rtx/ModeLine.hpp"

#include <string>

namespace uc_log::printer::rtx {

ModeLine::ModeLine() {
    Import("position", &ModeLine::position);
    Import("notice", &ModeLine::notice);
    Import("hints", &ModeLine::hints);
    Import("Hint", [this](std::string const& arg) {
        if(hinted) { hinted(argIndex(arg)); }
    });
}

}   // namespace uc_log::printer::rtx
