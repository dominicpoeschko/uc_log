#pragma once

#include "uc_log/detail/Clock.hpp"
#include "uc_log/detail/ClockedIo.hpp"
#include "uc_log/detail/ControlHistory.hpp"
#include "uc_log/detail/ControlProtocol.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/TcpPortStatus.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

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

/// Serves ControlProtocol.hpp over a unix socket or a TCP port: controlAnswer() computes the answers,
/// ControlServer handles the connections.
namespace uc_log::detail {

using MemoryResult = std::expected<std::vector<std::vector<std::byte>>, std::string>;
using MemoryReader = std::function<MemoryResult(std::span<control::Piece const>)>;
using MemoryWriter = std::function<MemoryResult(std::span<control::WordWrite const>)>;

/// What the server acts on. A function left empty answers an error for its request.
struct ControlTarget {
    /// Gets `cancelled`: true once the printer stops or the client is gone.
    using Action = std::function<std::expected<void, std::string>(std::function<bool()> const&)>;

    MemoryReader                           read{};
    MemoryWriter                           write{};
    std::function<control::StatusAnswer()> status{};
    Action                                 reset{};
    /// a reset through the probe's reset pin (`reset` with "line": true)
    Action resetLine{};
    Action flash{};
    /// halt (true) or resume (false) the core; the halted state after
    std::function<std::expected<bool, std::string>(bool)> core{};
    /// the core's registers (control::Registers' order); only while halted
    std::function<std::expected<std::vector<std::uint32_t>, std::string>()> registers{};
    /// core registers written (index, value); only while halted
    std::function<std::expected<void, std::string>(std::span<control::RegisterWrite const>)>
                                                                    writeRegisters{};
    std::function<std::vector<control::StatusMessage>(std::size_t)> messages{};
    /// µs clocks; empty = the real ones.
    std::function<std::int64_t()> unixMicros{};
    std::function<std::int64_t()> steadyMicros{};
    std::atomic<bool> const*      stop{};   // when set, running waits, resets and flashes return
};

namespace control_detail {
    inline std::int64_t realUnixMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    inline std::int64_t realSteadyMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                 Clock::now().time_since_epoch())
          .count();
    }

    inline std::int64_t unixMicros(ControlTarget const& t) {
        return t.unixMicros ? t.unixMicros() : realUnixMicros();
    }

    inline std::int64_t steadyMicros(ControlTarget const& t) {
        return t.steadyMicros ? t.steadyMicros() : realSteadyMicros();
    }

    inline std::string hex(std::span<std::byte const> bytes) {
        static constexpr std::string_view Digits = "0123456789abcdef";
        std::string                       out;
        out.reserve(2 * bytes.size());
        for(auto const b : bytes) {
            out += Digits[std::to_integer<unsigned>(b) >> 4U];
            out += Digits[std::to_integer<unsigned>(b) & 0xFU];
        }
        return out;
    }

    inline control::Error error(std::string text) {
        return control::Error{.error = std::move(text)};
    }

    inline control::Answer answer(control::Ping const&,
                                  ControlTarget const&) {
        return control::PingAnswer{};
    }

    inline control::Answer answer(control::Status const&,
                                  ControlTarget const& t) {
        if(!t.status) { return error("no status here"); }
        return t.status();
    }

    inline control::Answer answer(control::Messages const& m,
                                  ControlTarget const&     t) {
        if(!t.messages) { return error("no messages here"); }
        return control::MessagesAnswer{.messages = t.messages(m.count)};
    }

    inline control::Answer answer(control::Read const& r,
                                  ControlTarget const& t) {
        if(r.pieces.empty()) { return error("read: nothing to read"); }
        std::size_t total = 0;
        for(auto const& p : r.pieces) {
            if(p.size == 0) { return error("read: a piece of 0 bytes"); }
            total += p.size;
        }
        if(r.pieces.size() > control::MaxPieces || total > control::MaxReadBytes) {
            return error(fmt::format("read: too much for one request ({} pieces, {} bytes)",
                                     control::MaxPieces,
                                     control::MaxReadBytes));
        }
        if(!t.read) { return error("no target here"); }
        auto const result = t.read(r.pieces);
        auto const now    = unixMicros(t);
        if(!result) { return error(result.error()); }
        control::ReadAnswer out{.unix_us = now, .data = {}};
        out.data.reserve(result->size());
        for(auto const& bytes : *result) { out.data.push_back(hex(bytes)); }
        return out;
    }

    inline control::Answer answer(control::Write const& w,
                                  ControlTarget const&  t) {
        if(w.words.empty()) { return error("write: nothing to write"); }
        if(w.words.size() > control::MaxWriteWords) {
            return error(fmt::format("write: at most {} words a request", control::MaxWriteWords));
        }
        for(auto const& word : w.words) {
            if(word.address % 4 != 0) {
                return error(fmt::format("write: {:#010x} is not word aligned", word.address));
            }
        }
        if(!t.write) { return error("no target here"); }
        auto const result = t.write(w.words);
        auto const now    = unixMicros(t);
        if(!result) { return error(result.error()); }
        control::WriteAnswer out{.unix_us = now, .data = {}};
        out.data.reserve(result->size());
        for(auto const& bytes : *result) { out.data.push_back(hex(bytes)); }
        return out;
    }

    inline control::Answer answer(control::Wait const&         w,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled = {}) {
        using control::Condition;
        if(w.piece.size == 0 || w.piece.size > control::MaxWaitBytes) {
            return error("wait: a value is 1 to 8 bytes");
        }
        if(w.timeout_ms > control::MaxWaitTimeoutMs) {
            return error("wait: timeout_ms is at most 600000");
        }
        if(w.condition != Condition::changed && !w.value) {
            return error("wait: this condition needs a value");
        }
        if(!t.read) { return error("no target here"); }
        std::uint64_t const mask  = w.mask.value_or(~std::uint64_t{});
        std::uint64_t const value = w.value.value_or(0) & mask;

        // paced on the printer's clock only: a target with a clock of its own (steadyMicros) has its own time
        auto       nextSample = Clock::now();
        auto const sample     = [&]() -> std::expected<std::uint64_t, std::string> {
            if(!t.steadyMicros) {
                Clock::sleep_until(nextSample);
                nextSample = Clock::now() + std::chrono::milliseconds{1};
            }
            auto const result = t.read(std::span{&w.piece, 1});
            if(!result) { return std::unexpected{result.error()}; }
            if(result->empty()) {
                return std::unexpected{std::string{"wait: the read returned nothing"}};
            }
            std::uint64_t v{};
            for(std::size_t i = 0; i != result->front().size() && i != 8; ++i) {
                v |= std::to_integer<std::uint64_t>(result->front()[i]) << (8U * i);
            }
            return v & mask;
        };
        auto const start = steadyMicros(t);
        auto const first = sample();
        if(!first) { return error(first.error()); }
        auto last = *first;
        while(true) {
            bool const hit = [&] {
                switch(w.condition) {
                case Condition::changed: return last != *first;
                case Condition::eq:      return last == value;
                case Condition::ne:      return last != value;
                case Condition::lt:      return last < value;
                case Condition::gt:      return last > value;
                }
                return false;
            }();
            auto const waited = steadyMicros(t) - start;
            bool const late   = waited >= std::int64_t{w.timeout_ms} * 1000
                             || (t.stop != nullptr && *t.stop) || (cancelled && cancelled());
            if(hit || late) {
                return control::WaitAnswer{.hit       = hit,
                                           .unix_us   = unixMicros(t),
                                           .waited_us = waited,
                                           .first     = *first,
                                           .last      = last};
            }
            auto const next = sample();
            if(!next) { return error(next.error()); }
            last = *next;
        }
    }

    inline control::Answer action(ControlTarget::Action const& a,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled,
                                  control::Answer              done) {
        if(!a) { return error("not available here"); }
        auto const result
          = a([&] { return (t.stop != nullptr && *t.stop) || (cancelled && cancelled()); });
        if(!result) { return error(result.error()); }
        return done;
    }

    inline control::Answer answer(control::Reset const&        r,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled) {
        return action(r.line.value_or(false) ? t.resetLine : t.reset,
                      t,
                      cancelled,
                      control::ResetAnswer{});
    }

    inline control::Answer answer(control::Flash const&,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled) {
        return action(t.flash, t, cancelled, control::FlashAnswer{});
    }

    inline control::Answer answer(control::Halt const&,
                                  ControlTarget const& t) {
        if(!t.core) { return error("no target here"); }
        auto const result = t.core(true);
        if(!result) { return error(result.error()); }
        return control::HaltAnswer{.halted = *result};
    }

    inline control::Answer answer(control::Go const&,
                                  ControlTarget const& t) {
        if(!t.core) { return error("no target here"); }
        auto const result = t.core(false);
        if(!result) { return error(result.error()); }
        return control::GoAnswer{.halted = *result};
    }

    inline control::Answer answer(control::Registers const&,
                                  ControlTarget const& t) {
        if(!t.registers) { return error("no target here"); }
        auto result = t.registers();
        if(!result) { return error(result.error()); }
        return control::RegistersAnswer{.values = std::move(*result)};
    }

    inline control::Answer answer(control::WriteRegisters const& w,
                                  ControlTarget const&           t) {
        if(!t.writeRegisters) { return error("no target here"); }
        for(auto const& r : w.writes) {
            if(r.index > 18) {
                return error(fmt::format("write_registers: no register {} (0..18)", r.index));
            }
        }
        auto const result = t.writeRegisters(w.writes);
        if(!result) { return error(result.error()); }
        return control::WriteRegistersAnswer{};
    }

    inline control::Answer answer(control::Subscribe const&,
                                  ControlTarget const&) {
        return error("subscribe: only on a connection to the server");
    }
}   // namespace control_detail

