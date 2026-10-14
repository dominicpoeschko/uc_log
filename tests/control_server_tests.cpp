// ControlServer's behaviour over unix sockets on a virtual clock (Clock.hpp): requests beside streams, waits and
// their limits, a subscriber that never reads, stuck reads and a stuck socket thread (the self-check), the log
// history. Every wait is the printer's own time - no check depends on how fast the machine runs. What needs TCP is in
// tcp_server_tests. Run under TSan and ASan.
#include "SocketTestSupport.hpp"
#include "uc_log/detail/ControlServer.hpp"
#include "uc_log/detail/DuplexChannelServer.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

using local = boost::asio::local::stream_protocol;

static void testControlServer(boost::asio::ip::address const&              loopback,
                              std::filesystem::path const&                 baseDir,
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
    auto const requestsPath = baseDir / "requests.sock";
    local::endpoint const         requests{requestsPath.native()};
    uc_log::detail::ControlServer server{loopback, 0, requestsPath, errf, {}};
    for(int i = 0; i != 200 && server.getStatus() != TcpPortStatus::Active; ++i) {
        Clock::sleep_for(10ms);
    }
    CHECK(server.getStatus() == TcpPortStatus::Active, "control server bound");

    boost::asio::io_context   clientIo;
    LineClient<local::socket> client{clientIo};
    client.socket.connect(requests);
    CHECK(client.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":2})",
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

    LineClient<local::socket> subscriber{clientIo};
    subscriber.socket.connect(requests);
    CHECK(
      subscriber.ask(R"({"cmd":"subscribe","streams":["log"],"min_level":"warn","modules":["i2c"]})"
                     "\n")
        == R"({"cmd":"subscribe","protocol":2})",
      "subscribe answers first");
    Clock::Thread publisher{[&] {
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
    bool          onlyAnswers = true;
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

    LineClient<local::socket> waiter{clientIo};
    waiter.socket.connect(requests);
    waiter.send(
      R"({"cmd":"wait","piece":{"address":16,"size":4},"condition":"changed","timeout_ms":1500})"
      "\n");
    CHECK(waitUntil([&] { return readsAt[16] > 0; }), "the wait has started");
    auto const start = Clock::now();
    for(int i = 0; i != 10; ++i) { (void)client.ask(readLine); }
    CHECK(Clock::now() - start < 1s, "reads go on while a wait waits");
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

    {
        LineClient<local::socket> longLine{clientIo};
        longLine.socket.connect(requests);
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
        LineClient<local::socket> held{clientIo};
        held.socket.connect(requests);
        held.send(
          R"({"cmd":"wait","piece":{"address":20,"size":4},"condition":"changed","timeout_ms":500})"
          "\n{\"cmd\":\"ping\"}\n"
          + std::string(70000, ' '));
        CHECK(waitUntil([&] { return readsAt[20] > 0; }), "the held wait has started");
        Clock::sleep_for(50ms);
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
        LineClient<local::socket> piped{clientIo};
        piped.socket.connect(requests);
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
            readSome(oneShot.socket, boost::asio::buffer(rest), eofError, Clock::now() + 10s);
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
                        == R"({"cmd":"subscribe","protocol":2})",
                      "a quiet subscriber");
                quietSubscriber.socket.shutdown(local::socket::shutdown_send);
                Clock::sleep_for(100ms);
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
            auto const t0 = Clock::now();
            CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(after.ask(readLine))),
                  "answered after four abandoned waits");
            CHECK(Clock::now() - t0 < 2s,
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
                Clock::sleep_for(std::chrono::milliseconds{10});
                n = unixHub.peekFromClient(0, got);
            }
            CHECK(n == down.size(), "client -> target over the unix socket");
            std::string const up = "from the target";
            unixHub.sendToClient(0, std::as_bytes(std::span{up}));
            std::string back(up.size(), '\0');
            for(std::size_t have = 0; have != back.size();) {
                boost::system::error_code ec;
                auto const                read = readSome(duplexClient,
                                                          boost::asio::buffer(back) + have,
                                                          ec,
                                                          Clock::now() + 10s);
                if(ec) { break; }
                have += read;
            }
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
            CHECK(again.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":2})",
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

// a subscriber that never reads must cost the others nothing: its queue fills and drops, the
// socket thread and the workers go on serving the other clients
static void testStalledSubscriber(boost::asio::ip::address const&              loopback,
                                  std::filesystem::path const&                 path,
                                  uc_log::detail::ControlServer::ErrorF const& errf) {
    note("control server: a subscriber that never reads");
    namespace ctl     = uc_log::control;
    auto const reader = [](std::span<ctl::Piece const> pieces) -> uc_log::detail::MemoryResult {
        std::vector<std::vector<std::byte>> out;
        for(auto const& p : pieces) { out.emplace_back(p.size); }
        return out;
    };
    uc_log::detail::ControlServer server{loopback, 0, path, errf, {}};
    for(int i = 0; i != 200 && server.getStatus() != TcpPortStatus::Active; ++i) {
        Clock::sleep_for(10ms);
    }
    CHECK(server.getStatus() == TcpPortStatus::Active, "control server bound");
    server.setTarget(uc_log::detail::ControlTarget{.read = reader});

    auto const subscribeLine = std::string{R"({"cmd":"subscribe","streams":["log"]})"
                                           "\n"};
    auto const readLine      = std::string{R"({"cmd":"read","pieces":[{"address":16,"size":4}]})"
                                           "\n"};
    boost::asio::io_context   clientIo;
    LineClient<local::socket> stalled{clientIo};
    stalled.socket.connect(local::endpoint{path.native()});
    // a small receive buffer, so the kernel takes little and the server's queue fills
    stalled.socket.set_option(boost::asio::socket_base::receive_buffer_size{4096});
    CHECK(stalled.ask(subscribeLine) == R"({"cmd":"subscribe","protocol":2})",
          "the stalled one subscribes, then never reads again");

    // well past what the kernel buffers and the 4 MiB queue together hold
    auto const message = std::string(200, 'x');
    auto const flood   = [&](int from, int to) {
        for(int i = from; i != to; ++i) {
            server.publish(ctl::LogLine{.recv_time  = "t",
                                        .channel    = 0,
                                        .file       = "f.cpp",
                                        .line       = 1,
                                        .function   = "f",
                                        .level      = ctl::Level::info,
                                        .uc_time_ns = i,
                                        .message    = message,
                                        .module     = ""});
        }
    };
    std::atomic<bool> flooded{false};
    Clock::Thread     flooder{[&] {
        flood(0, 60'000);
        flooded = true;
    }};
    auto const        floodStart = Clock::now();
    while(!flooded && Clock::now() - floodStart < 5s) { Clock::sleep_for(10ms); }
    CHECK(flooded, "publishing does not wait for the stalled subscriber");
    if(!flooded) {
        stalled.socket.close();   // lets a blocked publish go, so the test ends
        flooder.join();
        return;
    }
    flooder.join();
    CHECK(waitUntil([&] { return server.droppedBytes() > 0; }),
          "its queue is full: lines for it are dropped");

    LineClient<local::socket> other{clientIo};
    other.socket.connect(local::endpoint{path.native()});
    auto const start = Clock::now();
    CHECK(other.ask(subscribeLine) == R"({"cmd":"subscribe","protocol":2})",
          "a second client still subscribes");
    Clock::Thread publisher{[&] { flood(60'000, 60'100); }};
    publisher.join();
    bool got = true;
    for(int i = 60'000; i != 60'100; ++i) {
        auto const  parsed = ctl::fromLine<ctl::Event>(other.next(2s));
        auto const* event  = std::get_if<ctl::Event>(&parsed);
        auto const* line   = event != nullptr ? std::get_if<ctl::LogLine>(event) : nullptr;
        got                = got && line != nullptr && line->uc_time_ns == i;
    }
    CHECK(got, "and gets every line published after it, in order");

    LineClient<local::socket> asker{clientIo};
    asker.socket.connect(local::endpoint{path.native()});
    bool answered = true;
    for(int i = 0; i != 20; ++i) {
        answered
          = answered && std::holds_alternative<ctl::ReadAnswer>(answerOf(asker.ask(readLine)));
    }
    CHECK(answered, "memory requests are answered beside it");
    CHECK(Clock::now() - start < 3s, "all of that within bounds");

    // the stalled one reads again: what it got, then the gap, counted exactly
    // (a gap can be reported in parts: the queue may have had room for a marker during the flood)
    std::uint64_t received = 0;
    std::uint64_t dropped  = 0;
    bool          marker   = false;
    while(received + dropped < 60'100) {
        auto const line = stalled.next(3s);
        if(line.empty()) { break; }
        auto const  parsed = ctl::fromLine<ctl::Event>(line);
        auto const* event  = std::get_if<ctl::Event>(&parsed);
        if(event == nullptr) { break; }
        if(std::holds_alternative<ctl::LogLine>(*event)) { ++received; }
        if(auto const* d = std::get_if<ctl::Dropped>(event)) {
            dropped += d->lines;
            marker = true;
        }
    }
    CHECK(marker && dropped > 0, "the slow client gets a dropped event once its queue drains");
    CHECK(received + dropped == 60'100, "with the exact count: received + dropped = published");
    // nothing more: no second marker for the same gap
    Clock::sleep_for(1500ms);
    CHECK(stalled.socket.available() == 0 && stalled.pending.empty(), "and nothing after it");
}

// reads stuck in the target (a probe call that does not come back) must not take ping and
// subscribe with them: they need no target
static void testStuckReads(boost::asio::ip::address const& loopback,
                           std::filesystem::path const&    path) {
    note("control server: every worker stuck in a read");
    namespace ctl = uc_log::control;
    std::mutex               mutex;
    Clock::ConditionVariable release;
    bool                     released = false;
    std::atomic<int>         inside{0};
    auto const reader = [&](std::span<ctl::Piece const> pieces) -> uc_log::detail::MemoryResult {
        ++inside;
        std::unique_lock<std::mutex> lock{mutex};
        release.wait(lock, [&] { return released; });
        std::vector<std::vector<std::byte>> out;
        for(auto const& p : pieces) { out.emplace_back(p.size); }
        return out;
    };
    std::mutex               messagesMutex;
    std::vector<std::string> messages;
    auto const               said = [&](std::string_view part) {
        std::lock_guard<std::mutex> const lock{messagesMutex};
        return std::ranges::any_of(messages, [&](auto const& m) {
            return m.find(part) != std::string::npos;
        });
    };
    uc_log::detail::ControlServer server{loopback, 0, path, [&](std::string_view msg) {
                                             std::lock_guard<std::mutex> const lock{messagesMutex};
                                             messages.emplace_back(msg);
                                         }};
    server.setStuckLimit(300ms);
    for(int i = 0; i != 200 && server.getStatus() != TcpPortStatus::Active; ++i) {
        Clock::sleep_for(10ms);
    }
    CHECK(server.getStatus() == TcpPortStatus::Active, "control server bound");
    server.setTarget(uc_log::detail::ControlTarget{.read = reader});

    auto const readLine = std::string{R"({"cmd":"read","pieces":[{"address":16,"size":4}]})"
                                      "\n"};
    boost::asio::io_context                                 clientIo;
    std::vector<std::unique_ptr<LineClient<local::socket>>> stuck;
    for(std::size_t i = 0; i != uc_log::detail::ControlServer::Workers; ++i) {
        stuck.push_back(std::make_unique<LineClient<local::socket>>(clientIo));
        stuck.back()->socket.connect(local::endpoint{path.native()});
        stuck.back()->send(readLine);
    }
    CHECK(waitUntil([&] {
              return inside.load() == static_cast<int>(uc_log::detail::ControlServer::Workers);
          }),
          "every worker sits in a read");

    LineClient<local::socket> other{clientIo};
    other.socket.connect(local::endpoint{path.native()});
    other.send("{\"cmd\":\"ping\"}\n");
    CHECK(other.next(2s) == R"({"cmd":"ping","protocol":2})", "ping still answers");
    LineClient<local::socket> subscriber{clientIo};
    subscriber.socket.connect(local::endpoint{path.native()});
    subscriber.send(R"({"cmd":"subscribe","streams":["log"]})"
                    "\n");
    CHECK(subscriber.next(2s) == R"({"cmd":"subscribe","protocol":2})", "subscribe still answers");
    CHECK(waitUntil([&] { return said("self-check: no worker free") && said("read for"); }),
          "the self-check says that the workers are stuck, and in what");
    CHECK(!said("socket thread has not turned"), "and not the socket thread, which turns");
    if(said("socket thread has not turned")) {
        for(auto const& m : messages) { std::printf("   %s\n", m.c_str()); }
    }

    {
        std::lock_guard<std::mutex> const lock{mutex};
        released = true;
    }
    release.notify_all();
    for(auto& client : stuck) {
        CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(client->next(5s))),
              "and the stuck reads answer once the target lets go");
    }
    CHECK(waitUntil([&] { return said("self-check: a worker is free again"); }),
          "the self-check says when it is over");
}

// the self-check names a socket thread that does not turn (here: held in the status callback)
static void testStuckSocketThread(boost::asio::ip::address const& loopback,
                                  std::filesystem::path const&    path) {
    note("control server: the socket thread stuck");
    std::mutex               mutex;
    Clock::ConditionVariable release;
    bool                     released = false;
    std::atomic<bool>        holding{false};
    std::vector<std::string> messages;
    auto const               said = [&](std::string_view part) {
        std::lock_guard<std::mutex> const lock{mutex};
        return std::ranges::any_of(messages, [&](auto const& m) {
            return m.find(part) != std::string::npos;
        });
    };
    {
        uc_log::detail::ControlServer server{loopback,
                                             0,
                                             path,
                                             [&](std::string_view msg) {
                                                 std::lock_guard<std::mutex> const lock{mutex};
                                                 messages.emplace_back(msg);
                                             },
                                             [&](TcpPortStatus, std::uint16_t) {
                                                 std::unique_lock<std::mutex> lock{mutex};
                                                 holding = true;
                                                 release.wait(lock, [&] { return released; });
                                             }};
        server.setStuckLimit(300ms);
        CHECK(waitUntil([&] { return holding.load(); }), "the socket thread is held");
        CHECK(waitUntil([&] { return said("self-check: the socket thread has not turned"); }),
              "the self-check says so");
        CHECK(!said("no worker free"), "and not the workers, which are free");
        {
            std::lock_guard<std::mutex> const lock{mutex};
            released = true;
        }
        release.notify_all();
        CHECK(waitUntil([&] { return said("self-check: the socket thread turns again"); }),
              "and when it turns again");
    }
}

static void testLogHistory(boost::asio::ip::address const&              loopback,
                           uc_log::detail::ControlServer::ErrorF const& errf) {
    note("control server: log history");
    namespace fs  = std::filesystem;
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
        CHECK(c.next().empty() && c.eof, "follow false: closed after backlog_end");
    }
    {
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        auto const refused
          = answerOf(c.ask(R"({"cmd":"subscribe","streams":["metrics"],"since_seq":0})"
                           "\n"));
        CHECK(std::holds_alternative<ctl::Error>(refused), "history without log: an error");
        CHECK(c.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping","protocol":2})",
              "and the connection still takes requests");
    }
    {
        // lines keep coming while the history goes out: every seq once, in order
        std::atomic<bool>         go{false};
        Clock::Thread             publisher{[&] {
            while(!go) { Clock::sleep_for(1ms); }
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
        // ~10 MB of history, more than the send queue holds, to a client that reads slowly. The subscriber above has
        // closed: the lines below go to nobody only once the server has let it go (else they fill its queue and drop)
        CHECK(waitUntil([&] { return server.getClientCount() == 0; }),
              "the live subscriber has left");
        auto const first = server.logSeq();
        for(int i = 0; i != 20'000; ++i) { publish(i, 400); }
        LineClient<local::socket> c{io};
        c.socket.connect(local::endpoint{path.native()});
        c.send(R"({"cmd":"subscribe","streams":["log"],"since_seq":)" + std::to_string(first)
               + R"(,"follow":false})"
                 "\n");
        Clock::sleep_for(200ms);   // let the send queue fill up
        CHECK(std::holds_alternative<ctl::SubscribeAnswer>(answerOf(c.next())), "subscribed");
        Seen seen;
        while(!seen.end && failures.load() == 0) { take(seen, c.next()); }
        CHECK(contiguous(seen.seqs, first) && seen.seqs.size() == 20'000 && seen.end
                && seen.end->next_seq == first + 20'000,
              "a history larger than the send queue arrives whole");
        CHECK(server.droppedBytes() == 0, "paced by the client, not dropped");
    }
    {
        CHECK(waitUntil([&] { return server.getClientCount() == 0; }), "the slow reader has left");
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
    std::setvbuf(stdout,
                 nullptr,
                 _IOLBF,
                 0);   // a hang killed by a time limit still shows its last step
    // first: every thread below takes part in it (the servers' socket threads, workers and self-checks too)
    uc_log::detail::VirtualTime const virtualTime;
    auto const                        errf = [](std::string_view msg) {
        if(!quiet) { std::printf("   [err] %.*s\n", static_cast<int>(msg.size()), msg.data()); }
    };
    auto const loopback = boost::asio::ip::make_address("127.0.0.1");
    auto const dir      = std::filesystem::temp_directory_path()
                        / ("uc_log_control_server_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);

    testControlServer(loopback, dir, errf);
    testLogHistory(loopback, errf);
    testStalledSubscriber(loopback, dir / "stalled.sock", errf);
    testStuckReads(loopback, dir / "stuck_reads.sock");
    testStuckSocketThread(loopback, dir / "stuck_socket.sock");
    std::filesystem::remove_all(dir);

    if(failures.load() == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures.load());
    return 1;
}
