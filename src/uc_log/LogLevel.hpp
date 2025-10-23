#pragma once
#include <cstdint>

namespace uc_log {
    enum class LogLevel : std::uint8_t {
        trace = 0,
        debug,
        info,
        warn,
        error,
        crit
    }; }