/// `cancelled`: true once the client is gone, so a wait, reset or flash stops early.
inline control::Answer controlAnswer(control::Request const&      request,
                                     ControlTarget const&         target,
                                     std::function<bool()> const& cancelled = {}) {
    return std::visit(
      [&](auto const& r) -> control::Answer {
          if constexpr(requires { control_detail::answer(r, target, cancelled); }) {
              return control_detail::answer(r, target, cancelled);
          } else {
              return control_detail::answer(r, target);
          }
      },
      request);
}

inline std::string controlAnswerLine(std::string_view     line,
                                     ControlTarget const& target) {
    auto const parsed = control::parseRequest(line);
    if(auto const* e = std::get_if<control::Error>(&parsed)) {
        return control::toLine(control::Answer{*e});
    }
    return control::toLine(controlAnswer(std::get<control::Request>(parsed), target));
}

inline std::filesystem::path controlSocketPath(std::filesystem::path const& logDir) {
    return unixSocketPath(logDir, control::SocketName);
}

/// One socket thread (TcpListener/TcpSession assume that); requests run on a worker pool.
class ControlServer {
public:
    using ErrorF  = std::function<void(std::string_view)>;
    using StatusF = std::function<void(TcpPortStatus, std::uint16_t)>;

    static constexpr std::size_t SendQueueCap    = 4 * 1024 * 1024;
    static constexpr std::size_t MaxLineBytes    = 64 * 1024;
    static constexpr std::size_t MaxPendingBytes = 1024 * 1024;
    static constexpr std::size_t Workers         = 8;
    /// Stands in `pending` for a line that was too long (a client sending it just gets that error).
    static constexpr std::string_view TooLongMarker{"\x01line too long"};
    /// wait, reset and flash at once; the other workers stay free for the quick requests (a TCP
    /// client that dies in a wait is only noticed at its timeout).
    static constexpr std::size_t MaxLongRequests = Workers - 2;
    static constexpr std::size_t BacklogChunk    = 256 * 1024;
    static constexpr auto        BacklogRetry    = std::chrono::milliseconds{2};
    static constexpr auto        BacklogStall    = std::chrono::seconds{30};

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

