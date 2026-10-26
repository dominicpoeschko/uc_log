// printer/TargetRecords.hpp: every Persistent record's decoder (round trip, every bad bit, another id, cut short),
// the name tables and the size rules, and the fixture target's records found through both of its maps. The SDK's
// tests/records_cross_tests.cpp holds the layouts to the SDK's headers.
#include "support/Check.hpp"
#include "support/FakeTargetPort.hpp"
#include "support/RecordsFixture.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/RecordReader.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace records = uc_log::printer::records;

/// A Persistent<T> as the target holds it: id, T's bytes, zlib crc32 over both.
template<typename T>
constexpr std::array<std::byte,
                     records::recordSize<T>>
encode(T const&      value,
       std::uint32_t id) {
    std::array<std::byte, records::recordSize<T>> out{};
    auto const                                    put = [&](std::size_t at, std::uint32_t w) {
        for(std::size_t i = 0; i != 4; ++i) {
            out[at + i] = static_cast<std::byte>((w >> (8 * i)) & 0xFFU);
        }
    };
    put(0, id);
    auto const bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    for(std::size_t i = 0; i != sizeof(T); ++i) { out[4 + i] = bytes[i]; }
    put(4 + sizeof(T), uc_log::printer::crc32(std::span{out}.first(4 + sizeof(T))));
    return out;
}

// the round trip at compile time: decodeRecord is the struct's bytes
constexpr auto panicBytes = encode(records::Panic{.count    = 1,
                                                  .cause    = 9,
                                                  .pc       = 0x1000'0301U,
                                                  .detail   = 3,
                                                  .sp       = 0x2000'1E00U,
                                                  .timeHigh = 1},
                                   records::panicId);
static_assert(records::decodePanic(panicBytes)->cause == 9
              && records::decodePanic(panicBytes)->site() == 0x1000'02FEU
              && records::decodePanic(panicBytes)->timeUs() == 1ULL << 32U);
static_assert(!records::decodeFault(panicBytes),
              "another id");
constexpr auto guardBytes = encode(records::GuardState{.bootCount   = 7,
                                                       .consecutive = 1,
                                                       .total       = 4,
                                                       .lastFailure = 4,
                                                       .lastDetail  = 2,
                                                       .flags       = 17},
                                   records::guardId);
static_assert(records::decodeGuard(guardBytes)->total == 4
              && records::decodeGuard(guardBytes)->flags == 17);
static_assert(records::decodeRecoveries(encode(records::Recoveries{.inProgress
                                                                   = (2U + 1) | (2U << 16U)},
                                               records::recoveriesId))
                ->inProgressCheck()
              == 2);
