// Hammer test for SocketSession / ControlServer / DuplexChannelServer over TCP: clients that churn, a client that
// never reads, restarts and bind-address changes under traffic, the duplex hub. Real sockets on real time: every check
// waits for the state it names, none for a time (the ctest TIMEOUT is the hang guard); ControlServer's behaviour on a
// virtual clock is control_server_tests. Run under TSan and ASan.
#include "SocketTestSupport.hpp"
#include "support/FakeTargetPort.hpp"
#include "uc_log/printer/ControlServer.hpp"
#include "uc_log/printer/DuplexChannelServer.hpp"
#include "uc_log/printer/SocketSession.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

using boost::asio::ip::tcp;

namespace {

// Ports below the kernel's ephemeral range: one in it may be any outgoing connection's local port (this test's own
// churn clients too), and the server could not bind it - it then waited for good.
std::uint16_t basePort() {
    unsigned      first = 32768;   // Linux's default start of ip_local_port_range
    std::ifstream range{"/proc/sys/net/ipv4/ip_local_port_range"};
    if(unsigned v{}; range >> v && v > 12000) { first = v; }
    return static_cast<std::uint16_t>(first - 10000 + static_cast<unsigned>(getpid()) % 9000);
}

// what is TCP's own on the control port: requests over it, and a client that half-closes right after its line
// (`echo ... | socat`) - on TCP the server sees the half-close only at the next failed send
void testControlServerTcp(boost::asio::ip::address const&               loopback,
                          std::uint16_t                                 port,
                          uc_log::printer::ControlServer::ErrorF const& errf) {
    note("control server over TCP");
    namespace ctl = uc_log::printer::control;
    uc_log::test::FakeTargetPort readerPort{
      [](std::span<uc_log::printer::TargetOp const> ops) -> uc_log::printer::TargetPort::Result {
          std::vector<std::vector<std::byte>> out;
          for(auto const& op : ops) {
              auto const& r = std::get<uc_log::printer::target_op::Read>(op);
              out.emplace_back(r.size);
              for(std::uint32_t i = 0; i != r.size; ++i) {
                  out.back()[i] = static_cast<std::byte>((r.address + i) & 0xFFU);
              }
          }
          return out;
      }};
    auto const readLine = std::string{R"({"cmd":"read","pieces":[{"address":536870928,"size":2}]})"
                                      "\n"};
    uc_log::printer::ControlServer server{loopback, port, {}, errf, {}};
    CHECK(
      waitUntil([&] { return server.getStatus() == uc_log::printer::SocketPortStatus::active; }),
      "control server bound");
    boost::asio::io_context clientIo;
    LineClient<tcp::socket> client{clientIo};
    client.socket.connect(tcp::endpoint{loopback, port});
    CHECK(client.ask("{\"cmd\":\"ping\"}\n") == R"({"cmd":"ping"})",
          "ping answers without a target");
    CHECK(std::holds_alternative<ctl::Error>(answerOf(client.ask(readLine))),
          "no target yet: an error, not a hang");
    server.setTarget(uc_log::printer::ControlTarget{.target = &readerPort});
    auto const  a = answerOf(client.ask(readLine));
    auto const* r = std::get_if<ctl::ReadAnswer>(&a);
    CHECK(r != nullptr && r->data == std::vector<std::string>{"1011"}, "answer over TCP");
    {
        LineClient<tcp::socket> oneShot{clientIo};
        oneShot.socket.connect(tcp::endpoint{loopback, port});
        oneShot.send(readLine);
        oneShot.socket.shutdown(tcp::socket::shutdown_send);
        CHECK(std::holds_alternative<ctl::ReadAnswer>(answerOf(oneShot.next())),
              "a half-closed TCP client gets its answer");
    }
}

}   // namespace