    TcpPortStatus getStatus() const { return listener_.status.load(); }

    std::uint16_t getPort() const { return listener_.currentPort.load(); }

    std::filesystem::path socketPath() const { return listener_.getSocketPath(); }

    std::size_t getClientCount() const {
        std::lock_guard<std::mutex> const lock{clientsMutex_};
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
        std::lock_guard<std::mutex> const lock{clientsMutex_};
        auto const&                       text = history_.append(line);
        auto constexpr index                   = static_cast<std::size_t>(control::Stream::log);
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
        std::lock_guard<std::mutex> const lock{clientsMutex_};
        return history_.nextSeq();
    }

    void setHistoryLimit(std::size_t bytes) {
        std::lock_guard<std::mutex> const lock{clientsMutex_};
        history_.setLimit(bytes);
    }

    void publish(control::StatusMessage const& message) { publishEvent(message); }

    void publish(control::MetricSample const& sample) { publishEvent(sample); }

private:
    struct Client {
        std::shared_ptr<TcpSession> session;
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
        bool hungUp() const {
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
    ErrorF      errorf_;
    StatusF     statusf_;
    TcpListener listener_;

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

    std::array<Clock::Thread, Workers> pool_;
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
                    std::lock_guard<std::mutex> const lock{runningMutex_};
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
                              Workers,
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
            std::lock_guard<std::mutex> const lock{server.runningMutex_};
            at = server.running_.insert(server.running_.end(), Running{cmd, Clock::now()});
        }

