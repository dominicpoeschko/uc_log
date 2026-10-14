// Any bytes over a real socket, in chunks the input chooses: what comes back is valid JSON lines,
// and afterwards a fresh connection's ping is still answered.
//
// On the server's virtual clock (uc_log/detail/Clock.hpp): its threads and this one move the time, so "nothing more
// came for 20 ms" means the server had nothing left to do, and an unanswered ping a server that is stuck - not a
// machine that was slow. The same input does the same on every run.

#include "../ControlProtocolFixture.hpp"
#include "FuzzChecks.hpp"
#include "uc_log/detail/Clock.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
namespace ctl = uc_log::control;
namespace fx  = uc_log::control_fixture;

using uc_log::detail::Clock;

struct Fixture {
    uc_log::detail::VirtualTime   time;   // first: the server's threads take part in it
    std::filesystem::path         path;
    fx::FakeTarget                fake;
    uc_log::detail::ControlServer server;

    Fixture()
      : path{std::filesystem::temp_directory_path()
             / ("uc_log_fuzz_" + std::to_string(::getpid()) + ".sock")}
      , server{boost::asio::ip::make_address("127.0.0.1"),
               0,
               path} {
        server.setTarget(fake.target());
        for(int i = 0; i != 500 && server.getStatus() != TcpPortStatus::Active; ++i) {
            Clock::sleep_for(std::chrono::milliseconds{1});
        }
    }
};

Fixture& fixture() {
    static auto* f = new Fixture;   // one server for the whole run; leaked on purpose
    return *f;
}

int connectTo(std::filesystem::path const& path) {
    int const   fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    path.native().copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    if(::connect(fd, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// what arrives until `ms` of the clock pass without data (or the first line, or the end); never blocks in recv: the
// server moves the time only while this thread waits on the clock too
std::string drain(int  fd,
                  int  ms,
                  bool untilLine = false) {
    std::string got;
    char        buffer[65536];
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    auto quietSince = Clock::now();
    while(Clock::now() - quietSince < std::chrono::milliseconds{ms}) {
        auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
        if(n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { break; }
        if(n < 0) {
            Clock::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        got.append(buffer, static_cast<std::size_t>(n));
        if(untilLine && got.find('\n') != std::string::npos) { break; }
        quietSince = Clock::now();
    }
    return got;
}
}   // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
    auto& f = fixture();
    if(size == 0) { return 0; }
    // first byte: chunk size (low 6 bits), half-close and read back (0x40)
    std::string_view const all{reinterpret_cast<char const*>(data), size};
    auto const             mode     = static_cast<unsigned char>(all.front());
    std::size_t const      chunk    = 1 + (mode & 0x3FU) * 17;
    bool const             readBack = (mode & 0x40U) != 0;
    std::string_view const rest     = all.substr(1);

    int const fd = connectTo(f.path);
    if(fd < 0) { uc_log::fuzz::fail("cannot connect", ""); }
    for(std::size_t at = 0; at < rest.size(); at += chunk) {
        auto const piece = rest.substr(at, chunk);
        if(::send(fd, piece.data(), piece.size(), MSG_NOSIGNAL) < 0) { break; }
    }
    if(readBack) {
        ::shutdown(fd, SHUT_WR);
        auto const  got   = drain(fd, 20);
        std::size_t start = 0;
        while(true) {
            auto const end = got.find('\n', start);
            if(end == std::string::npos) { break; }
            auto const line     = got.substr(start, end - start + 1);
            auto const asAnswer = ctl::fromLine<ctl::Answer>(line);
            if(std::holds_alternative<ctl::Answer>(asAnswer)) {
                uc_log::fuzz::checkLine<ctl::Answer>(line);
            } else {
                uc_log::fuzz::checkLine<ctl::Event>(line);
            }
            start = end + 1;
        }
    }
    ::close(fd);

    // 5 x 2 s of the server's clock: only a stuck server fails
    std::string const ping = "{\"cmd\":\"ping\"}\n";
    std::string       answer;
    for(int attempt = 0; attempt != 5 && answer.empty(); ++attempt) {
        int const probe = connectTo(f.path);
        if(probe < 0) { uc_log::fuzz::fail("cannot connect after the input", ""); }
        ::send(probe, ping.data(), ping.size(), MSG_NOSIGNAL);
        answer = drain(probe, 2000, true);
        ::close(probe);
    }
    if(answer != uc_log::control::toLine(uc_log::control::Answer{uc_log::control::PingAnswer{}})) {
        std::fprintf(stderr,
                     "server holds %zu client(s), %zu request(s) in flight\n",
                     f.server.getClientCount(),
                     f.server.requestsInFlight());
        uc_log::fuzz::fail("no ping answer within 10 s after the input", answer);
    }
    return 0;
}
