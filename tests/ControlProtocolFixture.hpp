#pragma once

// The fake target and the contents of the golden files in doc/control_protocol/, which
// control_protocol_tool writes and control_protocol_tests checks.

#include "support/FakeTargetPort.hpp"
#include "uc_log/printer/ControlAnswer.hpp"
#include "uc_log/printer/ControlEvents.hpp"
#include "uc_log/printer/ControlHistory.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/SocketPath.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace uc_log::control_fixture {

namespace ctl = uc_log::printer::control;

/// Memory reads as its address, except a counter at counterAddress and nothing from
/// disconnectedFrom on. Fake clocks keep every answer reproducible.
struct FakeTarget {
    static constexpr std::uint32_t counterAddress   = 0x3000'0000;
    static constexpr std::uint32_t disconnectedFrom = 0xE000'0000;
    static constexpr std::int64_t  unixMicros       = 1'758'448'800'000'000;   // 2025-09-21 10:00Z
    static constexpr std::int64_t  steadyStepMicros = 1000;

    std::atomic<std::uint32_t> counter{0};
    std::atomic<std::int64_t>  steady{0};
    std::function<void()>      onReset{};
    int                        lineResets{};   // resets through the reset pin ("line": true)
    bool                       coreHalted{};

    printer::TargetPort::Result read(std::span<ctl::Piece const> pieces) {
        std::vector<std::vector<std::byte>> out;
        for(auto const& p : pieces) {
            if(p.address >= disconnectedFrom) {
                return std::unexpected{std::string{"the target is not connected"}};
            }
            out.emplace_back(p.size);
            std::uint32_t const moving = p.address == counterAddress ? ++counter : 0;
            for(std::uint32_t i = 0; i != p.size; ++i) {
                out.back()[i] = static_cast<std::byte>(
                  (p.address == counterAddress ? (i < 4 ? moving >> (8U * i) : 0U) : p.address + i)
                  & 0xFFU);
            }
        }
        return out;
    }

    static std::vector<ctl::StatusMessage> allMessages() {
        return {
          {.time = "2025-09-21T10:00:00.000Z", .level = "status", .text = "firmware matches build"},
          {.time = "2025-09-21T10:00:01.000Z",  .level = "error",  .text = "RTT buffer 0 overflow"},
          {.time = "2025-09-21T10:00:02.000Z",   .level = "tool",      .text = "control socket up"}
        };
    }

    printer::TargetPort::Result write(std::span<ctl::WordWrite const> words) {
        std::vector<std::vector<std::byte>> out;
        for(auto const& w : words) {
            if(w.address >= disconnectedFrom) {
                return std::unexpected{std::string{"the target is not connected"}};
            }
            out.emplace_back(4);
            for(std::uint32_t i = 0; i != 4; ++i) {
                out.back()[i] = static_cast<std::byte>((w.value >> (8U * i)) & 0xFFU);
            }
        }
        return out;
    }

    // each transcript request runs against a fresh target: whether the core is halted is the reader's check
    // (tests/rtt_reader/), not this fake's
    printer::TargetPort::Result access(std::span<printer::TargetOp const> ops) {
        namespace to = printer::target_op;
        std::vector<ctl::Piece>     pieces;
        std::vector<ctl::WordWrite> words;
        for(auto const& op : ops) {
            if(auto const* r = std::get_if<to::Read>(&op)) {
                pieces.push_back({.address = r->address, .size = r->size});
            } else if(auto const* w = std::get_if<to::WriteWord>(&op)) {
                words.push_back({.address = w->address, .value = w->value});
            }
        }
        if(!pieces.empty()) { return read(pieces); }
        if(!words.empty()) { return write(words); }
        std::vector<std::vector<std::byte>> out;
        for(auto const& op : ops) {
            if(std::holds_alternative<to::Halt>(op) || std::holds_alternative<to::Go>(op)) {
                coreHalted = std::holds_alternative<to::Halt>(op);
                out.push_back({std::byte{coreHalted ? std::uint8_t{1} : std::uint8_t{0}}});
            } else if(std::holds_alternative<to::ReadRegisters>(op)) {
                std::vector<std::byte> regs;
                for(std::uint32_t r = 0; r != uc_log::printer::cortex_m::coreRegisterCount; ++r) {
                    uc_log::printer::appendLe32(regs, 0x1000U + r);
                }
                out.push_back(std::move(regs));
            } else {
                out.emplace_back();
            }
        }
        return out;
    }

    test::FakeTargetPort port{
      [this](std::span<printer::TargetOp const> ops) { return access(ops); }};

