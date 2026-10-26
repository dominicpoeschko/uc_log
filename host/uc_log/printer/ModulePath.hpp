#pragma once

// A log line's module is a dotted path (uc_log's UC_LOG_SCOPE_MODULE, "i2c.bus"): what a module filter matches.

#include <string_view>

namespace uc_log::printer {

/// "i2c.bus" is under "i2c" and under itself, not under "i2" or "i2c.b".
inline bool moduleUnder(std::string_view module,
                        std::string_view prefix) {
    return module.starts_with(prefix)
        && (module.size() == prefix.size() || module[prefix.size()] == '.');
}

}   // namespace uc_log::printer
