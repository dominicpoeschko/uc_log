#pragma once

#include "uc_log/detail/DuplexChannelInfo.hpp"
#include "uc_log/detail/RttChannelMap.hpp"
#include "uc_log/detail/TcpPortStatus.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log { namespace detail {

    // transparent TCP bridge for one duplex channel, single client, first wins
    struct DuplexChannelServer {
        static constexpr std::size_t RecvQueueCap = 64 * 1024;
        static constexpr std::size_t SendQueueCap = 1024 * 1024;

        std::string                           name;
        std::function<void(std::string_view)> errorMessagef;
        std::function<void(std::string_view)> messagef;
        std::function<void()>                 stateChangef;
        mutable std::mutex                    mutex;
        std::shared_ptr<TcpSession>           activeSession;   // guarded by mutex
        std::weak_ptr<TcpSession>             pausedSession;   // guarded by mutex
        std::vector<std::byte>                recvQueue;       // guarded by mutex
        std::atomic<bool>                     enabled{true};
        std::atomic<bool>                     hostToTargetOnly{false};
        std::atomic<std::uint64_t>            bytesToTarget{0};
        std::atomic<std::uint64_t>            bytesFromTarget{0};
        std::atomic<std::uint64_t>            bytesDropped{0};
        TcpListener                           listener;

        template<typename ErrorMessageF,
                 typename MessageF,
                 typename StateChangeF>
        DuplexChannelServer(boost::asio::io_context& ioc,
                            boost::asio::ip::address bindAddress,
                            std::string              name_,
                            std::uint16_t            port,
                            ErrorMessageF&&          errorMessagef_,
                            MessageF&&               messagef_,
                            StateChangeF&&           stateChangef_)
          : name{std::move(name_)}
          , errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
          , messagef{std::forward<MessageF>(messagef_)}
          , stateChangef{std::forward<StateChangeF>(stateChangef_)}
          , listener{ioc,
                     std::move(bindAddress),
                     [this](boost::asio::ip::tcp::socket socket) { onAccept(std::move(socket)); },
                     [this](std::string_view msg) {
                         errorMessagef(fmt::format("duplex \"{}\": {}", name, msg));
                     },
                     [this]() { notifyStateChange(); }} {
            listener.start(port);
        }

        // reader thread -> socket, silently dropped when no client is connected
        void sendToClient(std::span<std::byte const> data) {
            std::shared_ptr<TcpSession> session;
            {
                std::lock_guard<std::mutex> const lock{mutex};
                session = activeSession;
            }
            if(session) {
                if(session->trySend(data)) {
                    bytesFromTarget += data.size();
                } else {
                    // client too slow, dropped so a stalled client cannot grow host memory
                    bytesDropped += data.size();
                }
            }
        }

        // socket -> reader thread, non destructive: only consumeFromClient removes bytes
        std::size_t peekFromClient(std::span<std::byte> out) {
            std::lock_guard<std::mutex> const lock{mutex};
            auto const                        n = std::min(out.size(), recvQueue.size());
            std::copy_n(recvQueue.begin(), n, out.begin());
            return n;
        }

        void consumeFromClient(std::size_t n) {
            std::shared_ptr<TcpSession> resume;
            {
                std::lock_guard<std::mutex> const lock{mutex};
                n = std::min(n, recvQueue.size());
                recvQueue.erase(
                  recvQueue.begin(),
                  std::next(recvQueue.begin(), static_cast<std::make_signed_t<std::size_t>>(n)));
                bytesToTarget += n;
                if(recvQueue.size() < RecvQueueCap) {
                    resume = pausedSession.lock();
                    pausedSession.reset();
                }
            }
            if(resume) { resume->resumeRead(); }
        }

        // on rtt (re)start or target reset: stale bytes must not leak into the new session
        void clearBuffers() {
            std::shared_ptr<TcpSession> resume;
            {
                std::lock_guard<std::mutex> const lock{mutex};
                recvQueue.clear();
                resume = pausedSession.lock();
                pausedSession.reset();
            }
            if(resume) { resume->resumeRead(); }
        }

        void restart(std::uint16_t newPort) {
            enabled = true;
            listener.restart(newPort);
        }

        void stop() {
            enabled = false;
            listener.stop([this]() { closeSession(); });
        }

        // no notifyStateChange here: this is called from gui callbacks which already hold the gui
        // mutex, the triggering event redraws anyway
        void setPort(std::uint16_t newPort) {
            if(enabled) {
                restart(newPort);
            } else {
                listener.currentPort = newPort;
            }
        }

        void setBindAddress(boost::asio::ip::address address) {
            listener.setBindAddress(std::move(address));
            // an enabled server that is not currently listening (e.g. its port was
            // occupied) retries on the new address; the setBindAddress post above runs
            // first, so restart binds the updated address
            if(enabled && listener.status.load() != TcpPortStatus::Active) {
                restart(listener.currentPort.load());
            }
        }

        bool hasClient() const {
            std::lock_guard<std::mutex> const lock{mutex};
            return activeSession != nullptr;
        }

        DuplexChannelInfo info(std::size_t ordinal) const {
            return DuplexChannelInfo{ordinal,
                                     name,
                                     listener.currentPort.load(),
                                     enabled ? listener.status.load() : TcpPortStatus::NotStarted,
                                     enabled.load(),
                                     hasClient(),
                                     hostToTargetOnly.load(),
                                     bytesToTarget.load(),
                                     bytesFromTarget.load(),
                                     bytesDropped.load()};
        }

    private:
        void notifyStateChange() {
            if(stateChangef) { stateChangef(); }
        }

        // ioc thread
        void onAccept(boost::asio::ip::tcp::socket socket) {
            bool haveClient{};
            {
                std::lock_guard<std::mutex> const lock{mutex};
                haveClient = activeSession != nullptr;
            }
            if(haveClient) {
                // first client wins
                boost::system::error_code ec;
                socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                socket.close(ec);
                messagef(fmt::format("duplex \"{}\": rejected second client", name));
                return;
            }
            auto session = std::make_shared<TcpSession>(
              std::move(socket),
              SendQueueCap,
              [this](std::string_view msg) {
                  errorMessagef(fmt::format("duplex \"{}\": client {}", name, msg));
              },
              [this](std::span<std::byte const> data, std::shared_ptr<TcpSession> const& session_) {
                  return onClientData(data, session_);
              },
              [this](std::shared_ptr<TcpSession> const& session_) { onClientGone(session_); });
            {
                std::lock_guard<std::mutex> const lock{mutex};
                activeSession = session;
                pausedSession.reset();
                recvQueue.clear();
            }
            session->startRead();
            messagef(fmt::format("duplex \"{}\": client connected", name));
            notifyStateChange();
        }

        // ioc thread, returns false to pause reading until consumeFromClient drains the queue
        bool onClientData(std::span<std::byte const>         data,
                          std::shared_ptr<TcpSession> const& session) {
            std::lock_guard<std::mutex> const lock{mutex};
            if(session != activeSession) { return false; }
            recvQueue.insert(recvQueue.end(), data.begin(), data.end());
            if(recvQueue.size() < RecvQueueCap) { return true; }
            pausedSession = session;
            return false;
        }

        // ioc thread
        void onClientGone(std::shared_ptr<TcpSession> const& session) {
            {
                std::lock_guard<std::mutex> const lock{mutex};
                if(session != activeSession) { return; }
                activeSession.reset();
                pausedSession.reset();
                recvQueue.clear();
            }
            messagef(fmt::format("duplex \"{}\": client disconnected", name));
            notifyStateChange();
        }

        // ioc thread
        void closeSession() {
            std::shared_ptr<TcpSession> session;
            {
                std::lock_guard<std::mutex> const lock{mutex};
                session = std::move(activeSession);
                activeSession.reset();
                pausedSession.reset();
                recvQueue.clear();
            }
            if(session) { session->close(); }
        }
    };

    // owns all channel servers, servers persist across rtt restarts keyed by channel name so
    // port overrides survive a reflash. The io_context is shared and owned by the caller.
    struct DuplexChannelHub {
    private:
        std::function<void(std::string_view)>                       errorMessagef;
        std::function<void(std::string_view)>                       messagef;
        std::function<void()>                                       stateChangef;
        boost::asio::io_context&                                    ioc;
        boost::asio::ip::address                                    bindAddress;
        mutable std::mutex                                          mutex;
        std::map<std::string, std::shared_ptr<DuplexChannelServer>> serversByName;
        std::vector<std::shared_ptr<DuplexChannelServer>>           activeServers;
        std::uint16_t                                               basePort;

        std::shared_ptr<DuplexChannelServer> serverAt(std::size_t ordinal) const {
            std::lock_guard<std::mutex> const lock{mutex};
            if(ordinal >= activeServers.size()) { return nullptr; }
            return activeServers[ordinal];
        }

    public:
        template<typename ErrorMessageF,
                 typename MessageF,
                 typename StateChangeF>
        DuplexChannelHub(boost::asio::io_context& ioc_,
                         boost::asio::ip::address bindAddress_,
                         std::uint16_t            basePort_,
                         ErrorMessageF&&          errorMessagef_,
                         MessageF&&               messagef_,
                         StateChangeF&&           stateChangef_)
          : errorMessagef{std::forward<ErrorMessageF>(errorMessagef_)}
          , messagef{std::forward<MessageF>(messagef_)}
          , stateChangef{std::forward<StateChangeF>(stateChangef_)}
          , ioc{ioc_}
          , bindAddress{std::move(bindAddress_)}
          , basePort{basePort_} {}

        // called from the reader thread on every rtt (re)start, idempotent
        void configure(std::vector<DuplexChannelDesc> const& descs) {
            {
                std::lock_guard<std::mutex> const lock{mutex};

                std::vector<std::shared_ptr<DuplexChannelServer>> newActive;
                for(auto const& desc : descs) {
                    std::shared_ptr<DuplexChannelServer> server;
                    if(auto const it = serversByName.find(desc.name); it != serversByName.end()) {
                        server = it->second;
                        server->clearBuffers();
                        // also retries PortOccupied: the port may have been freed since the last try
                        if(server->enabled
                           && server->listener.status.load() != TcpPortStatus::Active)
                        {
                            server->restart(server->listener.currentPort.load());
                        }
                    } else {
                        server = std::make_shared<DuplexChannelServer>(
                          ioc,
                          bindAddress,
                          desc.name,
                          static_cast<std::uint16_t>(basePort + desc.ordinal),
                          errorMessagef,
                          messagef,
                          stateChangef);
                        serversByName.emplace(desc.name, server);
                    }
                    server->hostToTargetOnly = !desc.upIndex.has_value();
                    newActive.push_back(std::move(server));
                }

                // servers of channels that vanished (reflash with a different config) are stopped
                // but kept alive, sessions may still reference them
                for(auto const& [name_, server] : serversByName) {
                    if(std::ranges::find(newActive, server) == newActive.end()
                       && server->listener.status.load() != TcpPortStatus::NotStarted)
                    {
                        server->stop();
                    }
                }
                activeServers = std::move(newActive);
            }
            // outside the lock: the callback may lock the gui mutex, which in turn is held while
            // info() locks this hub's mutex
            if(stateChangef) { stateChangef(); }
        }

        std::vector<DuplexChannelInfo> info() const {
            std::lock_guard<std::mutex> const lock{mutex};
            std::vector<DuplexChannelInfo>    ret;
            for(std::size_t ordinal{}; ordinal < activeServers.size(); ++ordinal) {
                ret.push_back(activeServers[ordinal]->info(ordinal));
            }
            return ret;
        }

        void setPort(std::size_t   ordinal,
                     std::uint16_t port) {
            if(auto const server = serverAt(ordinal)) { server->setPort(port); }
        }

        void setEnabled(std::size_t ordinal,
                        bool        enable) {
            if(auto const server = serverAt(ordinal)) {
                if(enable) {
                    server->restart(server->listener.currentPort.load());
                } else {
                    server->stop();
                }
            }
        }

        std::uint16_t getBasePort() const {
            std::lock_guard<std::mutex> const lock{mutex};
            return basePort;
        }

        // apply to every server (including inactive ones kept alive) and remember the
        // address for servers created on a later reconfigure
        void setBindAddress(boost::asio::ip::address address) {
            std::lock_guard<std::mutex> const lock{mutex};
            bindAddress = address;
            for(auto const& [name, server] : serversByName) { server->setBindAddress(address); }
        }

        void setBasePort(std::uint16_t newBasePort) {
            std::lock_guard<std::mutex> const lock{mutex};
            basePort = newBasePort;
            for(std::size_t ordinal{}; ordinal < activeServers.size(); ++ordinal) {
                activeServers[ordinal]->setPort(static_cast<std::uint16_t>(newBasePort + ordinal));
            }
        }

        // reader thread pump interface
        void sendToClient(std::size_t                ordinal,
                          std::span<std::byte const> data) {
            if(auto const server = serverAt(ordinal)) { server->sendToClient(data); }
        }

        std::size_t peekFromClient(std::size_t          ordinal,
                                   std::span<std::byte> out) {
            if(auto const server = serverAt(ordinal)) { return server->peekFromClient(out); }
            return 0;
        }

        void consumeFromClient(std::size_t ordinal,
                               std::size_t n) {
            if(auto const server = serverAt(ordinal)) { server->consumeFromClient(n); }
        }
    };

}}   // namespace uc_log::detail
