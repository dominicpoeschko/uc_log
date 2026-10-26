#pragma once

// Where a printer's unix sockets live: <log dir>/<name>, or a short path under /tmp when that is too long for sun_path
// (uc_log_client.py computes the same path; doc/control_protocol/socket_paths.json pins the rule). No asio: the
// control fixture and the clients' tests use it without a server.

#include "remote_fmt/fmt_wrapper.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/un.h>
#include <unistd.h>

namespace uc_log::printer {

/// Where socket paths too long for sun_path go; only its owner may enter it.
inline std::filesystem::path unixSocketTmpDir() {
    return std::filesystem::path{"/tmp"} / fmt::format("uc_log_{}", ::getuid());
}

/// unixSocketTmpDir()/<FNV-1a>.<name> when too long for sun_path; uc_log_client.py must match.
inline std::filesystem::path unixSocketPath(std::filesystem::path const& logDir,
                                            std::string_view             name) {
    auto absolute = std::filesystem::absolute(logDir).lexically_normal();
    if(!absolute.has_filename() && absolute.has_parent_path()) {
        absolute = absolute.parent_path();   // "dir/" hashes like "dir", as in os.path.abspath
    }
    auto direct = absolute / name;   // not const: returned by move
    if(direct.native().size() < sizeof(sockaddr_un{}.sun_path)) { return direct; }
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for(char const c : absolute.native()) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return unixSocketTmpDir() / fmt::format("{:016x}.{}", hash, name);
}

/// Named after the ordinal: RTT buffer names are not always readable.
inline std::string duplexSocketName(std::size_t ordinal) {
    return fmt::format("duplex.{}.sock", ordinal);
}

}   // namespace uc_log::printer
