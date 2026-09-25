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

#include "remote_fmt/fmt_wrapper.hpp"
#include "uc_log/detail/TcpPortStatus.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace uc_log { namespace detail {

    using StreamProtocol = boost::asio::generic::stream_protocol;
    using StreamSocket   = StreamProtocol::socket;

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
        auto const direct = absolute / name;
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

    /// Removes a stale socket file; refuses a live one or anything that is not a socket.
    inline std::expected<void,
                         std::string>
    claimUnixSocketPath(std::filesystem::path const& path) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if(path.parent_path() == unixSocketTmpDir()) {
            // shared /tmp: another user may have made the directory first, to take the socket over
            struct stat dir{};
            if(::lstat(path.parent_path().c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode)
               || dir.st_uid != ::getuid())
            {
                return std::unexpected{
                  fmt::format("{} is not a directory of this user", path.parent_path().native())};
            }
            if((dir.st_mode & 077U) != 0 && ::chmod(path.parent_path().c_str(), 0700) != 0) {
                return std::unexpected{
                  fmt::format("{} cannot be made private", path.parent_path().native())};
            }
        }
        auto const st = std::filesystem::symlink_status(path, ec);
        if(!std::filesystem::exists(st)) { return {}; }
        if(!std::filesystem::is_socket(st)) {
            return std::unexpected{std::string{"the path exists and is not a socket"}};
        }
        boost::asio::io_context                     io;
        boost::asio::local::stream_protocol::socket probe{io};
        boost::system::error_code                   connectError;
        probe.connect(boost::asio::local::stream_protocol::endpoint{path.native()}, connectError);
        if(!connectError) {
            return std::unexpected{std::string{"another printer already serves it"}};
        }
        std::filesystem::remove(path, ec);
        return {};
    }

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

    // One accepted connection (TCP or unix socket). Socket and send queue are owned by the
    // io_context thread; other threads enter only through trySend()/resumeRead()/close()/
    // closeWhenSent(), which post onto the socket's executor. The bounded send queue makes a
    // stalled client drop data instead of growing host memory without bound.
    struct TcpSession : std::enable_shared_from_this<TcpSession> {
        // onData: called on the io_context thread, return false to pause reading
        // (resumeRead() re-arms). onGone: called once on the io_context thread when the
        // connection failed, or on EOF unless onEof is set; not for owner-initiated close().
        using DataF
          = std::function<bool(std::span<std::byte const>, std::shared_ptr<TcpSession> const&)>;
        using GoneF = std::function<void(std::shared_ptr<TcpSession> const&)>;

        StreamSocket                          socket;
        std::size_t                           sendQueueCap;
        std::function<void(std::string_view)> errorf;
        DataF                                 onData;
        GoneF                                 onGone;

        std::deque<std::shared_ptr<std::vector<std::byte>>> sendQueue;        // ioc thread only
        bool                                                sending{false};   // ioc thread only
        bool                                                gone{false};      // ioc thread only
        bool closeWhenDone{false};   // ioc thread only: close once the send queue is empty
        // Set before startRead(): EOF is a half-close, the peer may still read.
        GoneF                    onEof;
        std::atomic<std::size_t> queuedBytes{0};
        std::vector<std::byte>   recvData;

        template<typename ErrorF>
        TcpSession(StreamSocket socket_,
                   std::size_t  sendQueueCap_,
                   ErrorF&&     errorf_,
                   DataF        onData_,
                   GoneF        onGone_)
          : socket{std::move(socket_)}
          , sendQueueCap{sendQueueCap_}
          , errorf{std::forward<ErrorF>(errorf_)}
          , onData{std::move(onData_)}
          , onGone{std::move(onGone_)} {}

        // not an error worth reporting
        static bool peerLeft(boost::system::error_code const& ec) {
            return ec == boost::asio::error::eof || ec == boost::asio::error::broken_pipe
                || ec == boost::asio::error::connection_reset;
        }

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
                  } else if(error_code == boost::asio::error::eof && self->onEof) {
                      self->onEof(self);
                  } else if(error_code != boost::asio::error::operation_aborted) {
                      if(!peerLeft(error_code) && self->errorf) {
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

        // any thread: close (and call onGone) once the send queue is empty
        void closeWhenSent() {
            boost::asio::post(socket.get_executor(), [self = shared_from_this()]() {
                if(self->gone) { return; }
                self->closeWhenDone = true;
                if(!self->sending && self->sendQueue.empty()) { self->handleGone(); }
            });
        }

        // any thread, owner-initiated: closes without invoking onGone
        void close() {
            boost::asio::post(socket.get_executor(), [self = shared_from_this()]() {
                if(self->gone) { return; }
                self->gone = true;
                self->sendQueue.clear();
                boost::system::error_code ec;
                self->socket.shutdown(boost::asio::socket_base::shutdown_both, ec);
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
                      if(!self->sendQueue.empty() && !self->gone) {
                          self->doSend();
                      } else if(self->closeWhenDone) {
                          self->handleGone();
                      }
                  } else if(error_code != boost::asio::error::operation_aborted) {
                      if(!peerLeft(error_code) && self->errorf) {
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
            socket.shutdown(boost::asio::socket_base::shutdown_both, ec);
            socket.close(ec);
            if(onGone) { onGone(shared_from_this()); }
        }
    };

    // bind/accept/rebind state machine. All acceptor state lives on the io_context thread;
    // start/restart/stop may be called from any thread. Accepted sockets get keepalive so
    // half-open peers are eventually detected even on quiet connections.
    // With a socket path it listens there instead (owner-only, removed on close).
    struct TcpListener {
        using Acceptor = boost::asio::basic_socket_acceptor<StreamProtocol>;

        boost::asio::io_context&              ioc;
        boost::asio::ip::address              bindAddress;
        std::function<void(StreamSocket)>     onAccept;   // ioc thread
        std::function<void(std::string_view)> errorf;
        std::function<void()>                 statusChangef;
        mutable std::mutex                    pathMutex;
        std::filesystem::path                 socketPath;   // empty: TCP; guarded by pathMutex
        std::filesystem::path                 boundPath;    // ioc thread (and the destructor)

        std::optional<Acceptor>      acceptor;             // ioc thread only
        std::optional<std::uint16_t> pendingRestartPort;   // ioc thread only
        std::atomic<TcpPortStatus>   status{TcpPortStatus::NotStarted};
        std::atomic<std::uint16_t>   currentPort{0};
        std::atomic<bool>            ownsSocketFile{false};

        template<typename AcceptF,
                 typename ErrorF,
                 typename StatusChangeF>
        TcpListener(boost::asio::io_context& ioc_,
                    boost::asio::ip::address bindAddress_,
                    AcceptF&&                onAccept_,
                    ErrorF&&                 errorf_,
                    StatusChangeF&&          statusChangef_,
                    std::filesystem::path    socketPath_ = {})
          : ioc{ioc_}
          , bindAddress{std::move(bindAddress_)}
          , onAccept{std::forward<AcceptF>(onAccept_)}
          , errorf{std::forward<ErrorF>(errorf_)}
          , statusChangef{std::forward<StatusChangeF>(statusChangef_)}
          , socketPath{std::move(socketPath_)} {}

        // the owner has stopped the io_context thread by now
        ~TcpListener() { removeSocketFile(); }

        TcpListener(TcpListener const&)            = delete;
        TcpListener& operator=(TcpListener const&) = delete;

        /// Empty while on TCP.
        std::filesystem::path getSocketPath() const {
            std::lock_guard<std::mutex> const lock{pathMutex};
            return socketPath;
        }

        // Empty: TCP. Re-applied at once if listening.
        void setSocketPath(std::filesystem::path newPath) {
            boost::asio::post(ioc, [this, path = std::move(newPath)]() {
                {
                    std::lock_guard<std::mutex> const lock{pathMutex};
                    socketPath = path;
                }
                if(acceptor.has_value()) {
                    pendingRestartPort = currentPort.load();
                    releaseNow();
                } else if(status == TcpPortStatus::PortOccupied) {
                    tryBind(currentPort.load());   // the new place may be free
                } else if(statusChangef) {
                    statusChangef();
                }
            });
        }

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
                    releaseNow();
                } else {
                    status = TcpPortStatus::NotStarted;
                    if(statusChangef) { statusChangef(); }
                }
            });
        }

    private:
        // Frees the address before anything posted after this runs (another listener may bind
        // it next); the pending accept still completes with operation_aborted and finishes up.
        void releaseNow() {
            boost::system::error_code ec;
            acceptor->close(ec);
            removeSocketFile();
        }

        void removeSocketFile() {
            if(ownsSocketFile.exchange(false)) {
                std::error_code ec;
                std::filesystem::remove(boundPath, ec);
            }
        }

        void tryBind(std::uint16_t port) {
            auto const path = getSocketPath();
            try {
                acceptor.emplace(ioc);
                if(path.empty()) {
                    boost::asio::ip::tcp::endpoint const endpoint{bindAddress, port};
                    acceptor->open(endpoint.protocol());
                    acceptor->set_option(boost::asio::socket_base::reuse_address{true});
                    acceptor->bind(endpoint);
                } else {
                    if(auto const claimed = claimUnixSocketPath(path); !claimed) {
                        throw boost::system::system_error{boost::asio::error::address_in_use,
                                                          claimed.error()};
                    }
                    boost::asio::local::stream_protocol::endpoint const endpoint{path.native()};
                    acceptor->open(endpoint.protocol());
                    acceptor->bind(endpoint);
                    boundPath      = path;
                    ownsSocketFile = true;
                    std::error_code ec;
                    std::filesystem::permissions(path,
                                                 std::filesystem::perms::owner_read
                                                   | std::filesystem::perms::owner_write,
                                                 ec);
                }
                acceptor->listen();
                status      = TcpPortStatus::Active;
                currentPort = port;
                if(statusChangef) { statusChangef(); }
                asyncAcceptOne();
            } catch(boost::system::system_error const& e) {
                boost::system::error_code ec;
                if(acceptor.has_value()) { acceptor->close(ec); }
                acceptor.reset();
                removeSocketFile();
                if(path.empty()) {
                    errorf(fmt::format("TCP port {}: {}", port, e.what()));
                } else {
                    errorf(fmt::format("socket {}: {}", path.native(), e.what()));
                }
                status      = TcpPortStatus::PortOccupied;
                currentPort = port;
                if(statusChangef) { statusChangef(); }
            }
        }

        void asyncAcceptOne() {
            if(!acceptor.has_value()) { return; }
            acceptor->async_accept(
              [this](boost::system::error_code error_code, StreamSocket socket) {
                  if(!error_code) {
                      boost::system::error_code ec;
                      socket.set_option(boost::asio::socket_base::keep_alive{true}, ec);
                      onAccept(std::move(socket));
                      asyncAcceptOne();
                  } else if(error_code == boost::asio::error::operation_aborted) {
                      boost::system::error_code ec;
                      if(acceptor.has_value()) { acceptor->close(ec); }
                      acceptor.reset();
                      removeSocketFile();
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
