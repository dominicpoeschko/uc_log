#pragma once

#include "toxic_spokes/IP/Socket.hpp"
#include "toxic_spokes/Serial/Serial.hpp"

#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct TCPSender {
    ts::TCP_ServerSocket              socket;
    std::vector<ts::TCP_ClientSocket> clients;
    std::mutex                        mutex;
    std::jthread                      thread{std::bind_front(&TCPSender::runner, this)};

    explicit TCPSender(std::uint16_t port) : socket{port} {}

    void send(std::string_view msg) {
        std::lock_guard<std::mutex> lock{mutex};
        for(auto& c : clients) {
            try {
                c.send(std::as_bytes(std::span{msg}));
            } catch(std::exception const& e) {
                fmt::print(stderr, "Exception: {}\n", e.what());
            }
        }
        clean();
    }

private:
    void clean() {
        clients.erase(
          std::remove_if(
            clients.begin(),
            clients.end(),
            [](auto& client) { return !client.is_valid(); }),
          clients.end());
    }
    void runner(std::stop_token stoken) {
        while(!stoken.stop_requested()) {
            if(socket.can_accept(std::chrono::milliseconds{500})) {
                auto                        client = socket.accept();
                std::lock_guard<std::mutex> lock{mutex};
                clients.push_back(std::move(client));
            }
        }
    }
};
