#pragma once

// A head (every tab's): its tools and its hint as they fit beside its title in `width` cells.

#include "uc_log/printer/rtx/Runs.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer::rtx {

/// The tools as they fit: too many, and they keep their icon and key alone (the one that is on its name too).
void fitTools(std::vector<Chip>& tools,
              int                width);

/// `hint`, if it fits beside `title` and `tools` - or none: cut in the middle it says nothing.
std::string fitHint(std::string_view         title,
                    std::string              hint,
                    std::vector<Chip> const& tools,
                    int                      width);

}   // namespace uc_log::printer::rtx
