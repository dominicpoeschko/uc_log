// Hammer test for the reworked TcpServerCommon / TCPSender / DuplexChannelServer:
// connect/disconnect churn, stalled-client backpressure, duplex echo integrity,
// second-client rejection, port restart mid-traffic. Run under TSan and ASan.
#include "uc_log/detail/DuplexChannelServer.hpp"
#include "uc_log/detail/TcpSender.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>
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

static std::uint16_t basePort() { return static_cast<std::uint16_t>(41000 + (getpid() % 2000)); }

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

    TCPSender sender{ctx.ioc, loopback, portA, errf, [](TcpPortStatus, std::uint16_t) {}};

    uc_log::detail::DuplexChannelHub hub{ctx.ioc, loopback, duplexBase, errf, msgf, []() {}};

    uc_log::detail::AsioContextRunner runner{ctx};

    std::this_thread::sleep_for(100ms);
    CHECK(sender.getStatus() == TcpPortStatus::Active, "metrics port bound");

    // ---- part A: metrics sender -------------------------------------------------
    note("A: producer + client churn");
    std::atomic<bool> produce{true};
    std::string const bigMsg(4096, 'x');
    std::jthread      producer{[&]() {
        while(produce) {
            sender.send(bigMsg);
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
        // never read: the 4 MiB send queue must cap and drops must be counted
        auto const deadline = std::chrono::steady_clock::now() + 8s;
        while(sender.bytesDropped.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        CHECK(sender.bytesDropped.load() > 0,
              "stalled client dropped data instead of growing queue");
        boost::system::error_code ec;
        stalled.close(ec);
    }

    note("A: restart mid-traffic");
    sender.restart(portA2);
    std::this_thread::sleep_for(200ms);
    CHECK(sender.getStatus() == TcpPortStatus::Active, "rebound after restart");
    CHECK(sender.getPort() == portA2, "port updated");
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
        sender.setBindAddress(anyAddr);
        std::this_thread::sleep_for(200ms);
        CHECK(sender.getStatus() == TcpPortStatus::Active, "rebound on new address");
        {
            boost::asio::io_context   cioc;
            tcp::socket               s{cioc};
            boost::system::error_code ec;
            s.connect(tcp::endpoint{loopback, portA2},
                      ec);   // 0.0.0.0 still reachable via loopback
            CHECK(!ec, "client connects after address change");
            s.close(ec);
        }
        sender.setBindAddress(loopback);   // back to loopback
        std::this_thread::sleep_for(200ms);
        CHECK(sender.getStatus() == TcpPortStatus::Active, "rebound back to loopback");
    }

    produce = false;
    producer.join();

    sender.stop();
    std::this_thread::sleep_for(200ms);
    CHECK(sender.getStatus() == TcpPortStatus::NotStarted, "stopped");
    CHECK(sender.getClientCount() == 0, "sessions closed on stop");

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

    if(failures.load() == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures.load());
    return 1;
}
