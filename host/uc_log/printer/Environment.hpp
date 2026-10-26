#pragma once

#include <cstdlib>

namespace uc_log::printer {

/// A variable of the process environment, nullptr when it is not set. The one getenv of the printer and its tools:
/// getenv races only a setenv/putenv, and none of these programs changes its environment.
inline char const* environment(char const* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe) // no setenv/putenv in any program of the printer (see above)
    return std::getenv(name);
}

}   // namespace uc_log::printer
