#pragma once

// Every line the control server writes: one line, strict JSON, round-trips as `T`. Aborts otherwise.

#include "uc_log/detail/ControlProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size);

namespace uc_log::fuzz {

[[noreturn]] inline void fail(char const*      what,
                              std::string_view line) {
    std::fprintf(stderr,
                 "PROPERTY VIOLATED: %s\n  line: %.*s\n",
                 what,
                 static_cast<int>(line.size() > 400 ? 400 : line.size()),
                 line.data());
    std::abort();
}

template<typename T>
void checkLine(std::string const& line) {
    if(line.empty() || line.back() != '\n') { fail("not ended by a newline", line); }
    if(line.find('\n') != line.size() - 1) { fail("more than one line", line); }
    for(std::size_t i = 0; i + 1 < line.size(); ++i) {
        if(static_cast<unsigned char>(line[i]) < 0x20U) { fail("a raw control character", line); }
    }
    auto utf8 = line;
    control::makeUtf8(utf8);
    if(utf8 != line) { fail("not UTF-8", line); }
    auto const  parsed = control::fromLine<T>(line);
    auto const* value  = std::get_if<T>(&parsed);
    if(value == nullptr) { fail("does not read back", line); }
    if(control::toLine(*value) != line) { fail("does not write out as itself", line); }
}

}   // namespace uc_log::fuzz
