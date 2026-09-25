// uc_log_control_protocol_tool dump <dir>      write the golden files
// uc_log_control_protocol_tool seeds <dir>     write the fuzzers' seeds
// uc_log_control_protocol_tool serve <socket>  serve the fake target until stdin closes; a `reset`
//                                              publishes the golden stream (for test_uc_log_client.py)
#include "ControlProtocolFixture.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fx = uc_log::control_fixture;

namespace {
std::vector<std::string_view> requestLines() {
    std::vector<std::string_view> lines;
    for(auto const& x : fx::exchanges()) { lines.push_back(x.request); }
    for(auto const& s : fx::subscriptions()) { lines.push_back(s.request); }
    return lines;
}

constexpr std::array<std::string_view, 5> eventSeeds{
  R"(("main.cpp", 42, 2, 123ms, """foo""")hello world)",
  "status: temperature @METRIC(valve::temperature[m\u2103]=58300) flow "
  "@METRIC(regulator::flow[]=0)",
  "@METRIC(::rate[Hz]=1.5) without a scope",
  "@METRIC(a::b[]=nan) @METRIC(a::c[]=inf) @METRIC(a::d[]=1e308)",
  R"(("usb.cpp", 7, 3, 9ms, "usb.cdc", """poll""")stall @METRIC(valve::position[%]=42) done)",
};

void writeSeed(std::filesystem::path const& file,
               std::string_view             content) {
    std::ofstream{file, std::ios::binary} << content;
}

/// The session fuzzer's first byte picks the chunk size (low 6 bits) and whether the client
/// half-closes and reads back (0x40); the rest is what the client sends.
void writeSeeds(std::filesystem::path const& where) {
    for(auto const* name : {"request", "event", "session"}) {
        std::filesystem::remove_all(where / name);
        std::filesystem::create_directories(where / name);
    }
    auto const lines = requestLines();
    for(std::size_t i = 0; i != lines.size(); ++i) {
        auto const id = fmt::format("{:02}", i);
        writeSeed(where / "request" / id, lines[i]);
        writeSeed(where / "session" / id,
                  fmt::format("{}{}\n", static_cast<char>('@' + i % 7), lines[i]));
    }
    std::string pipelined = "@";
    for(std::size_t i = 0; i != 6; ++i) { pipelined += fmt::format("{}\n", lines[i]); }
    writeSeed(where / "session" / "pipelined", pipelined);
    writeSeed(where / "session" / "subscribe_then_more",
              fmt::format("A{}\n{}\n", fx::subscriptions().front().request, lines.front()));
    for(std::size_t i = 0; i != eventSeeds.size(); ++i) {
        writeSeed(where / "event" / fmt::format("{:02}", i), eventSeeds[i]);
    }
}
}   // namespace

int main(int    argc,
         char** argv) {
    std::string const mode = argc > 1 ? *std::next(argv, 1) : "";
    if(argc != 3 || (mode != "dump" && mode != "seeds" && mode != "serve")) {
        std::fprintf(
          stderr,
          "usage: uc_log_control_protocol_tool dump <dir> | seeds <dir> | serve <socket path>\n");
        return 2;
    }
    std::filesystem::path const where{*std::next(argv, 2)};

    if(mode == "dump") {
        std::filesystem::create_directories(where);
        for(auto const& [name, content] : fx::files()) {
            std::ofstream{where / name, std::ios::binary} << content;
            std::printf("wrote %s\n", (where / name).c_str());
        }
        return 0;
    }
    if(mode == "seeds") {
        writeSeeds(where);
        return 0;
    }

    fx::FakeTarget                fake;
    uc_log::detail::ControlServer server{
      boost::asio::ip::make_address("127.0.0.1"),
      0,
      where,
      [](std::string_view msg) {
          std::fprintf(stderr, "control server: %.*s\n", static_cast<int>(msg.size()), msg.data());
      }};
    fake.onReset = [&server] {
        for(auto const& e : fx::streamEvents()) {
            std::visit(
              [&]<typename T>(T const& value) {
                  if constexpr(!std::is_same_v<T, uc_log::control::BacklogEnd>) {
                      server.publish(value);
                  }
              },
              e);
        }
    };
    server.setTarget(fake.target());
    for(int i = 0; i != 500 && server.getStatus() != TcpPortStatus::Active; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if(server.getStatus() != TcpPortStatus::Active) {
        std::fprintf(stderr, "could not listen on %s\n", where.c_str());
        return 1;
    }
    std::printf("ready %s\n", where.c_str());
    std::fflush(stdout);
    std::string ignored;
    while(std::getline(std::cin, ignored)) {}
    server.clearTarget();
    return 0;
}
