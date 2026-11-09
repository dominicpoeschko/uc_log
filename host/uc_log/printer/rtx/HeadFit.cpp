#include "uc_log/printer/rtx/HeadFit.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void fitTools(std::vector<Chip>& tools,
              int                width) {
    std::size_t cells = 0;
    for(auto const& t : tools) {
        cells += cellWidth(t.text) + cellWidth(t.key) + (t.cls.contains("gap") ? 3 : 1);
    }
    if(static_cast<int>(cells) + 34 <= width) { return; }
    for(auto& t : tools) {
        if(auto const space = t.text.find(' ');
           space != std::string::npos && !t.cls.starts_with("on"))
        {
            t.text.resize(space);
        }
    }
}

std::string fitHint(std::string_view         title,
                    std::string              hint,
                    std::vector<Chip> const& tools,
                    int                      width) {
    std::size_t cells = cellWidth(title) + cellWidth(hint) + 8;   // the head's padding, some room
    for(auto const& t : tools) {
        cells += cellWidth(t.text) + cellWidth(t.key) + (t.cls.contains("gap") ? 3 : 1);
    }
    return static_cast<int>(cells) <= width ? std::move(hint) : std::string{};
}

}   // namespace uc_log::printer::rtx
