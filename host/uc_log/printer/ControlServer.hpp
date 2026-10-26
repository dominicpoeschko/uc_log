#pragma once

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/ClockedIo.hpp"
#include "uc_log/printer/ControlAnswer.hpp"
#include "uc_log/printer/ControlHistory.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"
#include "uc_log/printer/SocketSession.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

/// Serves ControlProtocol.hpp over a unix socket or a TCP port: controlAnswer() (ControlAnswer.hpp) computes the
/// answers, ControlServer handles the connections.
namespace uc_log::printer {

inline std::filesystem::path controlSocketPath(std::filesystem::path const& logDir) {
    return unixSocketPath(logDir, control::socketName);
}

/// One socket thread (SocketListener/SocketSession assume that); requests run on a worker pool.
class ControlServer {
public:
    using ErrorF  = std::function<void(std::string_view)>;
    using StatusF = std::function<void(SocketPortStatus, std::uint16_t)>;

    static constexpr std::size_t sendQueueCap    = 4 * 1024 * 1024;
    static constexpr std::size_t maxLineBytes    = 64 * 1024;
    static constexpr std::size_t maxPendingBytes = 1024 * 1024;
    static constexpr std::size_t workers         = 8;
    /// Stands in `pending` for a line that was too long (a client sending it just gets that error).
    static constexpr std::string_view tooLongMarker{"\x01line too long"};
    /// wait, reset and flash at once; the other workers stay free for the quick requests (a TCP
    /// client that dies in a wait is only noticed at its timeout).
    static constexpr std::size_t maxLongRequests = workers - 2;
    static constexpr std::size_t backlogChunk    = 256 * 1024;
    static constexpr auto        backlogRetry    = std::chrono::milliseconds{2};
    static constexpr auto        backlogStall    = std::chrono::seconds{30};

    /// Listens on `socketPath`, or bindAddress:port when empty; binds asynchronously.
    ControlServer(boost::asio::ip::address bindAddress,
                  std::uint16_t            port,
                  std::filesystem::path    socketPath,
                  ErrorF errorf            UC_LOG_LIFETIMEBOUND = {},
                  StatusF statusf          UC_LOG_LIFETIMEBOUND = {})
      : errorf_{std::move(errorf)}
      , statusf_{std::move(statusf)}
      , listener_{ioc_,
                  std::move(bindAddress),
                  [this](StreamSocket socket) { onAccept(std::move(socket)); },
                  [this](std::string_view msg) {
                      if(errorf_) { errorf_(msg); }
                  },
                  [this]() {
                      if(statusf_) {
                          statusf_(listener_.status.load(), listener_.currentPort.load());
                      }
                  },
                  std::move(socketPath)} {
        listener_.start(port);
        sweep();
        for(auto& worker : pool_) {
            worker = Clock::Thread{[this] { runIo(poolIoc_); }};
        }
        ioThread_    = Clock::Thread{[this] { runIo(ioc_); }};
        watchThread_ = Clock::Thread{[this](std::stop_token const& stop) { watch(stop); }};
    }

    ~ControlServer() {
        watchThread_.request_stop();
        if(watchThread_.joinable()) { watchThread_.join(); }
        ioc_.stop();
        if(ioThread_.joinable()) { ioThread_.join(); }
        targetStop_ = true;
        poolWork_.reset();   // the workers end once the queued requests are answered
        for(auto& worker : pool_) { worker.join(); }
    }

    ControlServer(ControlServer const&)            = delete;
    ControlServer& operator=(ControlServer const&) = delete;

    void setTarget(ControlTarget target) {
        std::unique_lock<std::shared_mutex> const lock{targetMutex_};
        targetStop_  = false;
        target_      = std::move(target);
        target_.stop = &targetStop_;
        hasTarget_   = true;
    }

    /// Blocks until requests using the target are done.
    void clearTarget() {
        targetStop_ = true;
        std::unique_lock<std::shared_mutex> const lock{targetMutex_};
        target_    = {};
        hasTarget_ = false;
    }

    void restart(std::uint16_t newPort) { listener_.restart(newPort); }

    /// Empty: back to TCP.
    void setSocketPath(std::filesystem::path path) { listener_.setSocketPath(std::move(path)); }

