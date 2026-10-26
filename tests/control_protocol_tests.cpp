#include "ControlProtocolFixture.hpp"
#include "RecordsControlFixture.hpp"
#include "support/Check.hpp"

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

namespace ctl = uc_log::printer::control;
namespace fx  = uc_log::control_fixture;

namespace {

ctl::Answer ask(std::string_view line) {
    fx::FakeTarget fake;
    auto const     parsed
      = ctl::fromLine<ctl::Answer>(uc_log::printer::controlAnswerLine(line, fake.target()));
    if(auto const* a = std::get_if<ctl::Answer>(&parsed)) { return *a; }
    return std::get<ctl::Error>(parsed);
}

template<typename T>
T const* as(ctl::Answer const& a) {
    return std::get_if<T>(&a);
}

// A request of the golden transcript (ControlProtocolFixture.hpp, exchanges()) by its name: this test and the golden
// file ask the very same bytes, so a request is spelled in one place.
std::string_view request(std::string_view name) {
    for(auto const& x : fx::exchanges()) {
        if(x.name == name) { return x.request; }
    }
    std::printf("FAIL: no exchange \"%.*s\" in the transcript\n",
                static_cast<int>(name.size()),
                name.data());
    ++uc_log::test::failures;
    return {};
}

ctl::Answer askNamed(std::string_view name) { return ask(request(name)); }

std::string errorOf(ctl::Answer const& a) {
    auto const* e = as<ctl::Error>(a);
    return e != nullptr ? e->error : std::string{};
}

namespace rf = uc_log::records_fixture;

rf::Case recordsCase(std::string_view name) {
    auto const all = rf::cases();
    for(auto const& c : all) {
        if(c.name == name) { return c; }
    }
    std::printf("FAIL: no records case \"%.*s\"\n", static_cast<int>(name.size()), name.data());
    ++uc_log::test::failures;
    return all.front();
}

ctl::Answer answerOf(std::string const& line) {
    auto const parsed = ctl::fromLine<ctl::Answer>(line);
    if(auto const* a = std::get_if<ctl::Answer>(&parsed)) { return *a; }
    return std::get<ctl::Error>(parsed);
}

ctl::RecordsAnswer recordsOf(std::string_view name) {
    auto const a = answerOf(rf::run(recordsCase(name)).answer);
    if(auto const* r = as<ctl::RecordsAnswer>(a)) { return *r; }
    std::printf("FAIL: %.*s: no records answer: %s\n",
                static_cast<int>(name.size()),
                name.data(),
                errorOf(a).c_str());
    ++uc_log::test::failures;
    return {};
}

std::size_t sizeOf(auto const& optionalVector) {
    return optionalVector ? optionalVector->size() : 0;
}

// The glz::opts members glaze reads by name (`requires { Opts.<name>; }`): a renamed one compiles and is silently
// ignored, so each is pinned by what it does - WriteOptions' escape_control_characters and the fixture's
// indentation_width.
void glazeOptionsByName() {
    auto const line = ctl::toLine(ctl::Error{.error = std::string{"a\x01"
                                                                  "b"}});
    CHECK(
      line.contains("\\u0001") && !line.contains('\x01'),
      "WriteOptions: a control character in a string goes out escaped (escape_control_characters)");
    auto const pretty = fx::jqPretty(R"({"a":[1]})");
    CHECK(pretty.contains("\n  \"a\"") && !pretty.contains("\n   \"a\""),
          "JqStyle: indented by 2 (indentation_width)");
}

// The fixture target's records through the control protocol: every key a client reads, decoded in the printer.
void recordsOverTheProtocol() {
    using V        = std::vector<std::string>;
    auto const all = recordsOf("records_lld");
    CHECK(all.map.path == "fixture.lld.map" && all.map.generation == 1, "the map it read with");
    CHECK(
      all.fault && all.fault->valid && all.fault->record && all.fault->record->pc == 0x1000'0124U
        && all.fault->core == 0U && all.fault->arch == 8U
        && all.fault->cfsrNames == (V{"PRECISERR", "BFARVALID"}) && sizeOf(all.fault->words) == 33
        && all.fault->words->at(8).name == "pc" && all.fault->words->at(8).value == 0x1000'0124U
        && all.fault->words->at(18).name == "exc_return" && all.fault->exceptionName.has_value(),
      "fault: the record and what it means");
    CHECK(all.stack && all.stack->valid && all.stack->from == 0x2000'0700U
            && sizeOf(all.stack->body) == 4,
          "stack snapshot: the valid words only");
    CHECK(all.panic && all.panic->causeName == "fault" && all.panic->site == 0x1000'02FEU
            && all.panic->timeUs == 123'456'789U,
          "panic: cause name, site, time");
    CHECK(all.starvation && all.starvation->record && all.starvation->record->silentMs == 812,
          "starvation");
    CHECK(all.recoveries && all.recoveries->inProgressCheck == 2U
            && all.recoveries->inProgressAttempt == 2U,
          "recoveries: the onStarved a reset cut short");
    CHECK(all.guard && all.guard->failureName == "starvation" && all.guard->record
            && all.guard->record->bootCount == 7,
          "guard: the last failure by name");
    CHECK(all.ubsan && all.ubsan->record && all.ubsan->record->count == 2, "ubsan");
    CHECK(sizeOf(all.stacks) == 2 && all.stacks->at(0).used == 608U
            && all.stacks->at(1).used == 128U,
          "both stacks");
    CHECK(sizeOf(all.rings) == 2 && all.rings->at(0).name == "edges"
            && sizeOf(all.rings->at(0).rows) == 1024 && all.rings->at(0).rows->front().index == 476
            && all.rings->at(1).name == "isr",
          "both rings, the full-size one whole");
    CHECK(all.panicCauses.size() == uc_log::printer::records::panicCauses.size()
            && all.exceptionNames.size() == 16 && all.exceptionNames[3] == "HardFault",
          "the name tables a client needs");
    auto const gnu = recordsOf("records_gnu");
    CHECK(gnu.fault && gnu.fault->valid && gnu.stack && gnu.stack->valid && sizeOf(gnu.stacks) == 2
            && gnu.rings && gnu.rings->empty(),
          "a GNU map: every record, no ring (no descriptor)");
    auto const crash = recordsOf("records_crash");
    CHECK(crash.fault && crash.panic && !crash.starvation && !crash.ubsan && !crash.stacks
            && !crash.rings,
          "only the kinds asked for");
    auto const empty = recordsOf("records_crash_health_empty");
    CHECK(empty.fault && !empty.fault->valid && !empty.fault->record && empty.panic
            && !empty.panic->valid && empty.guard && !empty.guard->valid,
          "records taken: there, not valid");
    auto const none = recordsOf("records_crash_health_no_symbols");
    CHECK(!none.fault && !none.stack && !none.panic && !none.starvation && !none.recoveries
            && !none.guard,
          "no symbols: no keys");
    auto const unpainted = recordsOf("records_stacks_not_painted");
    CHECK(sizeOf(unpainted.stacks) == 2 && !unpainted.stacks->at(0).valid
            && !unpainted.stacks->at(0).error && unpainted.stacks->at(1).valid,
          "a stack not painted");
    CHECK(sizeOf(recordsOf("records_stacks_no_symbols").stacks) == 0,
          "no stack symbols: no stacks");
    auto const clean = recordsOf("records_ubsan_none");
    CHECK(clean.ubsan && clean.ubsan->record && clean.ubsan->record->count == 0,
          "no sanitizer report");
    CHECK(!recordsOf("records_ubsan_not_sanitize").ubsan, "not a sanitize image: no counter");
    auto const moved = recordsOf("records_trace_written_while_reading");
    CHECK(sizeOf(moved.rings) == 2 && moved.rings->at(0).writtenWhileReading == 3U
            && moved.rings->at(0).rows->front().index == 479,
          "written while reading: the oldest dropped");
    auto const drops = recordsOf("records_trace_written_while_reading_drops");
    CHECK(sizeOf(drops.rings) == 2 && drops.rings->at(1).writtenWhileReading == 6U
            && drops.rings->at(1).rows->front().index == 3,
          "the small ring: what may be overwritten dropped");
    CHECK(sizeOf(recordsOf("records_trace_no_rings").rings) == 0, "no rings");
    auto const half = recordsOf("records_trace_a_storage_without_its_descriptor");
    CHECK(sizeOf(half.rings) == 1 && half.rings->front().name == "edges",
          "a storage alone is no ring");
    CHECK(errorOf(answerOf(rf::run(recordsCase("records_no_kinds")).answer))
            .starts_with("records: name the kinds"),
          "no kinds: an error");

    auto const raised = rf::run(recordsCase("raise_panic_check_failed"));
    auto const answer = answerOf(raised.answer);
    auto const regs   = uc_log::test::RecordsFixture::load().registers;
    CHECK(as<ctl::RaisePanicAnswer>(answer) != nullptr
            && as<ctl::RaisePanicAnswer>(answer)->cause == 9
            && as<ctl::RaisePanicAnswer>(answer)->name == "check failed",
          "raise_panic: the cause by its argument name");
    using W      = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    namespace cm = uc_log::printer::cortex_m;
    W const expected{
      {  cm::regR0,                                                           9U},
      {  cm::regLr,                                  (regs[cm::regPc] + 2U) | 1U},
      {cm::regXpsr, regs[cm::regXpsr] & ~uc_log::printer::panic_raise::epsrItIci},
      {  cm::regPc,                                                 0x1000'0148U}
    };
    CHECK(
      raised.registerWrites == expected && !raised.haltedAfter,
      "R0 cause, LR a call from the halted PC, xPSR without IT/ICI, PC raise; the core runs again");
    auto const user = answerOf(rf::run(recordsCase("raise_panic_user")).answer);
    CHECK(as<ctl::RaisePanicAnswer>(user) != nullptr
            && as<ctl::RaisePanicAnswer>(user)->name == "KVASIR_PANIC",
          "user is KVASIR_PANIC");
    auto const byRecordName = answerOf(rf::run(recordsCase("raise_panic_record_name")).answer);
    CHECK(as<ctl::RaisePanicAnswer>(byRecordName) != nullptr
            && as<ctl::RaisePanicAnswer>(byRecordName)->cause == 16,
          "a cause by its record name");
    auto const unknown = rf::run(recordsCase("raise_panic_unknown_cause"));
    CHECK(errorOf(answerOf(unknown.answer)).contains("check-failed")
            && unknown.registerWrites.empty(),
          "an unknown cause lists the names, nothing is written");
    CHECK(errorOf(answerOf(rf::run(recordsCase("raise_panic_no_raise")).answer))
            .contains("is not in the map"),
          "no raise() in the map");

    bool roundTrips = true;
    for(auto const& c : rf::cases()) {
        auto const line   = rf::run(c).answer + "\n";
        auto const parsed = ctl::fromLine<ctl::Answer>(line);
        roundTrips        = roundTrips && std::holds_alternative<ctl::Answer>(parsed)
                         && ctl::toLine(std::get<ctl::Answer>(parsed)) == line;
    }
    CHECK(roundTrips, "records answers round-trip through glaze");
}

std::string readFile(std::string const& path) {
    std::ifstream      in{path, std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}   // namespace

int main() {
    // the one request spelled out here: the transcript's ping is these bytes
    CHECK(request("ping") == R"({"cmd":"ping"})", "the transcript's ping is the protocol's ping");
    CHECK(as<ctl::PingAnswer>(ask(R"({"cmd":"ping"})")) != nullptr, "ping answers");

    auto const one = askNamed("read_one");
    CHECK(as<ctl::ReadAnswer>(one) != nullptr
            && as<ctl::ReadAnswer>(one)->data == std::vector<std::string>{"10111213"}
            && as<ctl::ReadAnswer>(one)->unixUs == fx::FakeTarget::unixMicros,
          "one piece: hex bytes, and the time they came back");
    auto const two = askNamed("read_two");
    CHECK(as<ctl::ReadAnswer>(two) != nullptr
            && (as<ctl::ReadAnswer>(two)->data == std::vector<std::string>{"feff00", "10"}),
          "two pieces in request order");
    CHECK(errorOf(askNamed("read_nothing")) == "read: nothing to read", "nothing to read");
    CHECK(errorOf(askNamed("read_zero")) == "read: a piece of 0 bytes", "size 0");
    CHECK(errorOf(askNamed("read_too_much")).starts_with("read: too much"), "more than 4096 bytes");
    {
        fx::FakeTarget fake;
        auto const     before = fake.counter.load();
        (void)uc_log::printer::controlAnswerLine(
          R"({"cmd":"read","pieces":[{"address":805306368,"size":5000}]})",
          fake.target());
        CHECK(fake.counter.load() == before, "a refused request never reaches the target");
    }
    CHECK(errorOf(askNamed("read_disconnected")) == "the target is not connected",
          "the reader's error is passed on");

    auto const changed = askNamed("wait_changed");
    CHECK(as<ctl::WaitAnswer>(changed) != nullptr && as<ctl::WaitAnswer>(changed)->hit
            && as<ctl::WaitAnswer>(changed)->first == 1 && as<ctl::WaitAnswer>(changed)->last == 2,
          "wait changed");
    auto const reached = askNamed("wait_eq");
    CHECK(as<ctl::WaitAnswer>(reached) != nullptr && as<ctl::WaitAnswer>(reached)->last == 20,
          "wait eq");
    auto const masked = askNamed("wait_mask");
    CHECK(as<ctl::WaitAnswer>(masked) != nullptr && as<ctl::WaitAnswer>(masked)->hit
            && as<ctl::WaitAnswer>(masked)->last == 0,
          "wait with a mask");
    auto const never = askNamed("wait_timeout");
    CHECK(as<ctl::WaitAnswer>(never) != nullptr && !as<ctl::WaitAnswer>(never)->hit
            && as<ctl::WaitAnswer>(never)->waitedUs >= 30'000
            && as<ctl::WaitAnswer>(never)->last == 50462976,
          "a wait that runs out says so, with the last value");
    CHECK(errorOf(askNamed("wait_too_wide")) == "wait: a value is 1 to 8 bytes",
          "more than 8 bytes is not a number");
    CHECK(errorOf(askNamed("wait_no_value")) == "wait: this condition needs a value",
          "eq needs a value");

    auto const messages = askNamed("messages_2");
    CHECK(as<ctl::MessagesAnswer>(messages) != nullptr
            && as<ctl::MessagesAnswer>(messages)->messages.size() == 2
            && as<ctl::MessagesAnswer>(messages)->messages.back().level == "tool",
          "messages: the newest, oldest first");
    auto const status = askNamed("status");
    CHECK(as<ctl::StatusAnswer>(status) != nullptr
            && as<ctl::StatusAnswer>(status)->firmware.state == ctl::FirmwareState::match,
          "status");
    CHECK(as<ctl::ResetAnswer>(askNamed("reset")) != nullptr, "reset");
    {   // "line": true goes to the reset-pin action; false or no "line" to the ordinary one
        fx::FakeTarget fake;
        auto const     reset = [&](std::string_view line) {
            auto const parsed
              = ctl::fromLine<ctl::Answer>(uc_log::printer::controlAnswerLine(line, fake.target()));
            auto const* a = std::get_if<ctl::Answer>(&parsed);
            return a != nullptr && as<ctl::ResetAnswer>(*a) != nullptr;
        };
        CHECK(reset(request("reset_line")) && fake.lineResets == 1,
              "reset with line: through the reset pin");
        CHECK(reset(R"({"cmd":"reset","line":false})") && reset(request("reset"))
                && fake.lineResets == 1,
              "reset without line: the ordinary reset");
    }
    {
        auto const written = askNamed("write_two");   // 0 and 0x12345678
        CHECK(as<ctl::WriteAnswer>(written) != nullptr
                && as<ctl::WriteAnswer>(written)->data
                     == (std::vector<std::string>{"00000000", "78563412"}),
              "words written and read back, little endian, in request order");
        CHECK(errorOf(askNamed("write_unaligned")) == "write: 0x5000000e is not word aligned",
              "a write off a word boundary is refused");
        CHECK(errorOf(askNamed("write_nothing")) == "write: nothing to write", "nothing to write");
    }
    CHECK(errorOf(askNamed("flash")) == "no hex file", "a failed flash says why");
    {
        auto const halted = askNamed("halt");
        CHECK(as<ctl::HaltAnswer>(halted) != nullptr && as<ctl::HaltAnswer>(halted)->halted,
              "halt: halted after");
        auto const running = askNamed("go");
        CHECK(as<ctl::GoAnswer>(running) != nullptr && !as<ctl::GoAnswer>(running)->halted,
              "go: running after");
    }
    {
        uc_log::printer::ControlTarget const bare{};
        auto const                           answer
          = ctl::fromLine<ctl::Answer>(uc_log::printer::controlAnswerLine(request("flash"), bare));
        CHECK(std::holds_alternative<ctl::Answer>(answer)
                && std::holds_alternative<ctl::Error>(std::get<ctl::Answer>(answer)),
              "no flash without a handler");
    }
    CHECK(errorOf(askNamed("records_no_map")).starts_with("records: no map file"),
          "records without a map: an error");
    CHECK(errorOf(askNamed("raise_panic_no_map")).starts_with("raise_panic: no map file"),
          "raise_panic without a map: an error");
    CHECK(!errorOf(askNamed("records_unknown_kind")).empty(), "an unknown record kind");
    recordsOverTheProtocol();
    glazeOptionsByName();
    CHECK(!errorOf(askNamed("not_json")).empty(), "a line that is not JSON is an error");
    CHECK(!errorOf(askNamed("unknown_cmd")).empty(), "an unknown command");
    CHECK(!errorOf(askNamed("unknown_key")).empty(), "an unknown key");

    {
        std::string nasty;
        for(int b = 0; b != 256; ++b) { nasty += static_cast<char>(b); }
        nasty += "\xE2\x82";                           // cut off in the middle of a character
        nasty += "\xED\xA0\x80\xC0\xAF";               // a surrogate, an overlong '/'
        nasty += "ok \xE2\x84\x83 \xF0\x9F\x98\x80";   // and real UTF-8 stays
        auto const line  = ctl::toLine(ctl::Event{
          ctl::LogLine{.recvTime = "",
                       .channel  = 0,
                       .file     = "",
                       .line     = 0,
                       .function = "",
                       .level    = ctl::Level::info,
                       .ucTimeNs = 0,
                       .message  = nasty,
                       .module   = ""}
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

    CHECK(uc_log::printer::moduleUnder("i2c.bus", "i2c")
            && uc_log::printer::moduleUnder("i2c", "i2c")
            && !uc_log::printer::moduleUnder("i2cx", "i2c")
            && !uc_log::printer::moduleUnder("i2c.bus", "i2c.b"),
          "a module is under its prefixes, dot by dot");

    {
        bool roundTrips = true;
        for(auto const& x : fx::exchanges()) {
            fx::FakeTarget fake;
            auto const     line   = uc_log::printer::controlAnswerLine(x.request, fake.target());
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
        using uc_log::printer::ControlHistory;
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
                out.streams  = streams;
                out.minLevel = minLevel;
                out.modules  = modules;
                out.sinceSeq = since;
                out.last     = last;
                out.follow   = follow;
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
            CHECK(text.contains(R"("seq":)" + std::to_string(i)),
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
        auto const afterDrop  = history.read(all, 0, SIZE_MAX);
        auto       oldestLine = std::to_string(history.firstSeq());   // "<seq>:<seq>"
        oldestLine += ':';
        oldestLine += std::to_string(history.firstSeq());
        CHECK(afterDrop.lost == history.firstSeq() && afterDrop.atEnd
                && logged(afterDrop.text).front() == oldestLine,
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

    auto goldens = fx::files();
    goldens.merge(rf::files());
    for(auto const& [name, generated] : goldens) {
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
        ++uc_log::test::failures;
    }

    return uc_log::test::report();
}
