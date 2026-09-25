// Hammer test for TcpServerCommon / ControlServer / DuplexChannelServer. Run under TSan and ASan.
#include "uc_log/detail/ControlServer.hpp"
#include "uc_log/detail/DuplexChannelServer.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

static std::atomic<int>  failures{0};
static std::atomic<bool> quiet{false};

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if(!(cond)) {                                                   \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++failures;                                                 \
        }                                                               \
    } while(0)

static void note(char const* msg) {
    if(!quiet) { std::printf("-- %s\n", msg); }
}

template<typename Socket>
struct LineClient {
    Socket                 socket;
    boost::asio::streambuf buffer;

    explicit LineClient(boost::asio::io_context& io) : socket{io} {}

    void send(std::string_view line) { boost::asio::write(socket, boost::asio::buffer(line)); }

    std::string next(std::chrono::milliseconds timeout = 10s) {
        auto& io = static_cast<boost::asio::io_context&>(socket.get_executor().context());
        boost::system::error_code ec = boost::asio::error::would_block;
        boost::asio::async_read_until(socket, buffer, '\n', [&](auto e, std::size_t) { ec = e; });
        io.restart();
        io.run_for(timeout);
        if(ec == boost::asio::error::would_block) {
            socket.cancel();
            io.restart();
            io.run();
            std::printf("FAIL: no line within %lld ms\n", static_cast<long long>(timeout.count()));
            ++failures;
            return {};
        }
        if(ec) { return {}; }
        std::string line;
        std::getline(std::istream{&buffer}, line);
        return line;
    }

    std::string ask(std::string_view line) {
        send(line);
        return next();
    }
};

static uc_log::control::Answer answerOf(std::string const& line) {
    auto const parsed = uc_log::control::fromLine<uc_log::control::Answer>(line);
    if(auto const* a = std::get_if<uc_log::control::Answer>(&parsed)) { return *a; }
    return uc_log::control::Error{.error = "not an answer: " + line};
}

static bool waitUntil(auto const& condition) {
    for(int i = 0; i != 500 && !condition(); ++i) { std::this_thread::sleep_for(10ms); }
    return condition();
}

static std::uint16_t basePort() { return static_cast<std::uint16_t>(41000 + (getpid() % 2000)); }

