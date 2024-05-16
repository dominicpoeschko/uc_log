#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct TCPSender {
    struct Session : std::enable_shared_from_this<Session> {
        std::function<void(std::string_view)> errorMessagef;
        boost::asio::ip::tcp::socket          socket;
        std::mutex                            mutex;
        bool                                  sending = false;
        std::vector<std::vector<std::byte>>   messages;
        std::vector<std::byte>                recvData;

        template<typename ErrorMessageF>
        explicit Session(boost::asio::ip::tcp::socket socket_, ErrorMessageF&& errorMessagef_)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
          , socket{std::move(socket_)} {}

        void send(std::span<std::byte const> data) {
            std::vector<std::byte> vec;
            vec.resize(data.size());
            std::copy(data.begin(), data.end(), vec.begin());

            std::lock_guard<std::mutex> lock{mutex};

            messages.push_back(std::move(vec));
            if(!sending) {
                doSend();
            }
        }

        void run() { async_read_some(); }

        void async_read_some() {
            recvData.resize(1024);

            socket.async_read_some(
              boost::asio::buffer(recvData.data(), 1024),
              [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                  if(!ec) {
                      self->async_read_some();
                  } else if(ec != boost::asio::error::eof) {
                      self->errorMessagef(fmt::format("client recv error {}", ec.message()));
                  }
              });
        }

        void write_rdy() {
            std::lock_guard<std::mutex> lock{mutex};
            sending = false;
            if(!messages.empty()) {
                doSend();
            }
        }

        void doSend() {
            sending = true;

            boost::asio::async_write(
              socket,
              boost::asio::buffer(messages.front(), messages.front().size()),
              [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                  if(!ec) {
                      self->messages.erase(self->messages.begin());
                      self->write_rdy();
                  } else if(ec != boost::asio::error::eof) {
                      self->errorMessagef(fmt::format("client send error {}", ec.message()));
                  }
              });
        }
    };

    std::function<void(std::string_view)> errorMessagef;
    boost::asio::io_context               ioc;
    boost::asio::ip::tcp::acceptor        acceptor;
    std::vector<std::weak_ptr<Session>>   clients;
    std::mutex                            mutex;
    std::jthread                          thread{std::bind_front(&TCPSender::runner, this)};

    template<typename ErrorMessageF>
    explicit TCPSender(std::uint16_t port, ErrorMessageF && errorMessagef_)
      : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}, acceptor{
        ioc,
        {boost::asio::ip::tcp::v4(), port}
    } {
        async_accept_one();
    }

    void send(std::string_view msg) {
        std::lock_guard<std::mutex> lock{mutex};
        for(auto& c : clients) {
            try {
                auto sp = c.lock();
                if(sp) {
                    sp->send(std::as_bytes(std::span{msg}));
                }
            } catch(std::exception const& e) {
                errorMessagef(fmt::format("catched: {}", e.what()));
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
            [](auto& client) { return client.use_count() == 0; }),
          clients.end());
    }

    void runner(std::stop_token stoken) {
        while(!stoken.stop_requested()) {
            ioc.run_for(std::chrono::milliseconds{250});
        }
    }

    void async_accept_one() {
        acceptor.async_accept(
          [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
              if(!ec) {
                  auto sp = std::make_shared<Session>(std::move(socket), errorMessagef);
                  clients.push_back(sp);
                  sp->run();
              } else {
                  errorMessagef(fmt::format("asio error {}", ec.message()));
              }
              async_accept_one();
          });
    }
};
