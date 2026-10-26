// uc_log_control_protocol_tool dump <dir>      write the golden files
// uc_log_control_protocol_tool seeds <dir>     write the fuzzers' seeds
// uc_log_control_protocol_tool dict <file>     exit 1 when the fuzz dictionary lacks a word of the golden requests
// uc_log_control_protocol_tool serve <socket>  serve the fake target until stdin closes; a `reset`
//                                              publishes the golden stream (for test_uc_log_client.py)
#include "ControlProtocolFixture.hpp"
#include "RecordsControlFixture.hpp"
#include "uc_log/printer/ControlServer.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace fx = uc_log::control_fixture;

namespace {
std::vector<std::string_view> requestLines() {
    std::vector<std::string_view> lines;
    for(auto const& x : fx::exchanges()) { lines.push_back(x.request); }
    for(auto const& c : uc_log::records_fixture::cases()) { lines.push_back(c.request); }
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

/// The rtt_stream fuzzer's first byte seeds the piece sizes and, odd, puts its run of valid frames first; the rest is
/// the stream. Frame starts (0x57 checked, 0x58 unchecked) where nothing follows them, between valid frames, and over
/// a whole stream: the resync's cases.
void writeRttStreamSeeds(std::filesystem::path const& dir) {
    using namespace std::string_literals;
    writeSeed(dir / "frames", "\x01"s);
    writeSeed(dir / "frames_dangling_start", "\x03\x57"s);
    writeSeed(dir / "frames_then_unchecked_start", "\x05\x58\x00\x00"s);
    writeSeed(dir / "start_bytes_only", "\x00"s + std::string(64, '\x57'));
    std::string every = "\x02"s;
    for(int b = 0; b != 256; ++b) { every += static_cast<char>(b); }
    writeSeed(dir / "every_byte", every);
}

/// The protocol's words in the golden requests that `control_protocol.dict` must hold as quoted tokens: every key and
/// every command of a request that parses (the transcript's refused requests and its values - module names - are not
/// the protocol's). The request fuzzer reaches a command only through them. Returns the missing ones.
std::vector<std::string> missingDictWords(std::filesystem::path const& dict) {
    std::ifstream     in{dict};
    std::stringstream text;
    text << in.rdbuf();
    std::set<std::string> tokens;
    std::string           line;
    while(std::getline(text, line)) {
        if(!line.empty() && line.front() == '"') { tokens.insert(line); }
    }
    std::set<std::string> missing;
    // two passes: a key, then a command (one alternation would stop at the key "cmd" and never see its value)
    std::array<std::regex, 2> const words{std::regex{R"rx("([A-Za-z_]+)"\s*:)rx"},
                                          std::regex{R"rx("cmd"\s*:\s*"([A-Za-z_]+)")rx"}};
    for(auto const request : requestLines()) {
        if(!std::holds_alternative<uc_log::printer::control::Request>(
             uc_log::printer::control::parseRequest(request)))
        {
            continue;
        }
        std::string const r{request};
        for(auto const& word : words) {
            for(auto it = std::sregex_iterator{r.begin(), r.end(), word};
                it != std::sregex_iterator{};
                ++it)
            {
                auto const token = fmt::format(R"("\"{}\"")", (*it)[1].str());
                if(!tokens.contains(token)) { missing.insert(token); }
            }
        }
    }
    return {missing.begin(), missing.end()};
}

/// The session fuzzer's first byte picks the chunk size (low 6 bits) and whether the client
/// half-closes and reads back (0x40); the rest is what the client sends.
void writeSeeds(std::filesystem::path const& where) {
    for(auto const* name : {"request", "event", "session", "rtt_stream"}) {
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
    writeRttStreamSeeds(where / "rtt_stream");
}
}   // namespace

int main(int    argc,
         char** argv) {
    std::string const mode = argc > 1 ? *std::next(argv, 1) : "";
    if(argc != 3 || (mode != "dump" && mode != "seeds" && mode != "dict" && mode != "serve")) {
        std::fprintf(stderr,
                     "usage: uc_log_control_protocol_tool dump <dir> | seeds <dir> | dict <file> | "
                     "serve <socket path>\n");
        return 2;
    }
    std::filesystem::path const where{*std::next(argv, 2)};

    if(mode == "dump") {
        std::filesystem::create_directories(where);
        auto all = fx::files();
        all.merge(uc_log::records_fixture::files());
        for(auto const& [name, content] : all) {
            std::ofstream{where / name, std::ios::binary} << content;
            std::printf("wrote %s\n", (where / name).c_str());
        }
        return 0;
    }
    if(mode == "seeds") {
        writeSeeds(where);
        return 0;
    }
    if(mode == "dict") {
        if(!std::filesystem::is_regular_file(where)) {
            std::fprintf(stderr, "no dictionary at %s\n", where.c_str());
            return 2;
        }
        auto const missing = missingDictWords(where);
        for(auto const& token : missing) {
            std::printf("%s: missing %s (a word of the golden requests)\n",
                        where.c_str(),
                        token.c_str());
        }
        return missing.empty() ? 0 : 1;
    }

    fx::FakeTarget                 fake;
    uc_log::printer::ControlServer server{
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
                  if constexpr(!std::is_same_v<T, uc_log::printer::control::BacklogEnd>
                               && !std::is_same_v<T, uc_log::printer::control::Dropped>)
                  {
                      server.publish(value);
                  }
              },
              e);
        }
    };
    server.setTarget(fake.target());
    // until the bind is decided, however long it takes (no deadline: the Python test's ctest TIMEOUT bounds a hang)
    while(server.getStatus() == uc_log::printer::SocketPortStatus::notStarted) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if(server.getStatus() != uc_log::printer::SocketPortStatus::active) {
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