// the control server on its sockets: requests, a stream beside them, unix sockets
static void testControlServer(boost::asio::ip::address const&              loopback,
                              std::uint16_t                                portA,
                              uc_log::detail::ControlServer::ErrorF const& errf) {
    note("control server: requests and a stream side by side");
    namespace ctl = uc_log::control;
    std::array<std::atomic<int>, 256> readsAt{};   // tells that a wait has started
    auto const                        reader
      = [&readsAt](std::span<ctl::Piece const> pieces) -> uc_log::detail::MemoryResult {
        std::vector<std::vector<std::byte>> out;
        for(auto const& p : pieces) {
            if(p.address < readsAt.size()) { ++readsAt[p.address]; }
            out.emplace_back(p.size);
            for(std::uint32_t i = 0; i != p.size; ++i) {
                out.back()[i] = static_cast<std::byte>((p.address + i) & 0xFFU);
            }
        }
        return out;
    };
    auto const readLine = std::string{R"({"cmd":"read","pieces":[{"address":536870928,"size":2}]})"
                                      "\n"};
    auto const portC    = static_cast<std::uint16_t>(portA + 3);
    uc_log::detail::ControlServer server{loopback, portC, {}, errf, {}};
    for(int i = 0; i != 200 && server.getStatus() != TcpPortStatus::Active; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(server.getStatus() == TcpPortStatus::Active, "control server bound");

    boost::asio::io_context clientIo;
    LineClient<tcp::socket> client{clientIo};
    client.socket.connect(tcp::endpoint{loopback, portC});
    CHECK(client.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":1})",
          "ping answers without a target");
    auto const early = answerOf(client.ask(readLine));
    CHECK(std::holds_alternative<ctl::Error>(early), "no target yet: an error, not a hang");

    server.setTarget(uc_log::detail::ControlTarget{.read = reader});
    for(int i = 0; i != 3; ++i) {
        auto const  a = answerOf(client.ask(readLine));
        auto const* r = std::get_if<ctl::ReadAnswer>(&a);
        CHECK(r != nullptr && r->data == std::vector<std::string>{"1011"},
              "answer over the socket");
    }
    auto const bad = answerOf(client.ask("read 20000010:2\n"));
    CHECK(std::holds_alternative<ctl::Error>(bad), "the old line protocol is an error");
    CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(client.ask(readLine))),
          "and the connection goes on after it");

    LineClient<tcp::socket> subscriber{clientIo};
    subscriber.socket.connect(tcp::endpoint{loopback, portC});
    CHECK(
      subscriber.ask(R"({"cmd":"subscribe","streams":["log"],"min_level":"warn","modules":["i2c"]})"
                     "\n")
        == R"({"cmd":"subscribe","protocol":1})",
      "subscribe answers first");
    std::jthread publisher{[&] {
        for(int i = 0; i != 300; ++i) {
            server.publish(ctl::LogLine{.recv_time = "t",
                                        .channel   = 0,
                                        .file      = "f.cpp",
                                        .line      = 1,
                                        .function  = "f",
                                        .level = i % 3 == 0 ? ctl::Level::warn : ctl::Level::info,
                                        .uc_time_ns = i,
                                        .message    = std::to_string(i),
                                        .module     = i % 2 == 1 ? "i2c.bus" : "usb"});
            server.publish(
              ctl::MetricSample{.name = "m", .scope = "", .unit = "", .time = 0, .value = 0});
        }
    }};
    bool         onlyAnswers = true;
    for(int i = 0; i != 100; ++i) {
        onlyAnswers
          = onlyAnswers && std::holds_alternative<ctl::ReadAnswer>(answerOf(client.ask(readLine)));
    }
    CHECK(onlyAnswers, "a request connection gets answers only, whatever is streamed");
    publisher.join();
    // warn and under i2c: i = 3, 9, 15 ... 297, the metrics not asked for
    bool inOrder = true;
    for(int i = 3; i < 300; i += 6) {
        auto const  parsed = ctl::fromLine<ctl::Event>(subscriber.next());
        auto const* event  = std::get_if<ctl::Event>(&parsed);
        auto const* line   = event != nullptr ? std::get_if<ctl::LogLine>(event) : nullptr;
        inOrder            = inOrder && line != nullptr && line->message == std::to_string(i);
    }
    CHECK(inOrder, "the stream: the lines through the filter, in order, nothing else");

    LineClient<tcp::socket> waiter{clientIo};
    waiter.socket.connect(tcp::endpoint{loopback, portC});
    waiter.send(
      R"({"cmd":"wait","piece":{"address":16,"size":4},"condition":"changed","timeout_ms":1500})"
      "\n");
    CHECK(waitUntil([&] { return readsAt[16] > 0; }), "the wait has started");
    auto const start = std::chrono::steady_clock::now();
    for(int i = 0; i != 10; ++i) { (void)client.ask(readLine); }
    CHECK(std::chrono::steady_clock::now() - start < 1s, "reads go on while a wait waits");
    auto const  waited = answerOf(waiter.next());
    auto const* w      = std::get_if<ctl::WaitAnswer>(&waited);
    CHECK(w != nullptr && !w->hit && w->waited_us >= 1'500'000, "the wait runs out");

    {
        std::atomic<int> reads{0};
        auto const instant = [&reads](std::span<ctl::Piece const>) -> uc_log::detail::MemoryResult {
            ++reads;
            return std::vector<std::vector<std::byte>>{std::vector<std::byte>(4)};
        };
        auto const a = uc_log::detail::controlAnswer(
          ctl::Wait{
            .piece      = {.address = 0, .size = 4},
            .condition  = ctl::Condition::ne,
            .value      = 0,
            .mask       = std::nullopt,
            .timeout_ms = 200
        },
          uc_log::detail::ControlTarget{.read = instant});
        auto const* paced = std::get_if<ctl::WaitAnswer>(&a);
        CHECK(paced != nullptr && !paced->hit, "a wait on an instant reader runs out");
        CHECK(reads.load() >= 50 && reads.load() < 400,
              "and reads about once a millisecond, not as fast as it can");

        auto const empty = [](std::span<ctl::Piece const>) -> uc_log::detail::MemoryResult {
            return std::vector<std::vector<std::byte>>{};
        };
        auto const e = uc_log::detail::controlAnswer(
          ctl::Wait{
            .piece      = {.address = 0, .size = 4},
            .condition  = ctl::Condition::changed,
            .value      = std::nullopt,
            .mask       = std::nullopt,
            .timeout_ms = 200
        },
          uc_log::detail::ControlTarget{.read = empty});
        CHECK(std::holds_alternative<ctl::Error>(e), "a read that returns nothing is an error");
    }

    // `echo ... | socat` half-closes right after its line
    {
        LineClient<tcp::socket> oneShot{clientIo};
        oneShot.socket.connect(tcp::endpoint{loopback, portC});
        oneShot.send(readLine);
        oneShot.socket.shutdown(tcp::socket::shutdown_send);
        CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(oneShot.next())),
              "a half-closed TCP client gets its answer");
    }

    {
        LineClient<tcp::socket> longLine{clientIo};
        longLine.socket.connect(tcp::endpoint{loopback, portC});
        longLine.send("{\"cmd\":\"ping\"}\n" + std::string(70000, ' ') + "tail\n"
                      + "{\"cmd\":\"ping\"}\n");
        CHECK(std::holds_alternative<ctl::PingAnswer>(answerOf(longLine.next())),
              "the line before it is answered");
        CHECK(std::holds_alternative<ctl::Error>(answerOf(longLine.next())),
              "the long line: one error");
        CHECK(std::holds_alternative<ctl::PingAnswer>(answerOf(longLine.next())),
              "and the next answer is the next line's, not a second error for the rest");
    }

    // the rest of a long line arriving while the lines before it still wait for the worker
    // (held here by a wait): only the rest is dropped, not a queued line
    {
        LineClient<tcp::socket> held{clientIo};
        held.socket.connect(tcp::endpoint{loopback, portC});
        held.send(
          R"({"cmd":"wait","piece":{"address":20,"size":4},"condition":"changed","timeout_ms":500})"
          "\n{\"cmd\":\"ping\"}\n"
          + std::string(70000, ' '));
        CHECK(waitUntil([&] { return readsAt[20] > 0; }), "the held wait has started");
        std::this_thread::sleep_for(50ms);
        held.send(std::string(1000, ' ') + "tail\n{\"cmd\":\"ping\"}\n");
        CHECK(std::holds_alternative<ctl::WaitAnswer>(answerOf(held.next())),
              "the wait is answered");
        CHECK(std::holds_alternative<ctl::PingAnswer>(answerOf(held.next())),
              "the line queued before the long one is answered");
        CHECK(std::holds_alternative<ctl::Error>(answerOf(held.next())),
              "the long line queued behind it: one error");
        CHECK(std::holds_alternative<ctl::PingAnswer>(answerOf(held.next())),
              "and the line after it");
    }

    {
        LineClient<tcp::socket> piped{clientIo};
        piped.socket.connect(tcp::endpoint{loopback, portC});
        std::string lines;
        for(int i = 0; i != 50; ++i) { lines += "{\"cmd\":\"ping\"}\n" + readLine; }
        piped.send(lines);
        bool pipedInOrder = true;
        for(int i = 0; i != 50; ++i) {
            pipedInOrder = pipedInOrder
                        && std::holds_alternative<ctl::PingAnswer>(answerOf(piped.next()))
                        && std::holds_alternative<ctl::ReadAnswer>(answerOf(piped.next()));
        }
        CHECK(pipedInOrder, "100 pipelined requests, 100 answers in order");
    }

    server.clearTarget();
    CHECK(std::holds_alternative<ctl::Error>(answerOf(client.ask(readLine))),
          "a target taken away answers an error");

    namespace fs = std::filesystem;
    using local  = boost::asio::local::stream_protocol;
    auto const dir
      = fs::temp_directory_path() / ("uc_log_control_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    auto const path = uc_log::detail::controlSocketPath(dir);
    CHECK(path == dir / "control.sock", "the socket lies in the log directory");
    auto const deep = uc_log::detail::controlSocketPath(dir / std::string(120, 'x'));
    CHECK(deep.native().size() < 108 && deep.native().starts_with("/tmp/uc_log_"),
          "a directory too deep for sun_path gets a hashed name");
    CHECK(deep.native().ends_with(".control.sock"), "and keeps the socket's name");
    CHECK(uc_log::detail::duplexSocketName(0) == "duplex.0.sock",
          "a duplex channel's socket is named after its ordinal");

    auto const waitFor = [](auto const& condition) { return waitUntil(condition); };
    {
        std::ofstream{path} << "not a socket";
        {
            uc_log::detail::ControlServer notOurs{loopback, 0, path, errf, {}};
            CHECK(waitFor([&] { return notOurs.getStatus() == TcpPortStatus::PortOccupied; }),
                  "a path that is not a socket is refused");
        }
        std::string content;
        std::getline(std::ifstream{path}, content);
        CHECK(fs::is_regular_file(path) && content == "not a socket", "and the file is left alone");
        fs::remove(path);
        {   // what a killed printer leaves: a socket file nobody listens on
            local::acceptor stale{clientIo};
            stale.open();
            stale.bind(local::endpoint{path.native()});
        }
        CHECK(fs::is_socket(path), "a stale socket file");
        uc_log::detail::ControlServer unixServer{loopback, 0, path, errf, {}};
        unixServer.setTarget(uc_log::detail::ControlTarget{.read = reader});
        CHECK(waitFor([&] { return unixServer.getStatus() == TcpPortStatus::Active; }),
              "a stale file is replaced");
        CHECK(fs::is_socket(path), "the socket file is there");
        CHECK((fs::status(path).permissions() & (fs::perms::group_all | fs::perms::others_all))
                == fs::perms::none,
              "the socket is the owner's alone");
        {
            LineClient<local::socket> localClient{clientIo};
            localClient.socket.connect(local::endpoint{path.native()});
            CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(localClient.ask(readLine))),
                  "answer over the unix socket");
        }
        {
            LineClient<local::socket> oneShot{clientIo};
            oneShot.socket.connect(local::endpoint{path.native()});
            oneShot.send(readLine + readLine);
            oneShot.socket.shutdown(local::socket::shutdown_send);
            CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(oneShot.next()))
                    && std::holds_alternative<ctl::ReadAnswer>(answerOf(oneShot.next())),
                  "a half-closed unix client gets both answers");
            boost::system::error_code eofError;
            std::array<char, 16>      rest{};
            oneShot.socket.read_some(boost::asio::buffer(rest), eofError);
            CHECK(eofError == boost::asio::error::eof, "and then the server closes");
        }

        // nothing is streamed to it, so no failing send can notice the close
        {
            // the clients above leave asynchronously
            CHECK(waitFor([&] { return unixServer.getClientCount() == 0; }), "no clients left");
            auto const before = unixServer.getClientCount();
            {
                LineClient<local::socket> quietSubscriber{clientIo};
                quietSubscriber.socket.connect(local::endpoint{path.native()});
                CHECK(quietSubscriber.ask(R"({"cmd":"subscribe","streams":["metrics"]})"
                                          "\n")
                        == R"({"cmd":"subscribe","protocol":1})",
                      "a quiet subscriber");
                quietSubscriber.socket.shutdown(local::socket::shutdown_send);
                std::this_thread::sleep_for(100ms);
                CHECK(unixServer.getClientCount() == before + 1, "half-closed, still subscribed");
            }
            CHECK(waitFor([&] { return unixServer.getClientCount() == before; }),
                  "closed: the server lets it go");
        }

        // four waits occupy all four workers; unix sockets only, where a hang-up and a
        // half-close look different (on TCP such a wait runs to its timeout)
        {
            std::vector<std::unique_ptr<LineClient<local::socket>>> quitters;
            for(int i = 0; i != 4; ++i) {
                quitters.push_back(std::make_unique<LineClient<local::socket>>(clientIo));
                quitters.back()->socket.connect(local::endpoint{path.native()});
                quitters.back()->send(fmt::format(
                  R"({{"cmd":"wait","piece":{{"address":{},"size":4}},"condition":"changed","timeout_ms":10000}})"
                  "\n",
                  32 + 4 * i));
            }
            CHECK(waitUntil([&] {
                      return readsAt[32] > 0 && readsAt[36] > 0 && readsAt[40] > 0
                          && readsAt[44] > 0;
                  }),
                  "all four waits have started");
            quitters.clear();
            LineClient<local::socket> after{clientIo};
            after.socket.connect(local::endpoint{path.native()});
            auto const t0 = std::chrono::steady_clock::now();
            CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(after.ask(readLine))),
                  "answered after four abandoned waits");
            CHECK(std::chrono::steady_clock::now() - t0 < 2s,
                  "at once: the abandoned waits stopped when their clients left");
        }

        // long requests leave workers free: one past the limit is refused, a read still goes
        {
            constexpr auto limit = uc_log::detail::ControlServer::MaxLongRequests;
            CHECK(waitUntil([&] { return unixServer.requestsInFlight() == 0; }),
                  "the abandoned waits have ended");
            std::vector<std::unique_ptr<LineClient<local::socket>>> waiters;
            for(std::size_t i = 0; i != limit; ++i) {
                waiters.push_back(std::make_unique<LineClient<local::socket>>(clientIo));
                waiters.back()->socket.connect(local::endpoint{path.native()});
                waiters.back()->send(fmt::format(
                  R"({{"cmd":"wait","piece":{{"address":{},"size":4}},"condition":"changed","timeout_ms":10000}})"
                  "\n",
                  64 + 4 * i));
            }
            CHECK(waitUntil([&] {
                      for(std::size_t i = 0; i != limit; ++i) {
                          if(readsAt[64 + 4 * i] == 0) { return false; }
                      }
                      return true;
                  }),
                  "the limit's waits have started");
            LineClient<local::socket> extra{clientIo};
            extra.socket.connect(local::endpoint{path.native()});
            auto const refused = answerOf(extra.ask(
              R"({"cmd":"wait","piece":{"address":128,"size":4},"condition":"changed","timeout_ms":10000})"
              "\n"));
            CHECK(std::holds_alternative<ctl::Error>(refused)
                    && std::get<ctl::Error>(refused).error.starts_with("busy"),
                  "a wait past the limit is refused");
            CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(extra.ask(readLine))),
                  "a read is still answered");
            waiters.clear();
        }

        {
            uc_log::detail::ControlServer second{loopback, 0, path, errf, {}};
            CHECK(waitFor([&] { return second.getStatus() == TcpPortStatus::PortOccupied; }),
                  "a second server on the same path does not bind");
        }
        CHECK(fs::is_socket(path), "and leaves the first one's socket");

        uc_log::detail::AsioContext      unixCtx;
        uc_log::detail::DuplexChannelHub unixHub{unixCtx.ioc,
                                                 loopback,
                                                 0,
                                                 [](std::string_view) {},
                                                 [](std::string_view) {},
                                                 []() {},
                                                 dir};
        {
            uc_log::detail::AsioContextRunner unixRunner{unixCtx};
            unixHub.configure({
              uc_log::detail::DuplexChannelDesc{.ordinal   = 0,
                                                .name      = "control",
                                                .upIndex   = 1,
                                                .downIndex = 1}
            });
            auto const duplexPath = dir / "duplex.0.sock";
            CHECK(waitFor([&] { return fs::is_socket(duplexPath); }),
                  "duplex.0.sock in the log directory");
            CHECK(unixHub.info().size() == 1 && unixHub.info()[0].socketPath == duplexPath,
                  "the hub says where the channel is");

            local::socket duplexClient{clientIo};
            duplexClient.connect(local::endpoint{duplexPath.native()});
            std::string const down = "to the target";
            boost::asio::write(duplexClient, boost::asio::buffer(down));
            std::array<std::byte, 64> got{};
            std::size_t               n = 0;
            for(int i = 0; i != 200 && n != down.size(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                n = unixHub.peekFromClient(0, got);
            }
            CHECK(n == down.size(), "client -> target over the unix socket");
            std::string const up = "from the target";
            unixHub.sendToClient(0, std::as_bytes(std::span{up}));
            std::string back(up.size(), '\0');
            boost::asio::read(duplexClient, boost::asio::buffer(back));
            CHECK(back == up, "target -> client over the unix socket");

            unixServer.setSocketPath({});
            unixHub.setSocketDir({});
            CHECK(waitFor([&] { return !fs::exists(path) && !fs::exists(duplexPath); }),
                  "switched to TCP: the socket files are gone");
            CHECK(waitFor([&] {
                      return unixServer.getStatus() == TcpPortStatus::Active
                          && unixHub.info()[0].socketPath.empty();
                  }),
                  "and the servers listen on TCP");
            unixServer.setSocketPath(path);
            unixHub.setSocketDir(dir);
            CHECK(waitFor([&] { return fs::is_socket(path) && fs::is_socket(duplexPath); }),
                  "switched back: the socket files are there again");
            LineClient<local::socket> again{clientIo};
            again.socket.connect(local::endpoint{path.native()});
            CHECK(again.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":1})",
                  "and take a client");

            // a reflash puts a new channel at ordinal 0 and moves "control" to 1
            unixHub.configure({
              uc_log::detail::DuplexChannelDesc{.ordinal   = 0,
                                                .name      = "fresh",
                                                .upIndex   = 2,
                                                .downIndex = 2},
              uc_log::detail::DuplexChannelDesc{.ordinal   = 1,
                                                .name      = "control",
                                                .upIndex   = 1,
                                                .downIndex = 1}
            });
            CHECK(waitFor([&] {
                      auto const info = unixHub.info();
                      return info.size() == 2 && info[0].status == TcpPortStatus::Active
                          && info[1].status == TcpPortStatus::Active
                          && info[0].socketPath == (dir / "duplex.0.sock").native()
                          && info[1].socketPath == (dir / "duplex.1.sock").native();
                  }),
                  "a moved channel follows its ordinal, the new one takes the free socket");
        }
    }
    CHECK(!fs::exists(path) && !fs::exists(dir / "duplex.0.sock")
            && !fs::exists(dir / "duplex.1.sock"),
          "the socket files go with their servers");
    fs::remove_all(dir);
}