constexpr auto faultBytes
  = encode(records::Fault{.flags = 1U | 1U << 8U | 8U << 12U, .pc = 0x1000'0124U, .psplim = 33},
           records::faultId);
static_assert(records::decodeFault(faultBytes)->core() == 1
              && records::decodeFault(faultBytes)->arch() == 8
              && records::decodeFault(faultBytes)->words()[8] == 0x1000'0124U
              && records::decodeFault(faultBytes)->words()[32] == 33);
static_assert(records::faultFieldNames[8] == "pc" && records::faultFieldNames[18] == "exc_return");

template<auto Decode>
bool decodes(std::span<std::byte const> raw) {
    return Decode(raw).has_value();
}

/// Every flipped bit, another id and a short copy are no record.
template<typename Decode>
bool rejectsDamage(std::span<std::byte const> raw,
                   std::uint32_t              otherId,
                   Decode                     decode) {
    std::vector<std::byte> copy{raw.begin(), raw.end()};
    if(!decode(std::span<std::byte const>{copy})) { return false; }
    for(std::size_t bit = 0; bit != 8 * copy.size(); ++bit) {
        copy[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        if(decode(std::span<std::byte const>{copy})) { return false; }
        copy[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    }
    auto other = copy;
    for(std::size_t i = 0; i != 4; ++i) {
        other[i] = static_cast<std::byte>((otherId >> (8 * i)) & 0xFFU);
    }
    if(decode(std::span<std::byte const>{other})) { return false; }
    return !decode(std::span<std::byte const>{copy}.first(copy.size() - 4));
}

void everyRecordRejectsDamage() {
    using namespace records;
    CHECK(rejectsDamage(encode(Fault{.count = 2, .pc = 0x1001}, faultId),
                        panicId,
                        decodes<decodeFault>),
          "fault");
    CHECK(rejectsDamage(panicBytes, faultId, decodes<decodePanic>), "panic");
    CHECK(rejectsDamage(encode(Starvation{.check = 1, .silentMs = 812}, starvationId),
                        recoveriesId,
                        decodes<decodeStarvation>),
          "starvation");
    CHECK(rejectsDamage(encode(Recoveries{.total = 2}, recoveriesId),
                        starvationId,
                        decodes<decodeRecoveries>),
          "recoveries");
    CHECK(rejectsDamage(guardBytes, starvationId, decodes<decodeGuard>), "boot guard");

    // lastStack<16>: from, bytes, 4 words
    std::vector<std::byte> stack;
    auto const             put = [&](std::uint32_t w) {
        for(std::size_t i = 0; i != 4; ++i) {
            stack.push_back(static_cast<std::byte>((w >> (8 * i)) & 0xFFU));
        }
    };
    for(auto w : {stackSnapshotId, 0x2000'0700U, 8U, 0x1000'0201U, 7U, 0U, 0U}) { put(w); }
    put(uc_log::printer::crc32(stack));
    CHECK(stack.size() == stackSnapshotSize(16), "lastStack<16>: 32 bytes");
    auto const s = decodeStackSnapshot(stack);
    CHECK(s && s->from == 0x2000'0700U && s->words.size() == 4 && s->body().size() == 2
            && s->body()[1] == 7,
          "stack snapshot: the copy and its valid words");
    CHECK(rejectsDamage(stack, faultId, decodes<decodeStackSnapshot>), "stack snapshot");
}

void namesAndSizes() {
    using namespace records;
    CHECK(stackSnapshotBytes("Kvasir::Fault::lastStack<(unsigned short)256>") == 256U,
          "N from an lld/GNU name");
    CHECK(stackSnapshotBytes("Kvasir::Fault::lastStack<64u>") == 64U, "N with a suffix");
    CHECK(stackSnapshotBytes("Kvasir::Fault::lastStack<63>") == std::nullopt,
          "N not whole words: none");
    CHECK(stackSnapshotBytes("Kvasir::Fault::lastStack<x>") == std::nullopt, "not a number: none");
    CHECK(stackSnapshotBytes("Kvasir::Fault::lastFault") == std::nullopt, "another symbol: none");
    CHECK(GuardState{.lastFailure = 5}.failureName() == "external"
            && GuardState{.lastFailure = 9}.failureName() == "failure 9",
          "guard failure names");
    CHECK(guardFlagNames[4] == "healthyThisRun" && faultFlagNames[6] == "stackCopied",
          "flag names");
}

/// The fixture target's records, found by name in a map - sizes from the types (a GNU map has none for them).
struct Found {
    std::optional<records::Fault>         fault;
    std::optional<records::StackSnapshot> stack;
    std::optional<records::Panic>         panic;
    std::optional<records::Starvation>    starvation;
    std::optional<records::Recoveries>    recoveries;
    std::optional<records::GuardState>    guard;
    std::optional<records::Ubsan>         ubsan;
};

Found foundThrough(std::string const& mapFile) {
    using namespace records;
    auto const         fixture = uc_log::test::RecordsFixture::load();
    std::istringstream text{uc_log::test::RecordsFixture::text(mapFile)};
    auto const         map = uc_log::printer::MapSymbols::parse(text);
    auto const         at  = [&](std::string_view name, std::uint32_t size) {
        auto const* s = map.find(name);
        return s ? fixture.read(s->address, size) : std::vector<std::byte>{};
    };
    Found f;
    f.fault      = decodeFault(at(faultSymbol, faultSize));
    f.panic      = decodePanic(at(panicSymbol, panicSize));
    f.starvation = decodeStarvation(at(starvationSymbol, recordSize<Starvation>));
    f.recoveries = decodeRecoveries(at(recoveriesSymbol, recordSize<Recoveries>));
    f.guard      = decodeGuard(at(guardSymbol, recordSize<GuardState>));
    f.ubsan      = decodeUbsan(at(ubsanSymbol, ubsanSize));
    for(auto const& e : map.entries()) {
        if(auto const n = stackSnapshotBytes(e.name)) {
            f.stack = decodeStackSnapshot(fixture.read(e.address, stackSnapshotSize(*n)));
        }
    }
    return f;
}

void fixtureRecords(std::string const& mapFile) {
    auto const f = foundThrough(mapFile);
    CHECKF(f.fault && f.fault->count == 2 && f.fault->pc == 0x1000'0124U && f.fault->arch() == 8
             && f.fault->core() == 0 && records::cfsrText(f.fault->cfsr) == "PRECISERR BFARVALID",
           "%s: the fault record",
           mapFile.c_str());
    CHECKF(f.stack && f.stack->from == 0x2000'0700U && f.stack->words.size() == 16
             && f.stack->body().size() == 4,
           "%s: the stack snapshot, lastStack<64>",
           mapFile.c_str());
    CHECKF(f.panic && f.panic->causeName() == "fault" && f.panic->timeUs() == 123'456'789U
             && f.panic->site() == 0x1000'02FEU,
           "%s: the panic record",
           mapFile.c_str());
    CHECKF(f.starvation && f.starvation->check == 1 && f.starvation->silentMs == 812,
           "%s: KHLT",
           mapFile.c_str());
    CHECKF(f.recoveries && f.recoveries->inProgressCheck() == 2
             && f.recoveries->inProgressAttempt() == 2,
           "%s: KHRV",
           mapFile.c_str());
    CHECKF(f.guard && f.guard->bootCount == 7 && f.guard->failureName() == "starvation"
             && f.guard->lastDetail == 2,
           "%s: KBTG",
           mapFile.c_str());
    CHECKF(f.ubsan && f.ubsan->count == 2 && f.ubsan->lastReturnAddress == 0x1000'0215U,
           "%s: ubsan",
           mapFile.c_str());
}

void fixtureThroughLld() { fixtureRecords("fixture.lld.map"); }

void fixtureThroughGnu() { fixtureRecords("fixture.gnu.map"); }

/// The fixture as a target behind a TargetPort: every access recorded; `countAfter` is what a read of the 'edges'
/// ring's count word alone gives (records written while the ring was read).
struct FixtureTarget {
    uc_log::test::RecordsFixture            fixture = uc_log::test::RecordsFixture::load();
    std::vector<std::vector<std::uint32_t>> accesses;   // the bytes of each read, per access
    std::optional<std::uint32_t>            countAfter;
    uc_log::test::FakeTargetPort port{[this](std::span<uc_log::printer::TargetOp const> ops) {
        uc_log::printer::TargetPort::Result::value_type out;
        auto&                                           sizes = accesses.emplace_back();
        for(auto const& op : ops) {
            auto const& read = std::get<uc_log::printer::target_op::Read>(op);
            sizes.push_back(read.size);
            if(countAfter && read.address == 0x2000'1010U && read.size == 4) {
                out.push_back(fixture.read(0, 0));
                for(std::size_t i = 0; i != 4; ++i) {
                    out.back().push_back(static_cast<std::byte>((*countAfter >> (8 * i)) & 0xFFU));
                }
                continue;
            }
            out.push_back(fixture.read(read.address, read.size));
        }
        return out;
    }};

    records::Read read(std::string const& mapFile,
                       records::Kinds     kinds) {
        std::istringstream text{uc_log::test::RecordsFixture::text(mapFile)};
        auto const         places = records::Places::of(uc_log::printer::MapSymbols::parse(text));
        auto reader = records::RecordReader::through(port, std::chrono::milliseconds{100});
        return reader.read(places, kinds, {});
    }
};

void readerThrough(std::string const& mapFile,
                   bool               rings) {
    FixtureTarget target;
    auto const    r = target.read(mapFile, records::Kinds::all());
    CHECKF(r.fault && r.fault->record && r.fault->record->pc == 0x1000'0124U && r.stack
             && r.stack->record && r.stack->record->body().size() == 4 && r.panic && r.panic->record
             && r.panic->record->cause == 7,
           "%s: the crash records",
           mapFile.c_str());
    CHECKF(r.starvation && r.starvation->record && r.recoveries && r.recoveries->record && r.guard
             && r.guard->record && r.guard->record->bootCount == 7,
           "%s: the health records",
           mapFile.c_str());
    CHECKF(r.ubsan && r.ubsan->record && r.ubsan->record->count == 2, "%s: ubsan", mapFile.c_str());
    CHECKF(r.stacks.size() == 2 && r.stacks[0].name == "core 0 stack"
             && r.stacks[0].use.value_or(records::StackUse{}).used == 608
             && r.stacks[1].name == "core 1 stack"
             && r.stacks[1].use.value_or(records::StackUse{}).used == 128,
           "%s: both stacks, core 1's through the _LINKER_INTERN_ names",
           mapFile.c_str());
    if(rings) {
        CHECKF(r.rings.size() == 2 && r.rings[0].name == "edges" && r.rings[0].rows.size() == 1024
                 && r.rings[0].rows.front().index == 476 && r.rings[0].fields.size() == 4
                 && r.rings[1].name == "isr" && r.rings[1].rows.size() == 5
                 && r.rings[1].error.empty(),
               "%s: both rings, the full-size one whole",
               mapFile.c_str());
    } else {
        CHECKF(r.rings.empty(), "%s: no ring descriptor in a GNU map: no ring", mapFile.c_str());
    }
    for(auto const& access : target.accesses) {
        std::uint32_t all = 0;
        for(auto const n : access) {
            CHECK(n <= records::RecordReader::pieceBytes,
                  "no piece over a control-socket read's limit");
            all += n;
        }
        CHECK(all <= records::RecordReader::callBytes,
              "no access over CallBytes: the RTT reads go on between");
    }
}

void readerThroughLld() { readerThrough("fixture.lld.map", true); }

void readerThroughGnu() { readerThrough("fixture.gnu.map", false); }

void readerKindsAndWhileReading() {
    FixtureTarget target;
    auto const    none = target.read("fixture.lld.map", {});
    CHECK(!none.fault && !none.ubsan && none.stacks.empty() && none.rings.empty()
            && target.accesses.empty(),
          "nothing asked: nothing read");
    target.countAfter = 1503;
    auto const r      = target.read("fixture.lld.map", {.trace = true});
    CHECK(!r.fault && r.rings.size() == 2 && r.rings[0].writtenWhileReading == 3
            && r.rings[0].rows.front().index == 479,
          "trace alone; three written while reading: the oldest three dropped");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {  "everyRecordRejectsDamage",   everyRecordRejectsDamage},
                               {             "namesAndSizes",              namesAndSizes},
                               {         "fixtureThroughLld",          fixtureThroughLld},
                               {         "fixtureThroughGnu",          fixtureThroughGnu},
                               {          "readerThroughLld",           readerThroughLld},
                               {          "readerThroughGnu",           readerThroughGnu},
                               {"readerKindsAndWhileReading", readerKindsAndWhileReading},
    });
}
