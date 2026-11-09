#pragma once

// This machine's own addresses (what the printer's sockets can bind besides loopback and every interface), read from
// its interfaces, and whether a text is an IP address at all.

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// An address of this machine: what a socket can bind besides loopback and every interface.
struct LocalAddress {
    std::string address;
    std::string interface;

    bool operator==(LocalAddress const&) const = default;
};

/// The interfaces' IPv4 and IPv6 addresses (getifaddrs), without loopback and link-local ones (those need a
/// scope); empty when the interfaces cannot be read.
inline std::vector<LocalAddress> localAddresses() {
    ifaddrs* list = nullptr;
    if(getifaddrs(&list) != 0) { return {}; }
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> const owner{list, &freeifaddrs};
    std::vector<LocalAddress>                              out;
    for(auto const* a = list; a != nullptr; a = a->ifa_next) {
        if(a->ifa_addr == nullptr || (a->ifa_flags & IFF_LOOPBACK) != 0) { continue; }
        std::array<char, INET6_ADDRSTRLEN> text{};
        bool                               named = false;
        if(a->ifa_addr->sa_family == AF_INET) {
            sockaddr_in in{};
            std::memcpy(&in, a->ifa_addr, sizeof in);
            named = inet_ntop(AF_INET, &in.sin_addr, text.data(), text.size()) != nullptr;
        } else if(a->ifa_addr->sa_family == AF_INET6) {
            sockaddr_in6 in6{};
            std::memcpy(&in6, a->ifa_addr, sizeof in6);
            named = !IN6_IS_ADDR_LINKLOCAL(&in6.sin6_addr)
                 && inet_ntop(AF_INET6, &in6.sin6_addr, text.data(), text.size()) != nullptr;
        }
        if(!named) { continue; }
        LocalAddress l{text.data(), a->ifa_name != nullptr ? a->ifa_name : ""};
        if(std::ranges::find(out, l) == out.end()) { out.push_back(std::move(l)); }
    }
    return out;
}

/// Whether `text` is an IPv4 or IPv6 address (what a bind takes; no host names).
inline bool isIpAddress(std::string const& text) {
    std::array<unsigned char, sizeof(in6_addr)> buf{};
    return inet_pton(AF_INET, text.c_str(), buf.data()) == 1
        || inet_pton(AF_INET6, text.c_str(), buf.data()) == 1;
}

}   // namespace uc_log::printer