static void testLogHistory(boost::asio::ip::address const&              loopback,
                           uc_log::detail::ControlServer::ErrorF const& errf) {
    note("control server: log history");
    namespace fs  = std::filesystem;
    using local   = boost::asio::local::stream_protocol;
    namespace ctl = uc_log::control;
    auto const dir
      = fs::temp_directory_path() / ("uc_log_history_test_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    auto const                    path = uc_log::detail::controlSocketPath(dir);
    uc_log::detail::ControlServer server{loopback, 0, path, errf, {}};
    CHECK(waitUntil([&] { return server.getStatus() == TcpPortStatus::Active; }),
          "history server bound");
    auto const publish = [&](int i, std::size_t bytes = 0) {
        ctl::LogLine l;
        l.level   = i % 4 == 0 ? ctl::Level::warn : ctl::Level::info;
        l.message = std::to_string(i) + std::string(bytes, 'x');
        l.module  = i % 2 == 0 ? "i2c.bus" : "usb";
        server.publish(std::move(l));
    };

    struct Seen {
        std::vector<std::uint64_t>     seqs;
        std::vector<std::string>       messages;
        std::optional<ctl::BacklogEnd> end;
        std::size_t                    endAt{};   // lines before the backlog_end
        bool                           other{};
    };

    auto const take = [](Seen& seen, std::string const& text) {
        auto const  parsed = ctl::fromLine<ctl::Event>(text);
        auto const* event  = std::get_if<ctl::Event>(&parsed);
        if(auto const* l = event != nullptr ? std::get_if<ctl::LogLine>(event) : nullptr) {
            seen.seqs.push_back(l->seq);
            seen.messages.push_back(l->message.substr(0, l->message.find('x')));
        } else if(auto const* e = event != nullptr ? std::get_if<ctl::BacklogEnd>(event) : nullptr)
        {
            seen.other = seen.other || seen.end.has_value();
            seen.end   = *e;
            seen.endAt = seen.seqs.size();
        } else {
            seen.other = true;
        }
    };
    auto const contiguous = [](std::vector<std::uint64_t> const& seqs, std::uint64_t from) {
        for(std::size_t i = 0; i != seqs.size(); ++i) {
            if(seqs[i] != from + i) { return false; }
        }
        return true;
    };

    for(int i = 0; i != 100; ++i) { publish(i); }
    CHECK(server.logSeq() == 100, "lines are kept without any subscriber");

    boost::asio::io_context io;
    {
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        CHECK(
          answerOf(
            c.ask(
              R"({"cmd":"subscribe","streams":["log"],"min_level":"warn","last":3,"follow":false})"
              "\n"))
              .index()
            == ctl::Answer{ctl::SubscribeAnswer{}}.index(),
          "a history subscription is answered");
        Seen seen;
        for(int i = 0; i != 4; ++i) { take(seen, c.next()); }
        CHECK(seen.messages == (std::vector<std::string>{"88", "92", "96"}) && seen.end
                && seen.end->next_seq == 100 && seen.end->lost == 0 && !seen.other,
              "last 3 warn lines, then backlog_end");
        boost::system::error_code ec;
        boost::asio::read_until(c.socket, c.buffer, '\n', ec);
        CHECK(ec == boost::asio::error::eof, "follow false: closed after backlog_end");
    }
    {
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        auto const refused
          = answerOf(c.ask(R"({"cmd":"subscribe","streams":["metrics"],"since_seq":0})"
                           "\n"));
        CHECK(std::holds_alternative<ctl::Error>(refused), "history without log: an error");
        CHECK(c.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":1})",
              "and the connection still takes requests");
    }
    {
        // lines keep coming while the history goes out: every seq once, in order
        std::atomic<bool>         go{false};
        std::jthread              publisher{[&] {
            while(!go) { std::this_thread::yield(); }
            for(int i = 100; i != 20'100; ++i) { publish(i); }
        }};
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        c.send(R"({"cmd":"subscribe","streams":["log"],"since_seq":50})"
               "\n");
        go = true;
        CHECK(std::holds_alternative<ctl::SubscribeAnswer>(answerOf(c.next())), "subscribed");
        Seen seen;
        while(seen.seqs.size() + (seen.end ? 1 : 0) < 20'050 + 1 && failures.load() == 0) {
            take(seen, c.next());
        }
        publisher.join();
        CHECK(contiguous(seen.seqs, 50) && seen.seqs.size() == 20'050,
              "history then live: seq 50 .. 20099, none twice, none missing");
        CHECK(seen.end && seen.end->next_seq == 50 + seen.endAt && seen.end->lost == 0
                && !seen.other,
              "backlog_end sits exactly where the live lines begin");
        CHECK(server.droppedBytes() == 0, "nothing dropped on the way");
    }
    {
        // ~10 MB of history, more than the send queue holds, to a client that reads slowly
        auto const first = server.logSeq();
        for(int i = 0; i != 20'000; ++i) { publish(i, 400); }
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        c.send(R"({"cmd":"subscribe","streams":["log"],"since_seq":)" + std::to_string(first)
               + R"(,"follow":false})"
                 "\n");
        std::this_thread::sleep_for(200ms);   // let the send queue fill up
        CHECK(std::holds_alternative<ctl::SubscribeAnswer>(answerOf(c.next())), "subscribed");
        Seen seen;
        while(!seen.end && failures.load() == 0) { take(seen, c.next()); }
        CHECK(contiguous(seen.seqs, first) && seen.seqs.size() == 20'000 && seen.end
                && seen.end->next_seq == first + 20'000,
              "a history larger than the send queue arrives whole");
        CHECK(server.droppedBytes() == 0, "paced by the client, not dropped");
    }
    {
        server.setHistoryLimit(64 * 1024);
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        c.send(R"({"cmd":"subscribe","streams":["log"],"since_seq":0,"follow":false})"
               "\n");
        CHECK(std::holds_alternative<ctl::SubscribeAnswer>(answerOf(c.next())), "subscribed");
        Seen seen;
        while(!seen.end && failures.load() == 0) { take(seen, c.next()); }
        CHECK(seen.end && !seen.seqs.empty() && seen.end->lost == seen.seqs.front()
                && seen.end->lost > 0 && seen.end->next_seq == server.logSeq(),
              "lines dropped past the limit are counted as lost");
    }
    fs::remove_all(dir);
}

int main() {
    auto const errf = [](std::string_view msg) {
        if(!quiet) { std::printf("   [err] %.*s\n", static_cast<int>(msg.size()), msg.data()); }
    };
    auto const msgf = [](std::string_view msg) {
        if(!quiet) { std::printf("   [msg] %.*s\n", static_cast<int>(msg.size()), msg.data()); }
    };

    std::uint16_t const portA      = basePort();
    std::uint16_t const portA2     = static_cast<std::uint16_t>(portA + 1);
    std::uint16_t const duplexBase = static_cast<std::uint16_t>(portA + 10);

    auto const loopback = boost::asio::ip::make_address("127.0.0.1");

    uc_log::detail::AsioContext ctx;

    uc_log::detail::ControlServer control{loopback, portA, {}, errf, {}};

    uc_log::detail::DuplexChannelHub hub{ctx.ioc, loopback, duplexBase, errf, msgf, []() {}};

    uc_log::detail::AsioContextRunner runner{ctx};

    std::this_thread::sleep_for(100ms);
    CHECK(control.getStatus() == TcpPortStatus::Active, "control port bound");

    std::string const subscribeMetrics = R"({"cmd":"subscribe","streams":["metrics"]})"
                                         "\n";

    // ---- part A: the control server's metrics stream ----------------------------
    note("A: producer + client churn");
    std::atomic<bool>                   produce{true};
    uc_log::control::MetricSample const bigSample{.name  = std::string(4096, 'x'),
                                                  .scope = "test",
                                                  .unit  = "",
                                                  .time  = 1.0,
                                                  .value = 2.0};
    std::jthread                        producer{[&]() {
        while(produce) {
            control.publish(bigSample);
            std::this_thread::sleep_for(50us);
        }
    }};

    {
        std::atomic<int>          connected{0};
        std::vector<std::jthread> churners;
        for(int t = 0; t < 4; ++t) {
            churners.emplace_back([&, t]() {
                for(int i = 0; i < 25; ++i) {
                    try {
                        boost::asio::io_context cioc;
                        tcp::socket             s{cioc};
                        s.connect(tcp::endpoint{loopback, portA});
                        boost::asio::write(s, boost::asio::buffer(subscribeMetrics));
                        ++connected;
                        std::array<char, 8192>    buf;
                        boost::system::error_code ec;
                        for(int r = 0; r < 4 + (t + i) % 5; ++r) {
                            s.read_some(boost::asio::buffer(buf), ec);
                            if(ec) { break; }
                        }
                        s.close(ec);
                    } catch(std::exception const&) {}
                    std::this_thread::sleep_for(1ms);
                }
            });
        }
        churners.clear();   // join
        CHECK(connected.load() > 50, "churn clients connected");
    }

    note("A: stalled client backpressure");
    {
        boost::asio::io_context cioc;
        tcp::socket             stalled{cioc};
        stalled.connect(tcp::endpoint{loopback, portA});
        boost::asio::write(stalled, boost::asio::buffer(subscribeMetrics));
        // never read: the 4 MiB send queue must cap and drops must be counted
        auto const deadline = std::chrono::steady_clock::now() + 8s;
        while(control.droppedBytes() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        CHECK(control.droppedBytes() > 0, "stalled client dropped data instead of growing queue");
        boost::system::error_code ec;
        stalled.close(ec);
    }

    note("A: restart mid-traffic");
    control.restart(portA2);
    std::this_thread::sleep_for(200ms);
    CHECK(control.getStatus() == TcpPortStatus::Active, "rebound after restart");
    CHECK(control.getPort() == portA2, "port updated");
    {
        boost::asio::io_context   cioc;
        tcp::socket               s{cioc};
        boost::system::error_code ec;
        s.connect(tcp::endpoint{loopback, portA2}, ec);
        CHECK(!ec, "client connects on new port");
        s.close(ec);
    }

    note("A: bind-address change mid-traffic");
    {
        auto const anyAddr = boost::asio::ip::make_address("0.0.0.0");
        control.setBindAddress(anyAddr);
        std::this_thread::sleep_for(200ms);
        CHECK(control.getStatus() == TcpPortStatus::Active, "rebound on new address");
        {
            boost::asio::io_context   cioc;
            tcp::socket               s{cioc};
            boost::system::error_code ec;
            s.connect(tcp::endpoint{loopback, portA2},
                      ec);   // 0.0.0.0 still reachable via loopback
            CHECK(!ec, "client connects after address change");
            s.close(ec);
        }
        control.setBindAddress(loopback);
        std::this_thread::sleep_for(200ms);
        CHECK(control.getStatus() == TcpPortStatus::Active, "rebound back to loopback");
    }

    produce = false;
    producer.join();

    control.stop();
    std::this_thread::sleep_for(200ms);
    CHECK(control.getStatus() == TcpPortStatus::NotStarted, "stopped");
    CHECK(control.getClientCount() == 0, "sessions closed on stop");

    // ---- part B: duplex ----------------------------------------------------------
    note("B: duplex configure + echo integrity");
    hub.configure({
      uc_log::detail::DuplexChannelDesc{0, "shell", std::uint32_t{2}, 0},
      uc_log::detail::DuplexChannelDesc{1,   "raw",     std::nullopt, 1},
    });
    std::this_thread::sleep_for(100ms);
    {
        auto const infos = hub.info();
        CHECK(infos.size() == 2, "two duplex channels");
        CHECK(infos.size() == 2 && infos[0].status == TcpPortStatus::Active, "shell bound");
        CHECK(infos.size() == 2 && infos[1].hostToTargetOnly, "raw is host->target only");
    }

    // pump thread: acts as the RTT reader, echoes client bytes back to the client
    std::atomic<bool> pump{true};
    std::jthread      pumpThread{[&]() {
        std::array<std::byte, 4096> chunk;
        while(pump) {
            auto const n = hub.peekFromClient(0, chunk);
            if(n != 0) {
                hub.sendToClient(0, std::span{chunk}.first(n));
                hub.consumeFromClient(0, n);
            } else {
                std::this_thread::sleep_for(200us);
            }
        }
    }};

    {
        boost::asio::io_context cioc;
        tcp::socket             client{cioc};
        client.connect(tcp::endpoint{loopback, duplexBase});
        std::this_thread::sleep_for(100ms);

        // a second client must be rejected (first wins)
        {
            tcp::socket second{cioc};
            second.connect(tcp::endpoint{loopback, duplexBase});
            std::array<char, 64>      buf;
            boost::system::error_code ec;
            second.read_some(boost::asio::buffer(buf), ec);   // must eof quickly
            CHECK(ec == boost::asio::error::eof, "second client rejected with close");
        }

        // echo integrity: 1 MiB patterned data through recvQueue (64k cap => backpressure path)
        constexpr std::size_t Total = 1024 * 1024;
        std::vector<char>     tx(Total);
        for(std::size_t i = 0; i < Total; ++i) { tx[i] = static_cast<char>((i * 7 + 13) & 0xFF); }
        std::jthread      writer{[&]() {
            std::size_t off = 0;
            while(off < Total) {
                auto const chunk
                  = std::span{tx}.subspan(off, std::min<std::size_t>(32768, Total - off));
                auto const n = client.write_some(boost::asio::buffer(chunk.data(), chunk.size()));
                off += n;
            }
        }};
        std::vector<char> rx;
        rx.reserve(Total);
        std::array<char, 65536> buf;
        while(rx.size() < Total) {
            auto const n        = client.read_some(boost::asio::buffer(buf));
            auto const received = std::span{buf}.first(n);
            rx.insert(rx.end(), received.begin(), received.end());
        }
        writer.join();
        CHECK(rx.size() == Total, "echoed byte count");
        CHECK(std::equal(rx.begin(), rx.end(), tx.begin()), "echoed bytes identical");

        auto const infos = hub.info();
        CHECK(infos[0].bytesToTarget >= Total, "bytesToTarget counted");
        CHECK(infos[0].bytesFromTarget >= Total, "bytesFromTarget counted");

        boost::system::error_code ec;
        client.shutdown(tcp::socket::shutdown_both, ec);
        client.close(ec);
    }
    std::this_thread::sleep_for(200ms);
    CHECK(!hub.info()[0].connected, "client gone detected");

    note("B: port churn while pumping");
    for(int i = 0; i < 10; ++i) {
        hub.setPort(0, static_cast<std::uint16_t>(duplexBase + 20 + (i % 2)));
        hub.setEnabled(0, i % 2 == 0);
        std::this_thread::sleep_for(20ms);
    }
    hub.setEnabled(0, true);
    hub.setPort(0, duplexBase);
    std::this_thread::sleep_for(200ms);
    CHECK(hub.info()[0].status == TcpPortStatus::Active, "active after port churn");

    note("B: bind-address change on the whole hub");
    hub.setBindAddress(boost::asio::ip::make_address("0.0.0.0"));
    std::this_thread::sleep_for(200ms);
    CHECK(hub.info()[0].status == TcpPortStatus::Active, "active after hub address change");
    hub.setBindAddress(loopback);
    std::this_thread::sleep_for(200ms);
    CHECK(hub.info()[0].status == TcpPortStatus::Active, "active back on loopback");
    {
        boost::asio::io_context   cioc;
        tcp::socket               client{cioc};
        boost::system::error_code ec;
        client.connect(tcp::endpoint{loopback, duplexBase}, ec);
        CHECK(!ec, "duplex reachable after address round-trip");
        client.close(ec);
    }

    // reconfigure churn (reader reconnect path) while a client is attached
    {
        boost::asio::io_context cioc;
        tcp::socket             client{cioc};
        client.connect(tcp::endpoint{loopback, duplexBase});
        for(int i = 0; i < 5; ++i) {
            hub.configure({
              uc_log::detail::DuplexChannelDesc{0, "shell", std::uint32_t{2}, 0},
              uc_log::detail::DuplexChannelDesc{1,   "raw",     std::nullopt, 1},
            });
            std::this_thread::sleep_for(20ms);
        }
        boost::system::error_code ec;
        client.close(ec);
    }

    pump = false;
    pumpThread.join();

    testControlServer(loopback, portA, errf);
    testLogHistory(loopback, errf);

    if(failures.load() == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures.load());
    return 1;
}