    printer::ControlTarget target() {
        return printer::ControlTarget{
          .target = &port,
          .status =
            [] {
                return ctl::StatusAnswer{
                  .running  = true,
                  .halted   = false,
                  .flashing = false,
                  .sessions = 1,
                  .firmware = {.state = ctl::FirmwareState::match, .build = 0x1234abcd},
                  .errors   = 1
                };
            },
          .reset = [this](std::function<bool()> const&) -> std::expected<void, std::string> {
              if(onReset) { onReset(); }
              return {};
          },
          .resetLine = [this](std::function<bool()> const&) -> std::expected<void, std::string> {
              ++lineResets;
              return {};
          },
          .flash = [](std::function<bool()> const&) -> std::expected<void, std::string> {
              return std::unexpected{std::string{"no hex file"}};
          },
          .messages =
            [](std::size_t count) {
                auto const all = allMessages();
                auto const n   = std::min(count, all.size());
                return std::vector<ctl::StatusMessage>{all.end() - static_cast<std::ptrdiff_t>(n),
                                                       all.end()};
            },
          .unixMicros   = [] { return unixMicros; },
          .steadyMicros = [this] { return steady += steadyStepMicros; },
          .stop         = {}};
    }
};

/// `stable`: the answer does not depend on earlier requests to the same target.
struct Exchange {
    std::string_view name;
    std::string_view request;
    bool             stable{true};
};

inline std::vector<Exchange> exchanges() {
    return {
      {"ping", R"({"cmd":"ping"})"},
      {"status", R"({"cmd":"status"})"},
      {"messages", R"({"cmd":"messages"})"},
      {"messages_2", R"({"cmd":"messages","count":2})"},
      {"read_one", R"({"cmd":"read","pieces":[{"address":536870928,"size":4}]})"},
      {"read_two",
       R"({"cmd":"read","pieces":[{"address":536871166,"size":3},{"address":16,"size":1}]})"},
      {"read_nothing", R"({"cmd":"read","pieces":[]})"},
      {"read_zero", R"({"cmd":"read","pieces":[{"address":0,"size":0}]})"},
      {"read_too_much", R"({"cmd":"read","pieces":[{"address":0,"size":5000}]})"},
      {"read_disconnected", R"({"cmd":"read","pieces":[{"address":3758096384,"size":4}]})"},
      {"wait_changed",
       R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"changed","timeout_ms":1000})",
       false},
      {"wait_eq",
       R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"eq","value":20,"timeout_ms":1000})",
       false},
      {"wait_mask",
       R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"eq","value":0,"mask":3,"timeout_ms":1000})",
       false},
      {"wait_timeout",
       R"({"cmd":"wait","piece":{"address":536870912,"size":4},"condition":"ne","value":50462976,"timeout_ms":30})"},
      {"wait_too_wide",
       R"({"cmd":"wait","piece":{"address":536870912,"size":9},"condition":"changed","timeout_ms":10})"},
      {"wait_no_value",
       R"({"cmd":"wait","piece":{"address":536870912,"size":4},"condition":"eq","timeout_ms":10})"},
      {"wait_too_long",
       R"({"cmd":"wait","piece":{"address":536870912,"size":4},"condition":"changed","timeout_ms":600001})"},
      {"reset", R"({"cmd":"reset"})"},
      {"reset_line", R"({"cmd":"reset","line":true})"},
      {"halt", R"({"cmd":"halt"})"},
      {"registers", R"({"cmd":"registers"})"},
      {"write_registers",
       R"({"cmd":"write_registers","writes":[{"index":0,"value":7},{"index":15,"value":268443648}]})"},
      {"write_registers_bad", R"({"cmd":"write_registers","writes":[{"index":19,"value":0}]})"},
      {"go", R"({"cmd":"go"})"},
      {"flash", R"({"cmd":"flash"})"},
      {"write_two",
       R"({"cmd":"write","words":[{"address":1342177292,"value":0},{"address":1342177296,"value":305419896}]})"},
      {"write_nothing", R"({"cmd":"write","words":[]})"},
      {"write_unaligned", R"({"cmd":"write","words":[{"address":1342177294,"value":0}]})"},
      {"write_disconnected", R"({"cmd":"write","words":[{"address":3758096384,"value":1}]})"},
      {"records_no_map", R"({"cmd":"records","kinds":["crash","trace"]})"},
      {"records_unknown_kind", R"({"cmd":"records","kinds":["coredump"]})"},
      {"raise_panic_no_map", R"({"cmd":"raise_panic","cause":"user"})"},
      {"not_json", R"(read 20000010:4)"},
      {"unknown_cmd", R"({"cmd":"poke","pieces":[]})"},
      {"unknown_key", R"({"cmd":"ping","verbose":true})"},
    };
}

struct Subscription {
    std::string_view name;
    std::string_view request;
};

