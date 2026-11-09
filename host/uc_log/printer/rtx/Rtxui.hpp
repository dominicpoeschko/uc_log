#pragma once
// RTXUI's headers for the printer's RTXUI window, and only for it (the layering test poisons them for every other
// printer header). Bind(x) and BindCollection(...) are function-like macros in RTXUI: undefined here, so that no later
// header with a Bind(...) call of its own is rewritten; this window calls Import(name, &Class::member).
#pragma GCC system_header

#include <rtxui/rtxui.hpp>

#undef Bind
#undef BindCollection

#include <string_view>

// RTXUI's measure of text, the one its layout and paint use: declared in its src/rtxui/base/string.hpp, which is not
// installed, and defined in the library
auto string_width(std::string_view input) -> int;