int main() {
    std::setvbuf(stdout,
                 nullptr,
                 _IOLBF,
                 0);   // a hang killed by a time limit still shows its last step
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

    uc_log::printer::AsioContext ctx;

    // a rebind is done when the listener reports Active again: the status alone still reads Active from before it
    std::atomic<int>               controlBinds{0};
    uc_log::printer::ControlServer control{
      loopback,
      portA,
      {},
      errf,
      [&](uc_log::printer::SocketPortStatus status, std::uint16_t) {
          if(status == uc_log::printer::SocketPortStatus::active) { ++controlBinds; }
      }};

    uc_log::printer::DuplexChannelHub hub{ctx.ioc, loopback, duplexBase, errf, msgf, []() {}};

    uc_log::printer::AsioContextRunner runner{ctx};

    CHECK(
      waitUntil([&] { return control.getStatus() == uc_log::printer::SocketPortStatus::active; }),
      "control port bound");

    std::string const subscribeMetrics = R"({"cmd":"subscribe","streams":["metrics"]})"
                                         "\n";

    // ---- part A: the control server's metrics stream ----------------------------
    note("A: producer + client churn");
    std::atomic<bool>                            produce{true};
    uc_log::printer::control::MetricSample const bigSample{.name  = std::string(4096, 'x'),
                                                           .scope = "test",
                                                           .unit  = "",
                                                           .time  = 1.0,
                                                           .value = 2.0};
    std::jthread                                 producer{[&]() {
        while(produce) {
            control.publish(bigSample);
            Clock::sleep_for(50us);
        }
    }};

    {
        std::atomic<int>          connected{0};
        std::vector<std::jthread> churners;
        churners.reserve(4);
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
                    } catch(
                      std::exception const&) { /* ignored: a refused connect is part of the churn */
                    }
                    Clock::sleep_for(1ms);
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
        CHECK(waitUntil([&] { return control.droppedBytes() > 0; }),
              "stalled client dropped data instead of growing queue");
        boost::system::error_code ec;
        stalled.close(ec);
    }

    note("A: restart mid-traffic");
    auto binds = controlBinds.load();
    control.restart(portA2);
    CHECK(waitUntil([&] {
              return controlBinds.load() > binds && control.getPort() == portA2
                  && control.getStatus() == uc_log::printer::SocketPortStatus::active;
          }),
          "rebound on the new port after restart");
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
        binds              = controlBinds.load();
        control.setBindAddress(anyAddr);
        CHECK(waitUntil([&] {
                  return controlBinds.load() > binds
                      && control.getStatus() == uc_log::printer::SocketPortStatus::active;
              }),
              "rebound on new address");
        {
            boost::asio::io_context   cioc;
            tcp::socket               s{cioc};
            boost::system::error_code ec;
            s.connect(tcp::endpoint{loopback, portA2},
                      ec);   // 0.0.0.0 still reachable via loopback
            CHECK(!ec, "client connects after address change");
            s.close(ec);
        }
        binds = controlBinds.load();
        control.setBindAddress(loopback);
        CHECK(waitUntil([&] {
                  return controlBinds.load() > binds
                      && control.getStatus() == uc_log::printer::SocketPortStatus::active;
              }),
              "rebound back to loopback");
    }

    produce = false;
    producer.join();

    control.stop();
    CHECK(waitUntil(
            [&] { return control.getStatus() == uc_log::printer::SocketPortStatus::notStarted; }),
          "stopped");
    CHECK(waitUntil([&] { return control.getClientCount() == 0; }), "sessions closed on stop");

    // ---- part B: duplex ----------------------------------------------------------
    note("B: duplex configure + echo integrity");
    hub.configure({
      uc_log::printer::DuplexChannelDesc{0, "shell", std::uint32_t{2}, 0},
      uc_log::printer::DuplexChannelDesc{1,   "raw",     std::nullopt, 1},
    });
    CHECK(waitUntil([&] {
              auto const infos = hub.info();
              return infos.size() == 2
                  && infos[0].status == uc_log::printer::SocketPortStatus::active;
          }),
          "two duplex channels, shell bound");
    {
        auto const infos = hub.info();
        CHECK(infos.size() == 2, "two duplex channels");
        CHECK(infos.size() == 2 && infos[0].status == uc_log::printer::SocketPortStatus::active,
              "shell bound");
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
                Clock::sleep_for(200us);
            }
        }
    }};

    {
        boost::asio::io_context cioc;
        tcp::socket             client{cioc};
        client.connect(tcp::endpoint{loopback, duplexBase});
        CHECK(waitUntil([&] { return hub.info()[0].connected; }), "the first client is taken");

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
        constexpr std::size_t total = 1024 * 1024;
        std::vector<char>     tx(total);
        for(std::size_t i = 0; i < total; ++i) { tx[i] = static_cast<char>((i * 7 + 13) & 0xFF); }
        std::jthread      writer{[&]() {
            std::size_t off = 0;
            while(off < total) {
                auto const chunk
                  = std::span{tx}.subspan(off, std::min<std::size_t>(32768, total - off));
                auto const n = client.write_some(boost::asio::buffer(chunk.data(), chunk.size()));
                off += n;
            }
        }};
        std::vector<char> rx;
        rx.reserve(total);
        std::array<char, 65536> buf;
        while(rx.size() < total) {
            auto const n        = client.read_some(boost::asio::buffer(buf));
            auto const received = std::span{buf}.first(n);
            rx.insert(rx.end(), received.begin(), received.end());
        }
        writer.join();
        CHECK(rx.size() == total, "echoed byte count");
        CHECK(std::equal(rx.begin(), rx.end(), tx.begin()), "echoed bytes identical");

        auto const infos = hub.info();
        CHECK(infos[0].bytesToTarget >= total, "bytesToTarget counted");
        CHECK(infos[0].bytesFromTarget >= total, "bytesFromTarget counted");

        boost::system::error_code ec;
        client.shutdown(tcp::socket::shutdown_both, ec);
        client.close(ec);
    }
    CHECK(waitUntil([&] { return !hub.info()[0].connected; }), "client gone detected");

    note("B: port churn while pumping");
    for(int i = 0; i < 10; ++i) {
        hub.setPort(0, static_cast<std::uint16_t>(duplexBase + 20 + (i % 2)));
        hub.setEnabled(0, i % 2 == 0);
        Clock::sleep_for(20ms);
    }
    // a (re)bind is done once the listener's bind count moves: its status still reads Active from before it
    auto const rebound = [&](std::uint64_t before) {
        return waitUntil([&] {
            auto const info = hub.info()[0];
            return info.binds > before && info.status == uc_log::printer::SocketPortStatus::active;
        });
    };
    // the churn ends disabled: enabling binds on its last port first, then the port moves - one bind each, in order
    auto hubBinds = hub.info()[0].binds;
    hub.setEnabled(0, true);
    CHECK(rebound(hubBinds), "enabled again after port churn");
    hubBinds = hub.info()[0].binds;
    hub.setPort(0, duplexBase);
    CHECK(rebound(hubBinds) && hub.info()[0].port == duplexBase, "active after port churn");

    note("B: bind-address change on the whole hub");
    hubBinds = hub.info()[0].binds;
    hub.setBindAddress(boost::asio::ip::make_address("0.0.0.0"));
    CHECK(rebound(hubBinds), "active after hub address change");
    hubBinds = hub.info()[0].binds;
    hub.setBindAddress(loopback);
    CHECK(rebound(hubBinds), "active back on loopback");
    {
        boost::asio::io_context   cioc;
        tcp::socket               client{cioc};
        boost::system::error_code ec;
        client.connect(tcp::endpoint{loopback, duplexBase}, ec);
        CHECK(!ec, "duplex reachable after address round-trip");
    }

    // reconfigure churn (reader reconnect path) while a client is attached
    {
        boost::asio::io_context   cioc;
        tcp::socket               client{cioc};
        boost::system::error_code connectError;
        client.connect(tcp::endpoint{loopback, duplexBase}, connectError);
        CHECK(!connectError, "a client for the reconfigure churn");
        for(int i = 0; i < 5; ++i) {
            hub.configure({
              uc_log::printer::DuplexChannelDesc{0, "shell", std::uint32_t{2}, 0},
              uc_log::printer::DuplexChannelDesc{1,   "raw",     std::nullopt, 1},
            });
            Clock::sleep_for(20ms);
        }
        boost::system::error_code ec;
        client.close(ec);
    }

    pump = false;
    pumpThread.join();

    testControlServerTcp(loopback, static_cast<std::uint16_t>(portA + 3), errf);

    return uc_log::test::report();
}
