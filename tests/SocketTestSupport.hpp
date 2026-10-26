#pragma once
// What tcp_server_tests and control_server_tests share: a line client (the checks are support/Check.hpp's).
//
// Under a VirtualTime (control_server_tests) the client never blocks in a socket call: the server's threads move the
// virtual time only while every participant waits on the clock, so a test thread stuck in read() would hold the time
// and wait for a server that waits for the time. It polls and sleeps on the clock instead, and a deadline is virtual
// time (the same on every machine). Without one (tcp_server_tests) it blocks with no deadline at all: the ctest
// TIMEOUT is the hang guard, no check depends on how fast an answer comes.
#include "support/Check.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/ControlProtocol.hpp"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

using uc_log::printer::Clock;

inline std::atomic<bool> quiet{false};

inline void note(char const* msg) {
    if(!quiet) { std::printf("-- %s\n", msg); }
}

// One read_some: blocking without a VirtualTime; under one, polled on the clock until `deadline` (then timed_out).
template<typename Socket>
std::size_t readSome(Socket&                     socket,
                     boost::asio::mutable_buffer buffer,
                     boost::system::error_code&  ec,
                     Clock::time_point           deadline = Clock::time_point::max()) {
    if(!Clock::isVirtual()) { return socket.read_some(buffer, ec); }
    socket.non_blocking(true);
    while(true) {
        auto const n = socket.read_some(buffer, ec);
        if(ec != boost::asio::error::would_block) { return n; }
        if(Clock::now() >= deadline) {
            ec = boost::asio::error::timed_out;
            return 0;
        }
        Clock::sleep_for(std::chrono::milliseconds{1});
    }
}

template<typename Socket>
struct LineClient {
    Socket      socket;
    std::string pending;   // read, not yet handed out as a line
    bool        eof{};     // the server closed

    explicit LineClient(boost::asio::io_context& io) : socket{io} {}

    void send(std::string_view line) { boost::asio::write(socket, boost::asio::buffer(line)); }

    // The next line; empty at the end of the stream. `timeout` counts only under a VirtualTime.
    std::string next(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        auto const deadline
          = Clock::isVirtual() ? Clock::now() + timeout : Clock::time_point::max();
        while(true) {
            if(auto const nl = pending.find('\n'); nl != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                return line;
            }
            std::array<char, 65536>   chunk;
            boost::system::error_code ec;
            auto const n = readSome(socket, boost::asio::buffer(chunk), ec, deadline);
            pending.append(chunk.data(), n);
            if(ec == boost::asio::error::timed_out) {
                std::printf("FAIL: no line within %lld ms (virtual time)\n",
                            static_cast<long long>(timeout.count()));
                ++uc_log::test::failures;
                return {};
            }
            if(ec) {
                eof = ec == boost::asio::error::eof;
                return {};
            }
        }
    }

    std::string ask(std::string_view line) {
        send(line);
        return next();
    }
};

inline uc_log::printer::control::Answer answerOf(std::string const& line) {
    auto const parsed = uc_log::printer::control::fromLine<uc_log::printer::control::Answer>(line);
    if(auto const* a = std::get_if<uc_log::printer::control::Answer>(&parsed)) { return *a; }
    return uc_log::printer::control::Error{.error = "not an answer: " + line};
}

// Under a VirtualTime: up to 5 s of virtual time. Without one: until it holds (the ctest TIMEOUT is the hang guard).
inline bool waitUntil(auto const& condition) {
    auto const deadline
      = Clock::isVirtual() ? Clock::now() + std::chrono::seconds{5} : Clock::time_point::max();
    while(!condition()) {
        if(Clock::now() >= deadline) { return condition(); }
        Clock::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}