        RunningEntry(RunningEntry const&)            = delete;
        RunningEntry& operator=(RunningEntry const&) = delete;

        ~RunningEntry() {
            std::lock_guard<std::mutex> const lock{server.runningMutex_};
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
        std::string                       line;
        std::lock_guard<std::mutex> const lock{clientsMutex_};
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
        client->session                    = std::make_shared<TcpSession>(
          std::move(socket),
          SendQueueCap,
          [this](std::string_view msg) {
              if(errorf_) { errorf_(fmt::format("control client {}", msg)); }
          },
          [this, weak](std::span<std::byte const> data, std::shared_ptr<TcpSession> const&) {
              auto const c = weak.lock();
              if(c) { onData(c, data); }
              return c != nullptr;
          },
          [this, weak](std::shared_ptr<TcpSession> const&) {
              if(auto const c = weak.lock()) { remove(c); }
          });
        client->session->onEof = [this, weak](std::shared_ptr<TcpSession> const&) {
            if(auto const c = weak.lock()) { onEof(c); }
        };
        {
            std::lock_guard<std::mutex> const lock{clientsMutex_};
            clients_.push_back(client);
        }
        client->session->startRead();
    }

    // socket thread
    void onData(std::shared_ptr<Client> const& client,
                std::span<std::byte const>     data) {
        if(client->streaming) { return; }
        std::lock_guard<std::mutex> const lock{client->mutex};
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
        if(tail > MaxLineBytes) {
            // answered in line with the requests before it, which a worker may still be serving
            client->pending.erase(newline == std::string::npos ? 0 : newline + 1);
            client->pending.append(TooLongMarker).push_back('\n');
            newline            = client->pending.size() - 1;
            client->discarding = true;
        }
        if(client->pending.size() > MaxPendingBytes) {
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
        if(line == TooLongMarker) {
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
                std::lock_guard<std::mutex> const lock{clientsMutex_};
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
        std::lock_guard<std::mutex> const lock{client->mutex};
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
                std::lock_guard<std::mutex> const lock{client->mutex};
                auto const                        end = client->pending.find('\n');
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
                std::lock_guard<std::mutex> const lock{client->mutex};
                client->pending.clear();
                client->busy = false;
                return;
            }
            auto const& request = *needed;
            bool const  isLong  = std::holds_alternative<control::Wait>(request)
                               || std::holds_alternative<control::Reset>(request)
                               || std::holds_alternative<control::Flash>(request);
            if(isLong && longRequests_.fetch_add(1) >= MaxLongRequests) {
                --longRequests_;
                send(*client,
                     control::toLine(control::Answer{control::Error{
                       .error = fmt::format("busy: {} waits, resets or flashes already run",
                                            MaxLongRequests)}}));
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
        std::lock_guard<std::mutex> const lock{clientsMutex_};
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
        if(request.since_seq || request.last) {
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
            std::lock_guard<std::mutex> const lock{clientsMutex_};
            if(client->gone || std::ranges::find(clients_, client) == clients_.end()) { return; }
            auto const queued = client->session->queuedBytes.load();
            if(queued + BacklogChunk <= SendQueueCap) {
                auto chunk = history_.read(client->filter, client->cursor, BacklogChunk);
                if(chunk.atEnd) {
                    chunk.text += control::toLine(control::Event{
                      control::BacklogEnd{.next_seq = chunk.next,
                                          .lost     = client->lost + chunk.lost}
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
        if(Clock::now() - client->backlogProgress > BacklogStall) {
            if(errorf_) { errorf_("control client does not read its history: closed"); }
            remove(client);
            client->session->close();
            return;
        }
        auto timer = std::make_shared<ClockTimer>(ioc_, BacklogRetry);
        timer->async_wait([this, client, timer](boost::system::error_code ec) {
            if(!ec) { backlogStep(client); }
        });
    }

    // socket thread
    void remove(std::shared_ptr<Client> const& client) {
        client->gone = true;
        std::lock_guard<std::mutex> const lock{clientsMutex_};
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
            std::lock_guard<std::mutex> const lock{clientsMutex_};
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

}   // namespace uc_log::detail
