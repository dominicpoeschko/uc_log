#pragma once

#include <cstdint>

enum class TcpPortStatus : std::uint8_t { NotStarted, Active, PortOccupied };
enum class LogFileStatus : std::uint8_t { NotStarted, Active, Error };