    void setBindAddress(boost::asio::ip::address address) {
        listener_.setBindAddress(std::move(address));
    }

    void stop() {
        listener_.stop([this]() { closeAllClients(); });
    }

    SocketPortStatus getStatus() const { return listener_.status.load(); }

    std::uint16_t getPort() const { return listener_.currentPort.load(); }

    std::size_t getClientCount() const {
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        return clients_.size();
    }

    std::uint64_t droppedBytes() const { return droppedBytes_.load(); }

    std::size_t requestsInFlight() const { return inFlight_.load(); }

    /// The self-check's limit: a socket thread that has not turned, or a pool with no free worker,
    /// for that long is reported through errorf (once, and again when it is over).
    void setStuckLimit(std::chrono::milliseconds limit) { stuckLimit_ = limit; }

    bool wants(control::Stream stream) const {
        return subscribers_[static_cast<std::size_t>(stream)].load(std::memory_order_relaxed) != 0;
    }

    /// Kept in the history even with no subscriber.
    void publish(control::LogLine line) {
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        auto const&                        text = history_.append(line);
        auto constexpr index                    = static_cast<std::size_t>(control::Stream::log);
        if(subscribers_[index].load(std::memory_order_relaxed) == 0) { return; }
        for(auto const& client : clients_) {
            if(!client->streams[index] || client->catchingUp
               || !control::passes(client->filter, line))
            {
                continue;
            }
            send(*client, text);
        }
    }

    std::uint64_t logSeq() const {
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        return history_.nextSeq();
    }

    void setHistoryLimit(std::size_t bytes) {
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        history_.setLimit(bytes);
    }

    void publish(control::StatusMessage const& message) { publishEvent(message); }

    void publish(control::MetricSample const& sample) { publishEvent(sample); }

private:
    struct Client {
        std::shared_ptr<SocketSession> session;
        // guarded by mutex; the socket thread appends to pending, one worker (busy) drains it
        std::mutex        mutex;
        std::string       pending;
        bool              busy{};
        bool              discarding{};   // inside a line that was too long
        bool              readClosed{};
        std::atomic<bool> gone{false};
        std::atomic<bool> halfClosed{false};
        // dup of the session's fd: a worker must never poll a number the kernel reused
        int fd{-1};

        Client() = default;

        Client(Client const&)            = delete;
        Client& operator=(Client const&) = delete;

        ~Client() {
            if(fd >= 0) { ::close(fd); }
        }

        /// Both directions closed; only a unix socket can tell (POLLHUP), not TCP.
        [[nodiscard]] bool hungUp() const {
            if(gone) { return true; }
            if(fd < 0) { return false; }
            pollfd p{.fd = fd, .events = 0, .revents = 0};
            return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
        }

        std::atomic<bool> streaming{false};
        // lines dropped for this client since the last Dropped event it got (send())
        std::atomic<std::uint64_t> droppedLines{0};
        // guarded by clientsMutex_
        control::Subscribe  filter;
        std::array<bool, 3> streams{};
        bool                catchingUp{};   // live log lines wait until the history is sent
        std::uint64_t       cursor{};
        std::uint64_t       lost{};
        // socket thread
        Clock::time_point backlogProgress{};
    };

    boost::asio::io_context                                                  ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_{
      ioc_.get_executor()};
    ErrorF         errorf_;
    StatusF        statusf_;
    SocketListener listener_;

    mutable std::mutex                   clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;   // guarded by clientsMutex_
    ControlHistory                       history_;   // guarded by clientsMutex_
    std::array<std::atomic<int>, 3>      subscribers_{};
    std::atomic<std::uint64_t>           droppedBytes_{0};
    std::atomic<std::size_t>             inFlight_{0};

    std::shared_mutex targetMutex_;
    ControlTarget     target_;
    bool              hasTarget_{false};   // guarded by targetMutex_
    std::atomic<bool> targetStop_{false};

    ClockTimer sweepTimer_{ioc_};
    // the workers: an io_context of their own run by Workers threads (on the Clock: a test's virtual time waits for
    // them), started after everything else exists
    boost::asio::io_context                                                  poolIoc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> poolWork_{
      poolIoc_.get_executor()};
    std::atomic<std::size_t> longRequests_{0};

    // the self-check (watch())

