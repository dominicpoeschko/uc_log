#pragma once

#include <cstdint>

namespace uc_log::printer {

enum class SocketPortStatus : std::uint8_t { notStarted, active, portOccupied };
enum class LogFileStatus : std::uint8_t { notStarted, active, error };

}   // namespace uc_log::printer
