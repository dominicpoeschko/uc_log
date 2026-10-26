#pragma once

// The `records` and `raise_panic` requests against the fixture target of tests/data/records: the golden file
// doc/control_protocol/records_fixture.jsonl, which control_protocol_tool writes and control_protocol_tests checks,
// and which Kvasir_SDK's tools/test_kvasir_bench.py feeds its text goldens from (one input for the C++ decoding and
// the text). Its cases are the text goldens' (a record left, records taken, symbols missing, a ring written while it
// is read), each a change of the fixture's bytes or map lines.

#include "support/FakeTargetPort.hpp"
#include "support/RecordsFixture.hpp"
#include "uc_log/printer/ControlAnswer.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/ControlRecords.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::records_fixture {

namespace ctl     = uc_log::printer::control;
namespace records = uc_log::printer::records;

/// The fixture as a halted-or-running core behind a TargetPort: memory from its regions, the halt's registers, every
/// register write kept. `countAfter`: what a read of that word alone gives (a ring's count read again after its
/// storage: records written while it was read).
struct Target {
    test::RecordsFixture                                   fixture = test::RecordsFixture::load();
    std::optional<std::pair<std::uint32_t, std::uint32_t>> countAfter;
    bool                                                   halted{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>>   registerWrites;   // (index, value)

    printer::TargetPort::Result access(std::span<printer::TargetOp const> ops) {
        namespace to = printer::target_op;
        printer::TargetPort::Result::value_type out;
        for(auto const& op : ops) {
            if(auto const* r = std::get_if<to::Read>(&op)) {
                if(countAfter && r->address == countAfter->first && r->size == 4) {
                    std::vector<std::byte> word;
                    printer::appendLe32(word, countAfter->second);
                    out.push_back(std::move(word));
                } else {
                    out.push_back(fixture.read(r->address, r->size));
                }
            } else if(std::holds_alternative<to::Halt>(op) || std::holds_alternative<to::Go>(op)) {
                halted = std::holds_alternative<to::Halt>(op);
                out.push_back({std::byte{halted ? std::uint8_t{1} : std::uint8_t{0}}});
            } else if(std::holds_alternative<to::ReadRegisters>(op)) {
                std::vector<std::byte> regs;
                for(auto const value : fixture.registers) { printer::appendLe32(regs, value); }
                out.push_back(std::move(regs));
            } else if(auto const* w = std::get_if<to::WriteRegister>(&op)) {
                if(!halted) { return std::unexpected{std::string{"the core runs"}}; }
                registerWrites.emplace_back(w->index, w->value);
                out.emplace_back();
            } else {
                return std::unexpected{std::string{"not a fixture op"}};
            }
        }
        return out;
    }

    test::FakeTargetPort port{
      [this](std::span<printer::TargetOp const> ops) { return access(ops); }};

    /// The bytes at `address` replaced (inside the regions).
    void put(std::uint32_t              address,
             std::span<std::byte const> bytes) {
        for(auto& r : fixture.memory) {
            for(std::size_t i = 0; i != bytes.size(); ++i) {
                auto const a = std::uint64_t{address} + i;
                if(a >= r.address && a < std::uint64_t{r.address} + r.hex.size() / 2) {
                    r.hex.replace(static_cast<std::size_t>(2 * (a - r.address)),
                                  2,
                                  fmt::format("{:02x}", std::to_integer<unsigned>(bytes[i])));
                }
            }
        }
    }

    /// A Persistent record of `value` (any whole-word struct, or the words themselves) as the target stores it: id,
    /// value, zlib crc32.
    template<typename T>
    void store(std::uint32_t address,
               std::uint32_t id,
               T const&      value) {
        std::vector<std::byte> raw;
        printer::appendLe32(raw, id);
        if constexpr(std::is_same_v<T, std::vector<std::uint32_t>>) {
            for(auto const w : value) { printer::appendLe32(raw, w); }
        } else {
            auto const bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
            raw.insert(raw.end(), bytes.begin(), bytes.end());
        }
        printer::appendLe32(raw, printer::crc32(raw));
        put(address, raw);
    }

    /// Zero the regions' bytes in [address, address + size).
    void clear(std::uint32_t address,
               std::size_t   size) {
        for(auto& r : fixture.memory) {
            auto const lo = std::max<std::uint64_t>(r.address, address);
            auto const hi = std::min<std::uint64_t>(std::uint64_t{r.address} + r.hex.size() / 2,
                                                    std::uint64_t{address} + size);
            if(lo < hi) {
                r.hex.replace(static_cast<std::size_t>(2 * (lo - r.address)),
                              static_cast<std::size_t>(2 * (hi - lo)),
                              std::string(static_cast<std::size_t>(2 * (hi - lo)), '0'));
            }
        }
    }
};

struct Case {
    std::string_view                                       name;
    std::string_view                                       request;
    std::string_view                                       map{"fixture.lld.map"};
    std::function<void(Target&)>                           change{};
    std::function<bool(std::string_view)>                  dropLine{};   // map lines left out
    std::optional<std::pair<std::uint32_t, std::uint32_t>> countAfter{};
};

inline constexpr std::string_view all
  = R"({"cmd":"records","kinds":["crash","health","ubsan","stacks","trace"]})";
inline constexpr std::string_view crashHealth = R"({"cmd":"records","kinds":["crash","health"]})";

inline bool has(std::string_view line,
                std::string_view what) {
    return line.contains(what);
}

/// Kvasir::Health::fnv1a: what a record keeps instead of a check's name.
constexpr std::uint32_t fnv1a(std::string_view text) {
    std::uint32_t h = 0x811C'9DC5U;
    for(char const c : text) { h = (h ^ static_cast<unsigned char>(c)) * 0x0100'0193U; }
    return h;
}

// where the fixture's image keeps its records (fixture.lld.map)
inline constexpr std::uint32_t faultAt      = 0x2000'6000U;
inline constexpr std::uint32_t stackAt      = 0x2000'6090U;
inline constexpr std::uint32_t panicAt      = 0x2000'60E4U;
inline constexpr std::uint32_t starvationAt = 0x2000'6110U;
inline constexpr std::uint32_t recoveriesAt = 0x2000'6134U;
inline constexpr std::uint32_t guardAt      = 0x2000'6158U;

inline void noInit(Target& t) { t.clear(0x2000'6000U, 0x170); }

// kvasir_bench's crash-record texts (test_kvasir_bench.py GOLDEN_1..5): a fault with a frame or without, the stack
// snapshot, panics with every kind of detail, site and time
inline records::Fault goldenFault(std::uint32_t flags) {
    return records::Fault{
      .count  = 2,
      .flags  = flags,
      .r      = {1, 0, 0, 0},
      .lr     = 0x1000'0201U,
      .pc     = 0x1000'0124U,
      .sp     = 0x2000'1F00U,
      .cfsr   = 0x8200U,
      .bfar   = 0x4000'0000U,
      .msplim = 0x2000'0000U
    };
}

inline records::Panic goldenPanic(std::uint32_t cause,
                                  std::uint32_t detail,
                                  std::uint32_t pc,
                                  std::uint64_t timeUs = 1234) {
    return records::Panic{.count    = 1,
                          .cause    = cause,
                          .pc       = pc,
                          .detail   = detail,
                          .core     = 1,
                          .sp       = 0x2000'1E00U,
                          .timeLow  = static_cast<std::uint32_t>(timeUs & 0xFFFF'FFFFU),
                          .timeHigh = static_cast<std::uint32_t>(timeUs >> 32U)};
}

/// lastStack<64>: from, bytes, 16 words - 16 bytes of them valid, the rest 0.
inline void goldenStack(Target& t) {
    std::vector<std::uint32_t> words{0x2000'1F00U, 16, 0x1000'0201U, 7, 0x1000'0125U, 0x2000'0000U};
    words.resize(2 + 16);
    t.store(stackAt, records::stackSnapshotId, words);
}

inline records::GuardState guard(std::uint32_t boot,
                                 std::uint16_t consecutive,
                                 std::uint16_t total,
                                 std::uint8_t  failure,
                                 std::uint8_t  detail,
                                 std::uint8_t  flags) {
    return records::GuardState{.bootCount   = boot,
                               .consecutive = consecutive,
                               .total       = total,
                               .lastFailure = failure,
                               .lastDetail  = detail,
                               .flags       = flags};
}

inline std::vector<Case> cases() {
    using records::faultId, records::panicId, records::starvationId, records::recoveriesId,
      records::guardId;
    auto const crash  = std::string_view{R"({"cmd":"records","kinds":["crash"]})"};
    auto const health = std::string_view{R"({"cmd":"records","kinds":["health"]})"};
    return {
      {.name = "records_lld", .request = all},
      {.name = "records_gnu", .request = all, .map = "fixture.gnu.map"},
      {.name = "records_crash", .request = R"({"cmd":"records","kinds":["crash"]})"},
      {.name    = "records_crash_health_empty",
       .request = crashHealth,
       .change  = [](Target& t) { noInit(t); }},
      {.name     = "records_crash_health_no_symbols",
       .request  = crashHealth,
       .dropLine = [](std::string_view l) { return has(l, "::last") || has(l, "BootGuard"); }},
      {.name    = "records_stacks_not_painted",
       .request = R"({"cmd":"records","kinds":["stacks"]})",
       .change  = [](Target& t) { t.clear(0x2000'0000U, 0x800); }},
      {.name    = "records_stacks_no_symbols",
       .request = R"({"cmd":"records","kinds":["stacks"]})",
       .dropLine =
         [](std::string_view l) {
             return has(l, "_LINKER_stack") || has(l, "_LINKER_INTERN_stack");
         }},
      {.name    = "records_ubsan_none",
       .request = R"({"cmd":"records","kinds":["ubsan"]})",
       .change  = [](Target& t) { t.clear(0x2000'1000U, 8); }},
      {.name     = "records_ubsan_not_sanitize",
       .request  = R"({"cmd":"records","kinds":["ubsan"]})",
       .dropLine = [](std::string_view l) { return has(l, "ubsanReports"); }},
      {.name       = "records_trace_written_while_reading",
       .request    = R"({"cmd":"records","kinds":["trace"]})",
       .countAfter = std::pair{0x2000'1010U, 1503U}},
      // 6 more while reading: the records 0..2 may be overwritten, they are dropped
      {.name       = "records_trace_written_while_reading_drops",
       .request    = R"({"cmd":"records","kinds":["trace"]})",
       .countAfter = std::pair{0x2000'5020U, 11U}},
      {.name    = "records_trace_no_rings",
       .request = R"({"cmd":"records","kinds":["trace"]})",
       .dropLine
       = [](std::string_view l) { return has(l, R"({"edges"})") || has(l, R"({"isr"})"); }},
      {.name    = "records_trace_a_storage_without_its_descriptor",
       .request = R"({"cmd":"records","kinds":["trace"]})",
       .dropLine
       = [](std::string_view l) { return l.ends_with("descriptor") && has(l, R"({"isr"})"); }},
      {.name = "records_no_kinds", .request = R"({"cmd":"records","kinds":[]})"},
      {.name    = "crash_golden_1",
       .request = crash,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(faultAt, faultId, goldenFault(1U | 2U << 8U | 8U << 12U));
             goldenStack(t);
             t.store(panicAt, panicId, goldenPanic(5, 42, 0x1000'0301U, 1ULL << 32U | 5U));
         }},
      {.name    = "crash_golden_2",
       .request = crash,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(faultAt, faultId, goldenFault(2));
             goldenStack(t);
             t.store(panicAt, panicId, goldenPanic(9, 3, 0x1000'0301U));
         }},
      {.name    = "crash_golden_3",
       .request = crash,
       .change =
         [](Target& t) {
             noInit(t);
             goldenStack(t);
             t.store(panicAt, panicId, goldenPanic(1, 0, 0, 0));
         }},
      {.name    = "crash_golden_4",
       .request = crash,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(panicAt, panicId, goldenPanic(99, 0, 0x1000'0301U));
         }},
      // the health and boot-guard texts (test_kvasir_bench.py HealthRecords)
      {.name    = "health_a_starvation",
       .request = health,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(starvationAt,
                     starvationId,
                     std::vector<std::uint32_t>{1, fnv1a("engine"), 812, 500, 4321, 0x6});
         }},
      {.name    = "health_a_reset_inside_on_starved",
       .request = health,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(
               recoveriesAt,
               recoveriesId,
               std::vector<std::uint32_t>{2, 3, fnv1a("core1"), 9000, (3U + 1U) | (2U << 16U), 0});
             t.store(guardAt, guardId, guard(7, 1, 4, 4, 3, 1 | 16));
         }},
      {.name    = "health_an_index_without_a_hash",
       .request = health,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(starvationAt,
                     starvationId,
                     std::vector<std::uint32_t>{0, fnv1a("renamed"), 1, 1, 1, 0});
             t.store(recoveriesAt,
                     recoveriesId,
                     std::vector<std::uint32_t>{0, 0, 0, 0, (5U + 1U) | (1U << 16U), 1});
         }},
      {.name    = "health_an_external_reset",
       .request = health,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(guardAt, guardId, guard(12, 3, 5, 5, 0, 1));
         }},
      {.name    = "health_a_panic_in_the_guard",
       .request = health,
       .change =
         [](Target& t) {
             noInit(t);
             t.store(guardAt, guardId, guard(3, 2, 2, 1, 2, 1));
         }},
      {.name    = "raise_panic_check_failed",
       .request = R"({"cmd":"raise_panic","cause":"check-failed"})"},
      {.name = "raise_panic_user", .request = R"({"cmd":"raise_panic","cause":"user"})"},
      {.name    = "raise_panic_record_name",
       .request = R"({"cmd":"raise_panic","cause":"stack overflow"})"},
      {.name = "raise_panic_unknown_cause", .request = R"({"cmd":"raise_panic","cause":"nope"})"},
      {.name     = "raise_panic_no_raise",
       .request  = R"({"cmd":"raise_panic","cause":"user"})",
       .dropLine = [](std::string_view l) { return has(l, "Kvasir::Panic::raise"); }},
    };
}

/// The case's map: the fixture's file without the dropped lines, parsed.
inline printer::ControlMap mapOf(Case const& c) {
    std::istringstream in{test::RecordsFixture::text(std::string{c.map})};
    std::string        kept;
    for(std::string line; std::getline(in, line);) {
        if(c.dropLine && c.dropLine(line)) { continue; }
        kept += line;
        kept += '\n';
    }
    std::istringstream text{kept};
    return printer::ControlMap{.snapshot = std::make_shared<printer::MapSnapshot const>(
                                 printer::MapSnapshot{printer::MapSymbols::parse(text), {}}),
                               .path       = std::string{c.map},
                               .generation = 1};
}

struct Outcome {
    std::string answer;   // the answer line, without its newline
    std::vector<std::pair<std::uint32_t, std::uint32_t>> registerWrites;
    bool                                                 haltedAfter{};
};

inline Outcome run(Case const& c) {
    Target target;
    target.countAfter = c.countAfter;
    if(c.change) { c.change(target); }
    auto const map = mapOf(c);
    auto       line
      = printer::controlAnswerLine(c.request,
                                   printer::ControlTarget{.target = &target.port, .map = [&map] {
                                                              return printer::ControlMap{map};
                                                          }});
    if(!line.empty() && line.back() == '\n') { line.pop_back(); }
    return {std::move(line), target.registerWrites, target.halted};
}

inline std::string jsonString(std::string_view text) {
    std::string out;
    (void)glz::write_json(text, out);
    return out;
}

/// records_fixture.jsonl: one line per case - its name, the map it read, the request, the answer, and for a
/// raise_panic the register writes the core got (index, value).
inline std::string transcript() {
    std::string out;
    for(auto const& c : cases()) {
        auto const  o = run(c);
        std::string writes;
        for(auto const& [index, value] : o.registerWrites) {
            writes += fmt::format("{}[{},{}]", writes.empty() ? "" : ",", index, value);
        }
        out += fmt::format(R"({{"name":{},"map":{},"request":{},"answer":{}{}}})",
                           jsonString(c.name),
                           jsonString(c.map),
                           jsonString(c.request),
                           o.answer,
                           c.request.contains("raise_panic")
                             ? fmt::format(R"(,"register_writes":[{}])", writes)
                             : std::string{})
             + "\n";
    }
    return out;
}

/// The golden files of this fixture, beside ControlProtocolFixture.hpp's files().
inline std::map<std::string,
                std::string>
files() {
    return {
      {"records_fixture.jsonl", transcript()},
    };
}

}   // namespace uc_log::records_fixture