    struct Running {
        std::string_view  cmd;
        Clock::time_point since;
    };

    std::atomic<std::chrono::milliseconds> stuckLimit_{std::chrono::seconds{10}};
    std::atomic<Clock::time_point>         ioTurn_{Clock::time_point::min()};
    std::atomic<Clock::time_point>         poolTurn_{Clock::time_point::min()};
    std::mutex                             runningMutex_;
    std::list<Running>                     running_;   // guarded by runningMutex_

    std::array<Clock::Thread, workers> pool_;
    Clock::Thread                      ioThread_;
    Clock::Thread                      watchThread_;

    // own thread, 1 Hz: both the socket thread and the pool take a turn, or a status line says
    // which of them is stuck and what the workers are doing - so a control path that goes quiet
    // shows its cause in the log instead of only in its clients' timeouts
    void watch(std::stop_token const& stop) {
        std::mutex               mutex;
        Clock::ConditionVariable wake;
        bool                     ioStuck   = false;
        bool                     poolStuck = false;
        // a turn asked for at `asked` is taken once the stamp reaches it; one asked at a time
        auto       ioAsked   = Clock::time_point::min();
        auto       poolAsked = Clock::time_point::min();
        auto const ask
          = [](auto& executor, std::atomic<Clock::time_point>& turn, Clock::time_point& asked) {
                if(turn.load() < asked) { return; }
                asked = Clock::now();
                boost::asio::post(executor, [&turn] { turn = Clock::now(); });
            };
        while(!stop.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock{mutex};
                wake.wait_for(
                  lock,
                  stop,
                  std::chrono::milliseconds{std::max<long>(stuckLimit_.load().count() / 4, 10)},
                  [] { return false; });
            }
            if(stop.stop_requested()) { return; }
            ask(ioc_, ioTurn_, ioAsked);
            ask(poolIoc_, poolTurn_, poolAsked);
            auto const now   = Clock::now();
            auto const limit = stuckLimit_.load();
            auto const ms    = [](Clock::duration d) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
            };
            auto const age
              = [now](std::atomic<Clock::time_point> const& turn, Clock::time_point asked) {
                    return turn.load() < asked ? now - asked : Clock::duration::zero();
                };
            auto const ioAge   = age(ioTurn_, ioAsked);
            auto const poolAge = age(poolTurn_, poolAsked);
            if(!ioStuck && ioAge > limit) {
                ioStuck = true;
                report(
                  fmt::format("the socket thread has not turned for {} ms: no client is "
                              "accepted, read or written",
                              ms(ioAge)));
            } else if(ioStuck && ioAge <= limit) {
                ioStuck = false;
                report("the socket thread turns again");
            }
            if(!poolStuck && poolAge > limit) {
                poolStuck = true;
                std::string what;
                {
                    std::scoped_lock<std::mutex> const lock{runningMutex_};
                    for(auto const& r : running_) {
                        what += fmt::format("{}{} for {} ms",
                                            what.empty() ? "" : ", ",
                                            r.cmd,
                                            ms(now - r.since));
                    }
                }
                report(
                  fmt::format("no worker free for {} ms, requests wait: {} of {} workers "
                              "busy (in the target: {})",
                              ms(poolAge),
                              inFlight_.load(),
                              workers,
                              what.empty() ? "none in a request" : what));
            } else if(poolStuck && poolAge <= limit) {
                poolStuck = false;
                report("a worker is free again");
            }
        }
    }

    // a request in running_ while it is answered
    struct RunningEntry {
        ControlServer&               server;
        std::list<Running>::iterator at;

        RunningEntry(ControlServer& s UC_LOG_LIFETIMEBOUND,
                     std::string_view cmd)
          : server{s} {
            std::scoped_lock<std::mutex> const lock{server.runningMutex_};
            at = server.running_.insert(server.running_.end(), Running{cmd, Clock::now()});
        }

        RunningEntry(RunningEntry const&)            = delete;
        RunningEntry& operator=(RunningEntry const&) = delete;

        ~RunningEntry() {
            std::scoped_lock<std::mutex> const lock{server.runningMutex_};
            server.running_.erase(at);
        }
    };

    void report(std::string_view what) {
        if(errorf_) { errorf_(fmt::format("self-check: {}", what)); }
    }

    template<typename T>
    void publishEvent(T const& value) {
        auto const index = static_cast<std::size_t>(control::streamOf<T>());
        if(subscribers_[index].load(std::memory_order_relaxed) == 0) { return; }
        std::string                        line;
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        for(auto const& client : clients_) {
            if(!client->streams[index] || !control::delivers(client->filter, value)) { continue; }
            if(line.empty()) { line = control::toLine(control::Event{value}); }
            send(*client, line);
        }
    }

    // A line that does not fit the client's queue is dropped for that client and counted; the count goes to it as a
    // Dropped event ahead of the next line that fits, so the gap shows in its stream where it is.
    void send(Client&          client,
              std::string_view text) {
        if(!sendDropped(client) || !client.session->trySend(std::as_bytes(std::span{text}))) {
            droppedBytes_ += text.size();
            ++client.droppedLines;
        }
    }

    // false: the Dropped event did not fit either
    bool sendDropped(Client& client) {
        auto const lines = client.droppedLines.load();
        if(lines == 0) { return true; }
        auto const marker = control::toLine(control::Event{control::Dropped{.lines = lines}});
        if(!client.session->trySend(std::as_bytes(std::span{marker}))) { return false; }
        client.droppedLines -= lines;
        return true;
    }

    // socket thread
    void onAccept(StreamSocket socket) {
        auto                        client = std::make_shared<Client>();
        std::weak_ptr<Client> const weak   = client;
        client->fd                         = ::fcntl(socket.native_handle(), F_DUPFD_CLOEXEC, 0);
        client->session                    = std::make_shared<SocketSession>(
          std::move(socket),
          sendQueueCap,
          [this](std::string_view msg) {
              if(errorf_) { errorf_(fmt::format("control client {}", msg)); }
          },
          [this, weak](std::span<std::byte const> data, std::shared_ptr<SocketSession> const&) {
              auto const c = weak.lock();
              if(c) { onData(c, data); }
              return c != nullptr;
          },
          [this, weak](std::shared_ptr<SocketSession> const&) {
              if(auto const c = weak.lock()) { remove(c); }
          });
        client->session->onEof = [this, weak](std::shared_ptr<SocketSession> const&) {
            if(auto const c = weak.lock()) { onEof(c); }
        };
        {
            std::scoped_lock<std::mutex> const lock{clientsMutex_};
            clients_.push_back(client);
        }
        client->session->startRead();
    }

    // socket thread
    void onData(std::shared_ptr<Client> const& client,
                std::span<std::byte const>     data) {
        if(client->streaming) { return; }
        std::scoped_lock<std::mutex> const lock{client->mutex};
        std::string_view incoming{reinterpret_cast<char const*>(data.data()), data.size()};
        if(client->discarding) {
            // the rest of the long line is in the new bytes only: `pending` still holds the
            // lines before it that a worker has not taken yet
            auto const end = incoming.find('\n');
            if(end == std::string_view::npos) { return; }
            incoming.remove_prefix(end + 1);
            client->discarding = false;
        }
        client->pending.append(incoming);
        auto       newline = client->pending.rfind('\n');
        auto const tail    = newline == std::string::npos ? client->pending.size()
                                                          : client->pending.size() - newline - 1;
        if(tail > maxLineBytes) {
            // answered in line with the requests before it, which a worker may still be serving
            client->pending.erase(newline == std::string::npos ? 0 : newline + 1);
            client->pending.append(tooLongMarker).push_back('\n');
            newline            = client->pending.size() - 1;
            client->discarding = true;
        }
        if(client->pending.size() > maxPendingBytes) {
            client->pending.clear();
            newline = std::string::npos;
            send(*client,
                 control::toLine(
                   control::Answer{control::Error{.error = "too many requests queued"}}));
        }
        if(newline != std::string::npos && !client->busy) {
            // what needs no target is answered right here: workers stuck in target requests (a
            // probe call that does not come back) must not take ping and subscribe with them
            while(true) {
                auto const end = client->pending.find('\n');
                if(end == std::string::npos) { return; }
                auto const quick
                  = answerWithoutTarget(client, std::string_view{client->pending}.substr(0, end));
                if(quick == Quick::subscribed) {
                    client->pending.clear();
                    return;
                }
                if(quick == Quick::needsTarget) { break; }
                client->pending.erase(0, end + 1);
            }
            client->busy = true;
            boost::asio::post(poolIoc_, [this, client] { serve(client); });
        }
    }

    enum class Quick { answered, subscribed, needsTarget };

    // socket thread or worker: answers `line` if that needs no target (and so no worker); a request that needs the
    // target is handed out in `needed`
    Quick answerWithoutTarget(std::shared_ptr<Client> const&   client,
                              std::string_view                 line,
                              std::optional<control::Request>* needed = nullptr) {
        if(line == tooLongMarker) {
            send(*client,
                 control::toLine(control::Answer{control::Error{.error = "line too long"}}));
            return Quick::answered;
        }
        auto const parsed = control::parseRequest(line);
        if(auto const* e = std::get_if<control::Error>(&parsed)) {
            send(*client, control::toLine(control::Answer{*e}));
            return Quick::answered;
        }
        auto const& request = std::get<control::Request>(parsed);
        if(std::holds_alternative<control::Ping>(request)) {
            send(*client, control::toLine(control::Answer{control::PingAnswer{}}));
            return Quick::answered;
        }
        if(auto const* s = std::get_if<control::Subscribe>(&request)) {
            if(auto const why = control::subscribeError(*s)) {
                send(*client, control::toLine(control::Answer{control::Error{.error = *why}}));
                return Quick::answered;
            }
            subscribe(client, *s);
            return Quick::subscribed;
        }
        if(needed != nullptr) { *needed = request; }
        return Quick::needsTarget;
    }

    // socket thread, 1 Hz: a half-closed client is only seen closing through hungUp() (a unix
    // socket; on TCP it shows at the next failed send).
    void sweep() {
        sweepTimer_.expires_after(std::chrono::seconds{1});
        sweepTimer_.async_wait([this](boost::system::error_code ec) {
            if(ec) { return; }
            std::vector<std::shared_ptr<Client>> leaving;
            {
                std::scoped_lock<std::mutex> const lock{clientsMutex_};
                for(auto const& client : clients_) {
                    if(client->halfClosed && client->hungUp()) { leaving.push_back(client); }
                    // a gap with no line after it: its Dropped event once the queue has room
                    if(client->streaming) { (void)sendDropped(*client); }
                }
            }
            for(auto const& client : leaving) {
                remove(client);
                client->session->close();
            }
            sweep();
        });
    }

    // socket thread: queued lines are still answered, then the connection closes.
    void onEof(std::shared_ptr<Client> const& client) {
        client->halfClosed = true;
        if(client->streaming) { return; }
        std::scoped_lock<std::mutex> const lock{client->mutex};
        client->readClosed = true;
        if(!client->busy) { client->session->closeWhenSent(); }
    }

    // worker
    void serve(std::shared_ptr<Client> const& client) {
        ++inFlight_;

        struct Done {
            std::atomic<std::size_t>& n;

            ~Done() { --n; }
        } const done{inFlight_};

        while(true) {
            std::string line;
            {
                std::scoped_lock<std::mutex> const lock{client->mutex};
                auto const                         end = client->pending.find('\n');
                if(end == std::string::npos || client->gone) {
                    client->busy = false;
                    if(client->readClosed) { client->session->closeWhenSent(); }
                    return;
                }
                line = client->pending.substr(0, end);
                client->pending.erase(0, end + 1);
            }
            std::optional<control::Request> needed;
            auto const                      quick = answerWithoutTarget(client, line, &needed);
            if(quick == Quick::answered) { continue; }
            if(quick == Quick::subscribed) {
                std::scoped_lock<std::mutex> const lock{client->mutex};
                client->pending.clear();
                client->busy = false;
                return;
            }
            if(!needed) { continue; }   // needsTarget hands the request out in `needed`
            auto const& request = *needed;
            bool const  isLong  = std::holds_alternative<control::Wait>(request)
                               || std::holds_alternative<control::Reset>(request)
                               || std::holds_alternative<control::Flash>(request);
            if(isLong && longRequests_.fetch_add(1) >= maxLongRequests) {
                --longRequests_;
                send(*client,
                     control::toLine(control::Answer{control::Error{
                       .error = fmt::format("busy: {} waits, resets or flashes already run",
                                            maxLongRequests)}}));
                continue;
            }
            auto answer = [&] {
                RunningEntry const entry{*this, glz::meta<control::Request>::ids[request.index()]};
                return answerRequest(request, [&client] { return client->hungUp(); });
            }();
            if(isLong) { --longRequests_; }
            send(*client, answer);
        }
    }

    std::string answerRequest(control::Request const&      request,
                              std::function<bool()> const& cancelled) {
        std::shared_lock<std::shared_mutex> const lock{targetMutex_};
        if(!hasTarget_) {
            return control::toLine(
              control::Answer{control::Error{.error = "the printer has no target yet"}});
        }
        return control::toLine(controlAnswer(request, target_, cancelled));
    }

    void subscribe(std::shared_ptr<Client> const& client,
                   control::Subscribe const&      request) {
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        // the client is already gone (remove() ran first): counting it would keep wants() true
        if(std::ranges::find(clients_, client) == clients_.end()) { return; }
        client->filter = request;
        for(auto const stream : request.streams) {
            auto const index = static_cast<std::size_t>(stream);
            if(!client->streams[index]) {
                client->streams[index] = true;
                ++subscribers_[index];
            }
        }
        client->streaming = true;
        // under the lock, so no event goes out before the answer
        send(*client, control::toLine(control::Answer{control::SubscribeAnswer{}}));
        if(request.sinceSeq || request.last) {
            client->catchingUp = true;
            client->cursor     = history_.start(request);
            boost::asio::post(ioc_, [this, client] {
                client->backlogProgress = Clock::now();
                backlogStep(client);
            });
        }
    }

    // socket thread: history, then live lines; switched under clientsMutex_ so none is lost.
    void backlogStep(std::shared_ptr<Client> const& client) {
        {
            std::scoped_lock<std::mutex> const lock{clientsMutex_};
            if(client->gone || std::ranges::find(clients_, client) == clients_.end()) { return; }
            auto const queued = client->session->queuedBytes.load();
            if(queued + backlogChunk <= sendQueueCap) {
                auto chunk = history_.read(client->filter, client->cursor, backlogChunk);
                if(chunk.atEnd) {
                    chunk.text += control::toLine(control::Event{
                      control::BacklogEnd{.nextSeq = chunk.next,
                                          .lost    = client->lost + chunk.lost}
                    });
                }
                if(chunk.text.empty()
                   || client->session->trySend(std::as_bytes(std::span{chunk.text})))
                {
                    client->cursor = chunk.next;
                    client->lost += chunk.lost;
                    client->backlogProgress = Clock::now();
                    if(chunk.atEnd) {
                        if(client->filter.follow) {
                            client->catchingUp = false;
                        } else {
                            client->session->closeWhenSent();
                        }
                        return;
                    }
                    boost::asio::post(ioc_, [this, client] { backlogStep(client); });
                    return;
                }
            }
        }
        if(Clock::now() - client->backlogProgress > backlogStall) {
            if(errorf_) { errorf_("control client does not read its history: closed"); }
            remove(client);
            client->session->close();
            return;
        }
        auto timer = std::make_shared<ClockTimer>(ioc_, backlogRetry);
        timer->async_wait([this, client, timer](boost::system::error_code ec) {
            if(!ec) { backlogStep(client); }
        });
    }

    // socket thread
    void remove(std::shared_ptr<Client> const& client) {
        client->gone = true;
        std::scoped_lock<std::mutex> const lock{clientsMutex_};
        for(std::size_t i = 0; i != client->streams.size(); ++i) {
            if(client->streams[i]) {
                client->streams[i] = false;
                --subscribers_[i];
            }
        }
        std::erase(clients_, client);
    }

    // socket thread
    void closeAllClients() {
        std::vector<std::shared_ptr<Client>> toClose;
        {
            std::scoped_lock<std::mutex> const lock{clientsMutex_};
            toClose = std::move(clients_);
            clients_.clear();
            for(auto const& client : toClose) { client->streams = {}; }
            for(auto& count : subscribers_) { count = 0; }
        }
        for(auto const& client : toClose) {
            client->gone = true;
            client->session->close();
        }
    }
};

}   // namespace uc_log::printer
