#pragma once

#include "uc_log/printer/SocketPortStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace uc_log { namespace printer {

    struct DuplexChannelInfo {
        std::size_t      ordinal{};
        std::string      name;
        std::string      socketPath;   // not empty: a unix socket, the port is not in use
        std::uint16_t    port{};
        SocketPortStatus status{SocketPortStatus::notStarted};
        bool             enabled{true};
        bool             connected{};
        bool             hostToTargetOnly{};
        std::uint64_t    bytesToTarget{};
        std::uint64_t    bytesFromTarget{};
        std::uint64_t
          bytesDropped{};        // host -> client data dropped because the client was too slow
        std::uint64_t binds{};   // successful binds of its listener: a rebind is done once it moves
    };

}}   // namespace uc_log::printer
