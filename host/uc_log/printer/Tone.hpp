#pragma once

// How much a piece of text matters, without the window: plain, ok, info, warn, bad. The window maps each to a colour of
// its theme (rtx/Theme.hpp); the text never names a colour.

#include <cstdint>

namespace uc_log::printer {

enum class Tone : std::uint8_t { plain, ok, info, warn, bad };

}   // namespace uc_log::printer