inline std::vector<Subscription> subscriptions() {
    return {
      {                "log", R"({"cmd":"subscribe","streams":["log"]})"                             },
      {       "log_warn_i2c",
       R"({"cmd":"subscribe","streams":["log"],"min_level":"warn","modules":["i2c"]})"               },
      { "log_metrics_no_usb",
       R"({"cmd":"subscribe","streams":["log","metrics"],"not_modules":["usb"]})"                    },
      {   "messages_metrics",               R"({"cmd":"subscribe","streams":["messages","metrics"]})"},
      {"history_last_2_warn",
       R"({"cmd":"subscribe","streams":["log"],"min_level":"warn","last":2,"follow":false})"         },
      {    "history_since_3", R"({"cmd":"subscribe","streams":["log"],"since_seq":3,"follow":false})"},
      {"history_without_log",           R"({"cmd":"subscribe","streams":["messages"],"since_seq":0})"},
    };
}

inline std::vector<ctl::Event> streamEvents() {
    using namespace std::chrono;
    auto const at = [](int ms) {
        return sys_time<milliseconds>{sys_days{year{2025} / 9 / 21} + hours{10} + milliseconds{ms}};
    };
    auto const entry = [](std::string      file,
                          std::size_t      line,
                          std::string      function,
                          uc_log::LogLevel level,
                          std::int64_t     ucNs,
                          std::string      module,
                          std::string      message) {
        printer::LogEntry e{0, {}};
        e.fileName     = std::move(file);
        e.line         = line;
        e.functionName = std::move(function);
        e.logLevel     = level;
        e.ucTime.time  = nanoseconds{ucNs};
        e.module       = std::move(module);
        e.logMsg       = std::move(message);
        e.parsedOk     = true;
        return e;
    };
    using L = uc_log::LogLevel;
    std::vector<ctl::Event> events;
    std::uint64_t           seq = 0;
    auto const              log = [&](int ms, printer::LogEntry const& e) {
        auto line = printer::toLogLine(at(ms), e);
        line.seq  = seq++;
        events.emplace_back(std::move(line));
        for(auto const& m : printer::toMetricSamples(at(ms), e)) { events.emplace_back(m); }
    };
    auto const message = [&](int                             ms,
                             std::string_view                level,
                             std::string_view                text,
                             std::optional<ctl::MessageKind> kind = {}) {
        events.emplace_back(printer::toStatusMessage(at(ms), level, text, kind));
    };
    log(0, entry("main.cpp", 10, "main", L::info, 1'500'000'000, "", "boot"));
    log(1, entry("Bus.hpp", 42, "Bus<>::run", L::warn, 1'600'000'000, "i2c.bus", "nack from 0x40"));
    message(2, "status", "firmware matches sanitize_flash.hex (build 1234abcd)");
    log(3,
        entry("Cdc.hpp",
              7,
              "Mixin<>::send",
              L::debug,
              1'700'000'000,
              "usb.cdc",
              "status: temperature @METRIC(valve::temperature[m℃]=58300) flow "
              "@METRIC(regulator::flow[]=0)"));
    log(4, entry("I2C.hpp", 3, "recover", L::error, 1'800'000'000, "i2c", "bus stuck"));
    message(5, "error", "core halted: pc=0x10001234\nlr=0x10005678", ctl::MessageKind::halt);
    log(6, entry("i2cx.hpp", 9, "x", L::warn, 1'900'000'000, "i2cx", "not under i2c"));
    log(7,
        entry("main.cpp",
              20,
              "main",
              L::crit,
              2'000'000'000,
              "",
              "@METRIC(::rate[Hz]=1.5) without a scope, @METRIC(::broken[]=nan) not a number"));
    return events;
}

inline std::string jsonString(std::string_view text) {
    std::string out;
    (void)glz::write_json(text, out);
    return out;
}

inline std::string chomp(std::string line) {
    if(!line.empty() && line.back() == '\n') { line.pop_back(); }
    return line;
}

inline std::string transcript() {
    std::string out;
    for(auto const& x : exchanges()) {
        FakeTarget fake;
        auto const answer = printer::controlAnswerLine(x.request, fake.target());
        out += fmt::format(R"({{"name":{},"stable":{},"request":{},"answer":{}}})",
                           jsonString(x.name),
                           x.stable ? "true" : "false",
                           jsonString(x.request),
                           chomp(answer))
             + "\n";
    }
    return out;
}

inline std::string streams() {
    std::string out;
    auto const  events = streamEvents();
    for(auto const& s : subscriptions()) {
        auto const  parsed    = ctl::parseRequest(s.request);
        auto const* request   = std::get_if<ctl::Request>(&parsed);
        auto const* subscribe = request != nullptr ? std::get_if<ctl::Subscribe>(request) : nullptr;
        auto const  refused = subscribe != nullptr ? ctl::subscribeError(*subscribe) : std::nullopt;
        std::string lines;
        if(subscribe != nullptr && !refused && (subscribe->sinceSeq || subscribe->last)) {
            // what a server holding these events sends: ControlHistory's read and a BacklogEnd
            printer::ControlHistory history;
            for(auto const& e : events) {
                if(auto const* line = std::get_if<ctl::LogLine>(&e)) {
                    auto copy = *line;
                    history.append(copy);
                }
            }
            auto chunk = history.read(*subscribe, history.start(*subscribe), SIZE_MAX);
            chunk.text += ctl::toLine(ctl::Event{
              ctl::BacklogEnd{.nextSeq = chunk.next, .lost = chunk.lost}
            });
            for(std::string_view rest = chunk.text; !rest.empty();) {
                auto const end = rest.find('\n');
                if(!lines.empty()) { lines += ','; }
                lines += rest.substr(0, end);
                rest.remove_prefix(end + 1);
            }
        } else if(subscribe != nullptr && !refused) {
            for(auto const& e : events) {
                bool const delivered
                  = std::visit([&](auto const& value) { return ctl::delivers(*subscribe, value); },
                               e);
                if(!delivered) { continue; }
                if(!lines.empty()) { lines += ','; }
                lines += chomp(ctl::toLine(e));
            }
        }
        std::string answer;
        if(subscribe == nullptr) {
            answer = chomp(ctl::toLine(ctl::Answer{std::get<ctl::Error>(parsed)}));
        } else if(refused) {
            answer = chomp(ctl::toLine(ctl::Answer{ctl::Error{.error = *refused}}));
        } else {
            answer = chomp(ctl::toLine(ctl::Answer{ctl::SubscribeAnswer{}}));
        }
        out += fmt::format(R"({{"name":{},"request":{},"answer":{},"events":[{}]}})",
                           jsonString(s.name),
                           jsonString(s.request),
                           answer,
                           lines)
             + "\n";
    }
    return out;
}

/// The .json files are written as `jq .` writes them (2 spaces), so a formatter leaves them alone.
struct JqStyle : glz::opts {
    std::uint8_t indentation_width = 2;
};

inline std::string jqPretty(std::string const& compact) {
    auto const  pretty = glz::prettify_json<JqStyle{}>(compact);
    std::string out;
    out.reserve(pretty.size() + 1);
    for(std::size_t i = 0; i != pretty.size(); ++i) {
        out += pretty[i];
        // jq signs every exponent: 1.7976931348623157E+308, glaze writes E308
        if(pretty[i] == 'E' && i != 0 && std::isdigit(static_cast<unsigned char>(pretty[i - 1]))
           && i + 1 != pretty.size() && std::isdigit(static_cast<unsigned char>(pretty[i + 1])))
        {
            out += '+';
        }
    }
    return out + "\n";
}

/// Too deep a directory hashes into /tmp/uc_log_<uid>/; "{uid}" keeps the file user-independent.
inline std::string socketPaths() {
    std::vector<std::string> const dirs{
      "/home/user/firmware/build/rtt_log/sanitize",
      "/a/b/../c/rtt_log/t",
      "/home/user/some/deeper/firmware/checkout/build_board/rtt_log/"
      "a_target_name_long_enough_to_push_the_socket_path_past_108_bytes"};
    std::string out   = "[";
    auto const  uid   = fmt::format("uc_log_{}", ::getuid());
    bool        first = true;
    for(auto const& d : dirs) {
        auto path = printer::unixSocketPath(d, ctl::socketName).native();
        if(auto const at = path.find(uid); at != std::string::npos) {
            path.replace(at, uid.size(), "uc_log_{uid}");
        }
        out += fmt::format(R"({}{{"log_dir":{},"name":{},"path":{}}})",
                           first ? "" : ",",
                           jsonString(d),
                           jsonString(ctl::socketName),
                           jsonString(path));
        first = false;
    }
    return jqPretty(out + "]");
}

inline std::string schema() {
    auto const request = glz::write_json_schema<ctl::Request>();
    auto const answer  = glz::write_json_schema<ctl::Answer>();
    auto const event   = glz::write_json_schema<ctl::Event>();
    if(!request || !answer || !event) { return "schema generation failed\n"; }
    return jqPretty(
      fmt::format(R"({{"request":{},"answer":{},"event":{}}})", *request, *answer, *event));
}

inline std::map<std::string,
                std::string>
files() {
    return {
      {      "schema.json",      schema()},
      { "transcript.jsonl",  transcript()},
      {     "stream.jsonl",     streams()},
      {"socket_paths.json", socketPaths()},
    };
}

}   // namespace uc_log::control_fixture
