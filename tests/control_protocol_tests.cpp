#include "ControlProtocolFixture.hpp"

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#ifndef UC_LOG_CONTROL_PROTOCOL_DIR
    #error "UC_LOG_CONTROL_PROTOCOL_DIR: where doc/control_protocol is"
#endif

static int failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if(!(cond)) {                                                   \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++failures;                                                 \
        }                                                               \
    } while(0)

namespace ctl = uc_log::control;
namespace fx  = uc_log::control_fixture;

static ctl::Answer ask(std::string_view line) {
    fx::FakeTarget fake;
    auto const     parsed
      = ctl::fromLine<ctl::Answer>(uc_log::detail::controlAnswerLine(line, fake.target()));
    if(auto const* a = std::get_if<ctl::Answer>(&parsed)) { return *a; }
    return std::get<ctl::Error>(parsed);
}

template<typename T>
static T const* as(ctl::Answer const& a) {
    return std::get_if<T>(&a);
}

static std::string errorOf(ctl::Answer const& a) {
    auto const* e = as<ctl::Error>(a);
    return e != nullptr ? e->error : std::string{};
}

static std::string readFile(std::string const& path) {
    std::ifstream      in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

int main() {
    CHECK(as<ctl::PingAnswer>(ask(R"({"cmd":"ping"})")) != nullptr
            && as<ctl::PingAnswer>(ask(R"({"cmd":"ping"})"))->protocol == ctl::ProtocolVersion,
          "ping answers the protocol version");

    auto const one = ask(R"({"cmd":"read","pieces":[{"address":536870928,"size":4}]})");
    CHECK(as<ctl::ReadAnswer>(one) != nullptr
            && as<ctl::ReadAnswer>(one)->data == std::vector<std::string>{"10111213"}
            && as<ctl::ReadAnswer>(one)->unix_us == fx::FakeTarget::UnixMicros,
          "one piece: hex bytes, and the time they came back");
    auto const two
      = ask(R"({"cmd":"read","pieces":[{"address":536871166,"size":3},{"address":16,"size":1}]})");
    CHECK(as<ctl::ReadAnswer>(two) != nullptr
            && (as<ctl::ReadAnswer>(two)->data == std::vector<std::string>{"feff00", "10"}),
          "two pieces in request order");
    CHECK(errorOf(ask(R"({"cmd":"read","pieces":[]})")) == "read: nothing to read",
          "nothing to read");
    CHECK(errorOf(ask(R"({"cmd":"read","pieces":[{"address":0,"size":0}]})"))
            == "read: a piece of 0 bytes",
          "size 0");
    CHECK(errorOf(ask(R"({"cmd":"read","pieces":[{"address":0,"size":5000}]})"))
            .starts_with("read: too much"),
          "more than 4096 bytes");
    {
        fx::FakeTarget fake;
        auto const     before = fake.counter.load();
        (void)uc_log::detail::controlAnswerLine(
          R"({"cmd":"read","pieces":[{"address":805306368,"size":5000}]})",
          fake.target());
        CHECK(fake.counter.load() == before, "a refused request never reaches the target");
    }
    CHECK(errorOf(ask(R"({"cmd":"read","pieces":[{"address":3758096384,"size":4}]})"))
            == "the target is not connected",
          "the reader's error is passed on");

    auto const changed = ask(
      R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"changed","timeout_ms":1000})");
    CHECK(as<ctl::WaitAnswer>(changed) != nullptr && as<ctl::WaitAnswer>(changed)->hit
            && as<ctl::WaitAnswer>(changed)->first == 1 && as<ctl::WaitAnswer>(changed)->last == 2,
          "wait changed");
    auto const reached = ask(
      R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"eq","value":20,"timeout_ms":1000})");
    CHECK(as<ctl::WaitAnswer>(reached) != nullptr && as<ctl::WaitAnswer>(reached)->last == 20,
          "wait eq");
    auto const masked = ask(
      R"({"cmd":"wait","piece":{"address":805306368,"size":4},"condition":"eq","value":0,"mask":3,"timeout_ms":1000})");
    CHECK(as<ctl::WaitAnswer>(masked) != nullptr && as<ctl::WaitAnswer>(masked)->hit
            && as<ctl::WaitAnswer>(masked)->last == 0,
          "wait with a mask");
    auto const never = ask(
      R"({"cmd":"wait","piece":{"address":536870912,"size":4},"condition":"ne","value":50462976,"timeout_ms":30})");
    CHECK(as<ctl::WaitAnswer>(never) != nullptr && !as<ctl::WaitAnswer>(never)->hit
            && as<ctl::WaitAnswer>(never)->waited_us >= 30'000
            && as<ctl::WaitAnswer>(never)->last == 50462976,
          "a wait that runs out says so, with the last value");
    CHECK(
      !errorOf(
         ask(
           R"({"cmd":"wait","piece":{"address":0,"size":9},"condition":"changed","timeout_ms":10})"))
         .empty(),
      "more than 8 bytes is not a number");
    CHECK(
      !errorOf(
         ask(R"({"cmd":"wait","piece":{"address":0,"size":4},"condition":"eq","timeout_ms":10})"))
         .empty(),
      "eq needs a value");

    auto const messages = ask(R"({"cmd":"messages","count":2})");
    CHECK(as<ctl::MessagesAnswer>(messages) != nullptr
            && as<ctl::MessagesAnswer>(messages)->messages.size() == 2
            && as<ctl::MessagesAnswer>(messages)->messages.back().level == "tool",
          "messages: the newest, oldest first");
    auto const status = ask(R"({"cmd":"status"})");
    CHECK(as<ctl::StatusAnswer>(status) != nullptr
            && as<ctl::StatusAnswer>(status)->firmware.state == ctl::FirmwareState::match,
          "status");
    CHECK(as<ctl::ResetAnswer>(ask(R"({"cmd":"reset"})")) != nullptr, "reset");
    {
        auto const written = ask(R"({"cmd":"write","words":[{"address":16,"value":258}]})");
        CHECK(as<ctl::WriteAnswer>(written) != nullptr
                && as<ctl::WriteAnswer>(written)->data == std::vector<std::string>{"02010000"},
              "a word written and read back, little endian");
        CHECK(!errorOf(ask(R"({"cmd":"write","words":[{"address":18,"value":0}]})")).empty(),
              "a write off a word boundary is refused");
    }
    CHECK(errorOf(ask(R"({"cmd":"flash"})")) == "no hex file", "a failed flash says why");
    {
        uc_log::detail::ControlTarget const bare{};
        auto const                          answer = ctl::fromLine<ctl::Answer>(
          uc_log::detail::controlAnswerLine(R"({"cmd":"flash"})", bare));
        CHECK(std::holds_alternative<ctl::Answer>(answer)
                && std::holds_alternative<ctl::Error>(std::get<ctl::Answer>(answer)),
              "no flash without a handler");
    }
    CHECK(!errorOf(ask("read 20000010:4")).empty(), "the old line protocol is an error");
    CHECK(!errorOf(ask(R"({"cmd":"poke"})")).empty(), "an unknown command");
    CHECK(!errorOf(ask(R"({"cmd":"ping","verbose":true})")).empty(), "an unknown key");

    {
        std::string nasty;
        for(int b = 0; b != 256; ++b) { nasty += static_cast<char>(b); }
        nasty += "\xE2\x82";                           // cut off in the middle of a character
        nasty += "\xED\xA0\x80\xC0\xAF";               // a surrogate, an overlong '/'
        nasty += "ok \xE2\x84\x83 \xF0\x9F\x98\x80";   // and real UTF-8 stays
        auto const line  = ctl::toLine(ctl::Event{
          ctl::LogLine{.recv_time  = "",
                       .channel    = 0,
                       .file       = "",
                       .line       = 0,
                       .function   = "",
                       .level      = ctl::Level::info,
                       .uc_time_ns = 0,
                       .message    = nasty,
                       .module     = ""}
        });
        bool       clean = line.back() == '\n' && line.find('\n') == line.size() - 1;
        for(char const ch : line.substr(0, line.size() - 1)) {
            clean = clean && static_cast<unsigned char>(ch) >= 0x20U;
        }
        CHECK(clean, "one line, no raw control character in it");
        auto utf8 = line;
        ctl::makeUtf8(utf8);
        CHECK(utf8 == line, "and nothing left that is not UTF-8");
        auto const  back = ctl::fromLine<ctl::Event>(line);
        auto const* e    = std::get_if<ctl::Event>(&back);
        auto const* l    = e != nullptr ? std::get_if<ctl::LogLine>(e) : nullptr;
        CHECK(l != nullptr && l->message.starts_with(std::string("\0\x01", 2))
                && l->message.ends_with("ok \xE2\x84\x83 \xF0\x9F\x98\x80"),
              "it reads back: the control characters as they were, real UTF-8 as it was");
        CHECK(!errorOf(ask("\x01\xff{\"cmd\"}")).empty(), "a request of such bytes: an error");
        std::string fine = "a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80";
        auto        copy = fine;
        ctl::makeUtf8(copy);
        CHECK(copy == fine, "makeUtf8 leaves UTF-8 alone");
        std::string bad = "\xFF";
        ctl::makeUtf8(bad);
        CHECK(bad == "\xEF\xBF\xBD", "and replaces a stray byte");
    }

    CHECK(ctl::moduleUnder("i2c.bus", "i2c") && ctl::moduleUnder("i2c", "i2c")
            && !ctl::moduleUnder("i2cx", "i2c") && !ctl::moduleUnder("i2c.bus", "i2c.b"),
          "a module is under its prefixes, dot by dot");

    {
        bool roundTrips = true;
        for(auto const& x : fx::exchanges()) {
            fx::FakeTarget fake;
            auto const     line   = uc_log::detail::controlAnswerLine(x.request, fake.target());
            auto const     parsed = ctl::fromLine<ctl::Answer>(line);
            roundTrips            = roundTrips && std::holds_alternative<ctl::Answer>(parsed)
                                 && ctl::toLine(std::get<ctl::Answer>(parsed)) == line;
        }
        CHECK(roundTrips, "answers round-trip through glaze");
        bool events = true;
        for(auto const& e : fx::streamEvents()) {
            auto const line   = ctl::toLine(e);
            auto const parsed = ctl::fromLine<ctl::Event>(line);
            events            = events && std::holds_alternative<ctl::Event>(parsed)
                             && ctl::toLine(std::get<ctl::Event>(parsed)) == line;
        }
        CHECK(events, "events round-trip through glaze");
    }

    {
        using uc_log::detail::ControlHistory;
        auto const line = [](int i, ctl::Level level, std::string module) {
            ctl::LogLine out;
            out.level   = level;
            out.message = std::to_string(i);
            out.module  = std::move(module);
            return out;
        };

        struct Sub {
            std::vector<ctl::Stream>     streams{ctl::Stream::log};
            std::optional<ctl::Level>    minLevel{};
            std::vector<std::string>     modules{};
            std::optional<std::uint64_t> since{};
            std::optional<std::uint32_t> last{};
            bool                         follow{true};

            operator ctl::Subscribe() const {
                ctl::Subscribe out;
                out.streams   = streams;
                out.min_level = minLevel;
                out.modules   = modules;
                out.since_seq = since;
                out.last      = last;
                out.follow    = follow;
                return out;
            }
        };

        auto const refused = [](Sub const& s) { return ctl::subscribeError(s).has_value(); };
        auto const logged  = [](std::string const& text) {
            std::vector<std::string> out;
            std::istringstream       in{text};
            for(std::string row; std::getline(in, row);) {
                auto const  parsed = ctl::fromLine<ctl::Event>(row);
                auto const* e      = std::get_if<ctl::Event>(&parsed);
                auto const* log    = e != nullptr ? std::get_if<ctl::LogLine>(e) : nullptr;
                out.push_back(log != nullptr ? std::to_string(log->seq) + ":" + log->message : "?");
            }
            return out;
        };
        using V = std::vector<std::string>;

        ControlHistory history;
        for(int i = 0; i != 6; ++i) {
            auto added
              = line(i, i % 2 == 0 ? ctl::Level::info : ctl::Level::warn, i < 3 ? "i2c" : "usb");
            auto const& text = history.append(added);
            CHECK(added.seq == static_cast<std::uint64_t>(i), "append numbers the lines from 0");
            CHECK(text.find(R"("seq":)" + std::to_string(i)) != std::string::npos,
                  "the stored protocol line carries its seq");
        }
        CHECK(history.nextSeq() == 6 && history.firstSeq() == 0, "six lines held");

        ctl::Subscribe const all   = Sub{.since = 0};
        auto const           chunk = history.read(all, history.start(all), SIZE_MAX);
        CHECK(logged(chunk.text) == (V{"0:0", "1:1", "2:2", "3:3", "4:4", "5:5"}) && chunk.atEnd
                && chunk.next == 6 && chunk.lost == 0,
              "since_seq 0: everything, in order");

        ctl::Subscribe const warnUsb
          = Sub{.minLevel = ctl::Level::warn, .modules = {"usb"}, .since = 0};
        CHECK(logged(history.read(warnUsb, 0, SIZE_MAX).text) == (V{"3:3", "5:5"}),
              "the history goes through the same filters as the stream");

        ctl::Subscribe const last2 = Sub{.minLevel = ctl::Level::warn, .last = 2};
        CHECK(history.start(last2) == 3, "last 2 warn: starts at the second newest warn line");
        CHECK(logged(history.read(last2, history.start(last2), SIZE_MAX).text) == (V{"3:3", "5:5"}),
              "and gives exactly those two");
        CHECK(history.start(Sub{.last = 99}) == 0, "more asked than held: from the oldest");
        CHECK(history.start(Sub{.since = 1000}) == 6, "a seq not reached yet: from the end");

        auto const small = history.read(all, 0, 1);
        CHECK(logged(small.text) == (V{"0:0"}) && small.next == 1 && !small.atEnd,
              "a chunk smaller than a line still moves on by one line");
        auto const firstTwo = history.read(all, 0, chunk.text.size() / 3);
        CHECK(logged(firstTwo.text).size() == 2 && firstTwo.next == 2, "chunks stop at maxBytes");

        auto const before = history.bytes();
        history.setLimit(before / 2);
        CHECK(history.firstSeq() > 0 && history.nextSeq() == 6 && history.bytes() <= before / 2,
              "a lower limit drops the oldest lines");
        auto const afterDrop = history.read(all, 0, SIZE_MAX);
        auto const oldest    = std::to_string(history.firstSeq());
        CHECK(afterDrop.lost == history.firstSeq() && afterDrop.atEnd
                && logged(afterDrop.text).front() == oldest + ":" + oldest,
              "lines asked for but dropped are counted as lost");
        history.setLimit(0);
        CHECK(history.nextSeq() - history.firstSeq() == 1, "the newest line always stays");
        auto next = line(6, ctl::Level::info, "");
        history.append(next);
        CHECK(next.seq == 6 && history.firstSeq() == 6, "and numbering goes on across drops");

        CHECK(!refused(Sub{.since = 0}), "since_seq with the log stream is fine");
        CHECK(refused(Sub{.streams = {ctl::Stream::messages}, .last = 1}),
              "history without the log stream is refused");
        CHECK(refused(Sub{.since = 0, .last = 1}), "since_seq and last together are refused");
        CHECK(refused(Sub{.follow = false}), "follow false without history is refused");
        CHECK(refused(Sub{
                .streams = {ctl::Stream::log, ctl::Stream::messages},
                .last    = 1,
                .follow  = false
        }),
              "follow false with another stream is refused: it would get live lines");
    }

    for(auto const& [name, generated] : fx::files()) {
        auto const path   = std::string{UC_LOG_CONTROL_PROTOCOL_DIR} + "/" + name;
        auto const onDisk = readFile(path);
        if(onDisk == generated) { continue; }
        std::printf(
          "FAIL: %s differs from what the protocol writes now - if the change is meant, "
          "run `cmake --build <build> --target update_control_protocol` and let the "
          "client (tools/uc_log_client.py) follow\n",
          path.c_str());
        std::istringstream a{onDisk};
        std::istringstream b{generated};
        std::string        la;
        std::string        lb;
        for(int line = 1;; ++line) {
            bool const ga = static_cast<bool>(std::getline(a, la));
            bool const gb = static_cast<bool>(std::getline(b, lb));
            if(!ga && !gb) { break; }
            if(la != lb || ga != gb) {
                std::printf("  first difference, line %d:\n  file: %s\n  now:  %s\n",
                            line,
                            ga ? la.c_str() : "(end)",
                            gb ? lb.c_str() : "(end)");
                break;
            }
        }
        ++failures;
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
