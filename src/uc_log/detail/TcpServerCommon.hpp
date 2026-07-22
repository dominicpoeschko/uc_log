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

#include "uc_log/detail/TcpPortStatus.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace uc_log { namespace detail {

    // io_context shared by all tcp servers. Split from AsioContextRunner so users of the
    // context can be declared between the two: the runner is joined before the users are
    // destroyed (no handler runs during their destruction), while the context itself
    // outlives the users (asio objects must not outlive their execution context).
    struct AsioContext {
        boost::asio::io_context                                                  ioc;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard{
          ioc.get_executor()};
    };

    struct AsioContextRunner {
        boost::asio::io_context& ioc;
        std::jthread             thread;

        explicit AsioContextRunner(AsioContext& context)
          : ioc{context.ioc}
          , thread{[this](std::stop_token const& stoken) {
              std::stop_callback const onStop{stoken, [this]() { ioc.stop(); }};
              ioc.run();
          }} {}
    };

    // one accepted connection. Socket and send queue are owned by the io_context thread;
    // other threads enter only through trySend()/resumeRead()/close(), which post onto the
    // socket's executor. The bounded send queue makes a stalled client drop data instead of
    // growing host memory without bound.
    struct TcpSession : std::enable_shared_from_this<TcpSession> {
        // onData: called on the io_context thread, return false to pause reading
        // (resumeRead() re-arms). onGone: called once on the io_context thread when the
        // connection failed or the peer closed it; not called for owner-initiated close().
        using DataF
          = std::function<bool(std::span<std::byte const>, std::shared_ptr<TcpSession> const&)>;
        using GoneF = std::function<void(std::shared_ptr<TcpSession> const&)>;

        boost::asio::ip::tcp::socket          socket;
        std::size_t                           sendQueueCap;
        std::function<void(std::string_view)> errorf;
        DataF                                 onData;
        GoneF                                 onGone;

        std::deque<std::shared_ptr<std::vector<std::byte>>> sendQueue;        // ioc thread only
        bool                                                sending{false};   // ioc thread only
        bool                                                gone{false};      // ioc thread only
        std::atomic<std::size_t>                            queuedBytes{0};
        std::vector<std::byte>                              recvData;

        template<typename ErrorF>
        TcpSession(boost::asio::ip::tcp::socket socket_,
                   std::size_t                  sendQueueCap_,
                   ErrorF&&                     errorf_,
                   DataF                        onData_,
                   GoneF                        onGone_)
          : socket{std::move(socket_)}
          , sendQueueCap{sendQueueCap_}
          , errorf{std::forward<ErrorF>(errorf_)}
          , onData{std::move(onData_)}
          , onGone{std::move(onGone_)} {}

        // any thread. Returns false when the queue is full and the data was dropped.
        bool trySend(std::span<std::byte const> data) {
            if(data.empty()) { return true; }
            if(queuedBytes.load(std::memory_order_relaxed) + data.size() > sendQueueCap) {
                return false;
            }
            queuedBytes.fetch_add(data.size(), std::memory_order_relaxed);
            auto buffer = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
            boost::asio::post(socket.get_executor(),
                              [self = shared_from_this(), buf = std::move(buffer)]() {
                                  if(self->gone) {
                                      self->queuedBytes.fetch_sub(buf->size(),
                                                                  std::memory_order_relaxed);
                                      return;
                                  }
                                  self->sendQueue.push_back(std::move(buf));
                                  if(!self->sending) { self->doSend(); }
                              });
            return true;
        }

        // ioc thread
        void startRead() {
            recvData.resize(1024);
            socket.async_read_some(
              boost::asio::buffer(recvData.data(), recvData.size()),
              [self = shared_from_this()](boost::system::error_code error_code,
                                          std::size_t               bytesRead) {
                  if(!error_code) {
                      if(!self->onData
                         || self->onData(std::span{self->recvData}.first(bytesRead), self))
                      {
                          self->startRead();
                      }
                  } else if(error_code != boost::asio::error::operation_aborted) {
                      if(error_code != boost::asio::error::eof && self->errorf) {
                          self->errorf(fmt::format("recv error {}", error_code.message()));
                      }
                      self->handleGone();
                  }
              });
        }

        // any thread
        void resumeRead() {
            boost::asio::post(socket.get_executor(), [self = shared_from_this()]() {
                if(!self->gone) { self->startRead(); }
            });
        }

        // any thread, owner-initiated: closes without invoking onGone
        void close() {
            boost::asio::post(socket.get_executor(), [self = shared_from_this()]() {
                if(self->gone) { return; }
                self->gone = true;
                self->sendQueue.clear();
                boost::system::error_code ec;
                self->socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                self->socket.close(ec);
            });
        }

    private:
        // ioc thread
        void doSend() {
            sending      = true;
            auto message = std::move(sendQueue.front());
            sendQueue.pop_front();

            // the handler owns the storage the asio buffer points into: it must stay alive
            // until the async write completes
            boost::asio::async_write(
              socket,
              boost::asio::buffer(message->data(), message->size()),
              [self = shared_from_this(), message](boost::system::error_code error_code,
                                                   std::size_t) {
                  self->queuedBytes.fetch_sub(message->size(), std::memory_order_relaxed);
                  if(!error_code) {
                      self->sending = false;
                      if(!self->sendQueue.empty() && !self->gone) { self->doSend(); }
                  } else if(error_code != boost::asio::error::operation_aborted) {
                      if(error_code != boost::asio::error::eof && self->errorf) {
                          self->errorf(fmt::format("send error {}", error_code.message()));
                      }
                      self->handleGone();
                  }
              });
        }

        // ioc thread
        void handleGone() {
            if(gone) { return; }
            gone = true;
            sendQueue.clear();
            boost::system::error_code ec;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket.close(ec);
            if(onGone) { onGone(shared_from_this()); }
        }
    };

    // bind/accept/rebind state machine. All acceptor state lives on the io_context thread;
    // start/restart/stop may be called from any thread. Accepted sockets get keepalive so
    // half-open peers are eventually detected even on quiet connections.
    struct TcpListener {
        boost::asio::io_context&                          ioc;
        boost::asio::ip::address                          bindAddress;
        std::function<void(boost::asio::ip::tcp::socket)> onAccept;   // ioc thread
        std::function<void(std::string_view)>             errorf;
        std::function<void()>                             statusChangef;

        std::optional<boost::asio::ip::tcp::acceptor> acceptor;             // ioc thread only
        std::optional<std::uint16_t>                  pendingRestartPort;   // ioc thread only
        std::atomic<TcpPortStatus>                    status{TcpPortStatus::NotStarted};
        std::atomic<std::uint16_t>                    currentPort{0};

        template<typename AcceptF,
                 typename ErrorF,
                 typename StatusChangeF>
        TcpListener(boost::asio::io_context& ioc_,
                    boost::asio::ip::address bindAddress_,
                    AcceptF&&                onAccept_,
                    ErrorF&&                 errorf_,
                    StatusChangeF&&          statusChangef_)
          : ioc{ioc_}
          , bindAddress{std::move(bindAddress_)}
          , onAccept{std::forward<AcceptF>(onAccept_)}
          , errorf{std::forward<ErrorF>(errorf_)}
          , statusChangef{std::forward<StatusChangeF>(statusChangef_)} {}

        void start(std::uint16_t port) {
            currentPort = port;
            boost::asio::post(ioc, [this, port]() { tryBind(port); });
        }

        void restart(std::uint16_t newPort) {
            currentPort = newPort;
            boost::asio::post(ioc, [this, newPort]() {
                if(acceptor.has_value()) {
                    pendingRestartPort = newPort;
                    boost::system::error_code ec;
                    acceptor->cancel(ec);
                } else {
                    tryBind(newPort);
                }
            });
        }

        // Change the address future binds use and re-apply immediately if currently
        // listening. A closed acceptor (stopped/disabled) just remembers the address for
        // the next start()/restart(), so this never opens a listener that was meant to be
        // down.
        void setBindAddress(boost::asio::ip::address newAddress) {
            boost::asio::post(ioc, [this, address = std::move(newAddress)]() {
                bindAddress = address;
                if(acceptor.has_value()) {
                    pendingRestartPort = currentPort.load();
                    boost::system::error_code ec;
                    acceptor->cancel(ec);
                }
            });
        }

        // preStopf runs on the io_context thread before the acceptor is torn down
        void stop(std::function<void()> preStopf = {}) {
            boost::asio::post(ioc, [this, preStop = std::move(preStopf)]() {
                if(preStop) { preStop(); }
                if(acceptor.has_value()) {
                    pendingRestartPort = std::nullopt;
                    boost::system::error_code ec;
                    acceptor->cancel(ec);
                } else {
                    status = TcpPortStatus::NotStarted;
                    if(statusChangef) { statusChangef(); }
                }
            });
        }

    private:
        void tryBind(std::uint16_t port) {
            try {
                acceptor.emplace(ioc, boost::asio::ip::tcp::endpoint{bindAddress, port});
                status      = TcpPortStatus::Active;
                currentPort = port;
                if(statusChangef) { statusChangef(); }
                asyncAcceptOne();
            } catch(boost::system::system_error const& e) {
                errorf(fmt::format("TCP port {} in use: {}", port, e.what()));
                status      = TcpPortStatus::PortOccupied;
                currentPort = port;
                if(statusChangef) { statusChangef(); }
            }
        }

        void asyncAcceptOne() {
            if(!acceptor.has_value()) { return; }
            acceptor->async_accept(
              [this](boost::system::error_code error_code, boost::asio::ip::tcp::socket socket) {
                  if(!error_code) {
                      boost::system::error_code ec;
                      socket.set_option(boost::asio::socket_base::keep_alive{true}, ec);
                      onAccept(std::move(socket));
                      asyncAcceptOne();
                  } else if(error_code == boost::asio::error::operation_aborted) {
                      boost::system::error_code ec;
                      if(acceptor.has_value()) { acceptor->close(ec); }
                      acceptor.reset();
                      if(pendingRestartPort.has_value()) {
                          auto const port = *pendingRestartPort;
                          pendingRestartPort.reset();
                          tryBind(port);
                      } else {
                          status = TcpPortStatus::NotStarted;
                          if(statusChangef) { statusChangef(); }
                      }
                  } else {
                      errorf(fmt::format("asio error {}", error_code.message()));
                      asyncAcceptOne();
                  }
              });
        }
    };

}}   // namespace uc_log::detail
