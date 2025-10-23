#pragma once

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wredundant-decls"
    #pragma GCC diagnostic ignored "-Woverloaded-virtual"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
    #pragma GCC diagnostic ignored "-Wshadow"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
    #pragma clang diagnostic ignored "-Wreserved-macro-identifier"
    #pragma clang diagnostic ignored "-Wsuggest-override"
    #pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
    #pragma clang diagnostic ignored "-Wmissing-noreturn"
    #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma clang diagnostic ignored "-Wglobal-constructors"
    #pragma clang diagnostic ignored "-Wdocumentation"
    #pragma clang diagnostic ignored "-Wsuggest-destructor-override"
    #pragma clang diagnostic ignored "-Wshorten-64-to-32"
    #pragma clang diagnostic ignored "-Wswitch-default"
    #pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
    #pragma clang diagnostic ignored "-Wswitch-enum"
    #pragma clang diagnostic ignored "-Wimplicit-fallthrough"
    #pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif

#include <boost/asio.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

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
        explicit Session(boost::asio::ip::tcp::socket socket_,
                         ErrorMessageF&&              errorMessagef_)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
          , socket{std::move(socket_)} {}

        void send(std::span<std::byte const> data) {
            std::vector<std::byte> vec;
            vec.resize(data.size());
            std::ranges::copy(data, vec.begin());

            std::lock_guard<std::mutex> const lock{mutex};

            messages.push_back(std::move(vec));
            if(!sending) { doSend(); }
        }

        void run() { async_read_some(); }

        void async_read_some() {
            recvData.resize(1024);

            socket.async_read_some(
              boost::asio::buffer(recvData.data(), 1024),
              [self = shared_from_this()](boost::system::error_code error_code, std::size_t) {
                  if(!error_code) {
                      self->async_read_some();
                  } else if(error_code != boost::asio::error::eof) {
                      self->errorMessagef(
                        fmt::format("client recv error {}", error_code.message()));
                  }
              });
        }

        void write_rdy() {
            std::lock_guard<std::mutex> const lock{mutex};
            sending = false;
            if(!messages.empty()) { doSend(); }
        }

        void doSend() {
            sending = true;

            auto message = std::move(messages.front());
            messages.erase(messages.begin());

            boost::asio::async_write(
              socket,
              boost::asio::buffer(message.data(), message.size()),
              [self            = shared_from_this(),
               capturedMessage = message](boost::system::error_code error_code, std::size_t) {
                  if(!error_code) {
                      self->write_rdy();
                  } else if(error_code != boost::asio::error::eof) {
                      self->errorMessagef(
                        fmt::format("client send error {}", error_code.message()));
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
        std::lock_guard<std::mutex> const lock{mutex};
        for(auto& client : clients) {
            try {
                auto session = client.lock();
                if(session) { session->send(std::as_bytes(std::span{msg})); }
            } catch(std::exception const& e) { errorMessagef(fmt::format("caught: {}", e.what())); }
        }
        clean();
    }

private:
    void clean() {
        auto ret
          = std::ranges::remove_if(clients, [](auto& client) { return client.use_count() == 0; });
        clients.erase(ret.begin(), ret.end());
    }

    void runner(std::stop_token const& stoken) {
        while(!stoken.stop_requested()) { ioc.run_for(std::chrono::milliseconds{250}); }
    }

    void async_accept_one() {
        acceptor.async_accept(
          [this](boost::system::error_code error_code, boost::asio::ip::tcp::socket socket) {
              if(!error_code) {
                  auto session = std::make_shared<Session>(std::move(socket), errorMessagef);
                  clients.push_back(session);
                  session->run();
              } else {
                  errorMessagef(fmt::format("asio error {}", error_code.message()));
              }
              async_accept_one();
          });
    }
};
