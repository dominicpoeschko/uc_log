#pragma once

#include "uc_log/detail/TcpPortStatus.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

// multi-client broadcast sender for the metrics stream. Slow clients drop data instead of
// growing host memory without bound.
struct TCPSender {
    static constexpr std::size_t SendQueueCap = 4 * 1024 * 1024;

    std::function<void(std::string_view)>                    errorMessagef;
    std::function<void(TcpPortStatus, std::uint16_t)>        statusChangef;
    mutable std::mutex                                       mutex;
    std::vector<std::shared_ptr<uc_log::detail::TcpSession>> clients;   // guarded by mutex
    std::atomic<std::uint64_t>                               bytesDropped{0};
    uc_log::detail::TcpListener                              listener;

    template<typename ErrorMessageF,
             typename StatusChangeF>
    explicit TCPSender(boost::asio::io_context& ioc,
                       boost::asio::ip::address bindAddress,
                       std::uint16_t            port,
                       ErrorMessageF&&          errorMessagef_,
                       StatusChangeF&&          statusChangef_)
      : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
      , statusChangef{std::forward<StatusChangeF>(statusChangef_)}
      , listener{ioc,
                 std::move(bindAddress),
                 [this](boost::asio::ip::tcp::socket socket) { onAccept(std::move(socket)); },
                 [this](std::string_view msg) { errorMessagef(msg); },
                 [this]() {
                     if(statusChangef) {
                         statusChangef(listener.status.load(), listener.currentPort.load());
                     }
                 }} {
        listener.start(port);
    }

    void send(std::string_view msg) {
        std::lock_guard<std::mutex> const lock{mutex};
        for(auto const& client : clients) {
            if(!client->trySend(std::as_bytes(std::span{msg}))) { bytesDropped += msg.size(); }
        }
    }

    void restart(std::uint16_t newPort) { listener.restart(newPort); }

    void setBindAddress(boost::asio::ip::address address) {
        listener.setBindAddress(std::move(address));
    }

    void stop() {
        listener.stop([this]() { closeAllSessions(); });
    }

    TcpPortStatus getStatus() const { return listener.status.load(); }

    std::uint16_t getPort() const { return listener.currentPort.load(); }

    std::size_t getClientCount() const {
        std::lock_guard<std::mutex> const lock{mutex};
        return clients.size();
    }

private:
    // ioc thread
    void onAccept(boost::asio::ip::tcp::socket socket) {
        auto session = std::make_shared<uc_log::detail::TcpSession>(
          std::move(socket),
          SendQueueCap,
          [this](std::string_view msg) { errorMessagef(fmt::format("client {}", msg)); },
          nullptr,   // received data is drained and discarded
          [this](std::shared_ptr<uc_log::detail::TcpSession> const& session_) {
              std::lock_guard<std::mutex> const lock{mutex};
              std::erase(clients, session_);
          });
        {
            std::lock_guard<std::mutex> const lock{mutex};
            clients.push_back(session);
        }
        session->startRead();
    }

    // ioc thread
    void closeAllSessions() {
        std::vector<std::shared_ptr<uc_log::detail::TcpSession>> toClose;
        {
            std::lock_guard<std::mutex> const lock{mutex};
            toClose = std::move(clients);
            clients.clear();
        }
        for(auto const& session : toClose) { session->close(); }
    }
};
