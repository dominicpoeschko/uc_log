#pragma once

#include "uc_log/detail/TcpPortStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace uc_log { namespace detail {

    struct DuplexChannelInfo {
        std::size_t   ordinal{};
        std::string   name;
        std::uint16_t port{};
        TcpPortStatus status{TcpPortStatus::NotStarted};
        bool          enabled{true};
        bool          connected{};
        bool          hostToTargetOnly{};
        std::uint64_t bytesToTarget{};
        std::uint64_t bytesFromTarget{};
        std::uint64_t
          bytesDropped{};   // host -> client data dropped because the client was too slow
    };

}}   // namespace uc_log::detail
