// The TUI's target inspectors: map symbols (lld and GNU ld), the record decoders, and the worker
// against a fake target - what kvasir_bench.py crash/ub/stack/trace/peek/profile/panic read.
#include "uc_log/detail/MapSymbols.hpp"
#include "uc_log/detail/TargetInspector.hpp"
#include "uc_log/detail/TargetRecords.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

using namespace uc_log::detail;

// the shape of a real lld map (test_examples 01_blink_debug.map, water_mix release.map)
static constexpr char const* LldMap
  = "     VMA      LMA     Size Align Out     In      Symbol\n"
    "10000000 10000000     1a18     4 .text\n"
    "100001c4 100001c4       24     4         "
    "x.lto.o:(.text._ZN6Kvasir4Nvic11DefaultIsrs5onIsrEv)\n"
    "100001c4 100001c4        0     1                 $t\n"
    "100001c5 100001c5       24     1                 Kvasir::Nvic::DefaultIsrs::onIsr()\n"
    "100001e8 100001e8      424     4         x.lto.o:(.text.ResetISR)\n"
    "100001e9 100001e9      424     1                 ResetISR\n"
    "10000149 10000149       12     1                 Kvasir::Panic::raise(Kvasir::Panic::Cause)\n"
    "1000a884 1000a884       17     1                 Kvasir::Trace::Ring<X, 4u, Y>::layout\n"
    "20000000 20000000     1020     4 .stack\n"
    "20000000 20000000        0     1         _LINKER_INTERN_stack_start_ = .\n"
    "20000020 20000020     1000     1         . = . + (cmake_min_stack_size)\n"
    "20000040 20000040        0     1         _LINKER_INTERN_stack_end_ = .\n"
    "20000040 20000040        0     1 .stack1\n"
    "20000040 20000040        0     1         _LINKER_INTERN_stack1_start_ = .\n"
    "20000040 20000040        0     1         _LINKER_INTERN_stack1_end_ = .\n"
    "20000100 20000100      100     4 .bss\n"
    "20000100 20000100        8     1                 Kvasir::Ubsan::ubsanReports\n"
    "20000108 20000108        4     1                 counter\n"
    "2000010c 2000010c       30     1                 Kvasir::Trace::Ring<X, 4u, Y>::storage\n"
    "200012bc 200012bc     8044     4 .noInit\n"
    "200012bc 200012bc       14     1                 Kvasir::Panic::lastPanic\n"
    "200012d4 200012d4       2c     1                 Kvasir::Fault::lastFault\n"
    "20009300 20009300        0     1 _LINKER_stack_start_ = _LINKER_INTERN_stack_start_\n"
    "20009300 20009300        0     1 _LINKER_stack_end_ = _LINKER_INTERN_stack_end_\n"
    "20009300 20009300        0     1 _LINKER_stack1_start_ = c ? a : b\n";

static void mapTests() {
    std::istringstream in{LldMap};
    auto const         m = MapSymbols::parse(in);
    CHECK(m.fromLld(), "lld map recognised");
    CHECK(m.find("$t") == nullptr, "mapping symbols left out");
    auto const* reset = m.find("ResetISR");
    CHECK(reset && reset->code && reset->start() == 0x100001e8U && reset->size == 0x424,
          "ResetISR");
    auto const* fault = m.find("Kvasir::Fault::lastFault");
    CHECK(fault && !fault->code && fault->address == 0x200012d4U && fault->size == 0x2c,
          "lastFault: address and size");
    CHECK(m.value("_LINKER_stack_start_") == 0x20000000U, "stack start through the alias");
    CHECK(m.value("_LINKER_stack_end_") == 0x20000040U, "stack end: `= .` gives the VMA");
    CHECK(!m.value("_LINKER_stack1_start_"), "a conditional assignment has no value");
    CHECK(m.describe(0x100001e9U + 0x10) == "ResetISR+0x10", "describe with offset");
    CHECK(m.describe(0x100001c5U) == "Kvasir::Nvic::DefaultIsrs::onIsr()", "describe a start");
    CHECK(m.describe(0x20000100U).empty(), "data is no function");
    CHECK(m.describe(0x10000000U).empty(), "before every function");

    // GNU ld: sizes from the input-section line
    std::istringstream gnu{
      "Linker script and memory map\n"
      ".bss            0x20000000      0x100\n"
      " .bss._ZN6Kvasir5Ubsan12ubsanReportsE\n"
      "                0x20000010        0x8 main.o\n"
      "                0x20000010                Kvasir::Ubsan::ubsanReports\n"
      ".text           0x10000000     0x1000\n"
      " .text.main     0x10000100       0x40 main.o\n"
      "                0x10000101                main\n"};
    auto const g = MapSymbols::parse(gnu);
    CHECK(!g.fromLld(), "GNU ld map");
    auto const* ub = g.find("Kvasir::Ubsan::ubsanReports");
    CHECK(ub && ub->address == 0x20000010U && ub->size == 8 && !ub->code, "GNU ld: data symbol");
    auto const* main = g.find("main");
    CHECK(main && main->code && main->size == 0x40, "GNU ld: function");
    CHECK(g.describe(0x10000110U) == "main+0x10", "GNU ld: describe");
}

static std::vector<std::byte> words(std::initializer_list<std::uint32_t> ws) {
    std::vector<std::byte> out;
    for(auto w : ws) {
        for(int i = 0; i != 4; ++i) {
            out.push_back(static_cast<std::byte>((w >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

static void recordTests() {
    using namespace records;
    auto const f
      = decodeFault(words({FaultMagic, 2, 0x1001, 0x2001, 0x3, 0xFFFFFFF9, 1, 2, 3, 4, 12}));
    CHECK(f && f->count == 2 && f->pc == 0x1001 && f->r[3] == 4 && f->r12 == 12
            && f->exception() == 3,
          "fault record");
    CHECK(!decodeFault(words({0, 2, 0x1001, 0x2001, 0x3, 0, 1, 2, 3, 4, 12})), "no magic: none");
    auto const p = decodePanic(words({PanicMagic, 1, 5, 0x10000101, 21}));
    CHECK(p && p->causeName() == "unhandled interrupt" && p->detail == 21U
            && p->site() == 0x100000FEU,
          "panic record, 20 bytes");
    auto const p16 = decodePanic(words({PanicMagic, 1, 0, 0}));
    CHECK(p16 && !p16->detail && !p16->site() && p16->causeName() == "assertion",
          "panic record, 16 bytes, no site");
    CHECK(decodePanic(words({PanicMagic, 1, 42, 0}))->causeName() == "cause 42", "unknown cause");
    CHECK(exceptionName(3) == "HardFault" && exceptionName(16 + 5) == "IRQ 5", "exception names");

    auto const s = stackUse(words({0x55AA55AA, StackPattern, StackPattern, 7, 8}));
    CHECK(s && s->size == 20 && s->never == 8 && s->used == 8, "stack: sentinel, 2 free, 2 used");
    CHECK(!stackUse(words({1, 2, 3})), "stack not painted");

    auto storage
      = words({TraceMagic, (4U << 16) | 2U, 6, 0x1000a884, 10, 11, 20, 21, 30, 31, 40, 41});
    auto const h = decodeRingHeader(storage);
    CHECK(h && h->fields == 2 && h->capacity == 4 && h->count == 6 && h->storageSize() == 48,
          "ring header");
    // count 6, capacity 4: records 2..5 kept, in slots 2,3,0,1
    auto rows = decodeRingRows(storage, *h, 6);
    CHECK(rows.size() == 4 && rows.front().index == 2 && rows.front().values[0] == 30
            && rows.back().index == 5 && rows.back().values[1] == 21,
          "ring rows oldest first");
    rows = decodeRingRows(storage, *h, 8);
    CHECK(rows.size() == 2 && rows.front().index == 4, "rows overwritten while reading dropped");
    auto const layout = decodeRingLayout(std::as_bytes(std::span{"button:us,level\0junk"}));
    CHECK(layout.name == "button" && layout.fields.size() == 2 && layout.fields[1] == "level",
          "ring layout");
}

// A target of words; DHCSR/DCRSR behave enough for raisePanic().
struct FakeTarget {
    std::mutex                             mutex;
    std::map<std::uint32_t, std::byte>     bytes;
    std::map<std::uint32_t, std::uint32_t> regs;   // DCRSR register file
    bool                                   halted{};
    std::uint32_t                          pcsr{0x100001f0U};
    std::atomic<int>                       calls{0};

    void put(std::uint32_t              address,
             std::span<std::byte const> data) {
        std::lock_guard<std::mutex> const lock{mutex};
        for(auto b : data) { bytes[address++] = b; }
    }

    std::uint32_t word(std::uint32_t address) {
        std::uint32_t v{};
        for(std::uint32_t i = 0; i != 4; ++i) {
            v |= std::to_integer<std::uint32_t>(bytes[address + i]) << (8 * i);
        }
        return v;
    }

    void setWord(std::uint32_t address,
                 std::uint32_t v) {
        for(std::uint32_t i = 0; i != 4; ++i) {
            bytes[address + i] = static_cast<std::byte>(v >> (8 * i));
        }
    }

    InspectorResult access(std::span<InspectorAccess const> accesses) {
        std::lock_guard<std::mutex> const lock{mutex};
        ++calls;
        std::vector<std::vector<std::byte>> out;
        for(auto const& a : accesses) {
            if(a.write) {
                if(a.address == 0xE000EDF0U) {
                    halted = (*a.write & 2U) != 0;
                } else if(a.address == 0xE000EDF4U) {
                    auto const reg = *a.write & 0x7FU;
                    if(*a.write & (1U << 16)) {
                        if(halted) { regs[reg] = word(0xE000EDF8U); }
                    } else {
                        setWord(0xE000EDF8U, regs[reg]);
                    }
                } else {
                    setWord(a.address, *a.write);
                }
            }
            if(a.address == 0xE000EDF0U) {
                setWord(a.address, (halted ? (1U << 17) : 0U) | (1U << 16));
            }
            if(a.address == 0xE000101CU) { setWord(a.address, halted ? 0xFFFFFFFFU : pcsr); }
            std::vector<std::byte> d;
            auto const             n = a.write ? 4U : a.size;
            for(std::uint32_t i = 0; i != n; ++i) { d.push_back(bytes[a.address + i]); }
            out.push_back(std::move(d));
        }
        return out;
    }
};

template<typename F>
static bool eventually(F&& f) {
    for(int i = 0; i != 300; ++i) {
        if(f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

static void inspectorTests() {
    auto const dir
      = std::filesystem::temp_directory_path() / ("uc_log_inspector_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    auto const mapPath = (dir / "fw.map").string();
    { std::ofstream{mapPath} << LldMap; }
    FakeTarget target;
    using namespace records;
    target.put(0x20000100U, words({3, 0x100001e9U + 0x20}));   // ubsanReports
    target.put(
      0x200012d4U,
      words({FaultMagic, 1, 0x100001c5U + 4, 0x100001e9U, 0x21000003, 0xFFFFFFF9, 0, 0, 0, 0, 0}));
    target.put(0x200012bcU, words({PanicMagic, 1, 0, 0x100001e9U + 6, 0}));
    // the stack: 0x20000000..0x20000040, sentinel + 9 painted + 6 used
    target.put(0x20000000U,
               words({0x55AA55AA,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      StackPattern,
                      1,
                      2,
                      3,
                      4,
                      5,
                      6}));
    // the ring: 2 fields, capacity 4, 3 records, layout in flash
    target.put(0x2000010cU,
               words({TraceMagic, (4U << 16) | 2U, 3, 0x1000a884U, 100, 1, 110, 0, 125, 1, 0, 0}));
    target.put(0x1000a884U, std::as_bytes(std::span{"button:us,level"}));
    target.put(0x20000108U, words({41}));   // counter

    std::atomic<int> changes{0};
    TargetInspector  inspector{
      TargetInspector::Hooks{.memory = [&](std::span<InspectorAccess const> a,
                             std::chrono::milliseconds) { return target.access(a); },
                             .connected = [] { return true; },
                             .sessions  = [] { return std::uint64_t{1}; },
                             .mapFile   = [&] { return mapPath; },
                             .changed   = [&] { ++changes; }}
    };

    CHECK(eventually([&] {
              auto const s = inspector.snapshot();
              return s.ubsan && s.fault && s.panic && !s.stacks.empty() && s.stacks[0].use;
          }),
          "ub, records and stack read at start");
    auto s = inspector.snapshot();
    CHECK(s.mapError.empty() && s.lldMap, "map read");
    CHECK(s.ubsan->count == 3 && s.ubsanWhere == "ResetISR+0x20", "ub count and where");
    CHECK(s.fault->count == 1 && s.faultPc == "Kvasir::Nvic::DefaultIsrs::onIsr()+0x4"
            && s.faultLr == "ResetISR",
          "fault record symbolised");
    CHECK(s.panic->causeName() == "assertion" && s.panicSite == "ResetISR+0x4", "panic site");
    CHECK(s.stacks.size() == 1 && s.stacks[0].use->size == 64 && s.stacks[0].use->used == 24
            && s.stacks[0].use->never == 36,
          "stack use (core 1's empty stack left out)");
    CHECK(s.rings.size() == 1 && s.rings[0].rows.empty(), "rings not read off their page");

    inspector.setPage(TargetInspector::Page::trace);
    CHECK(eventually([&] { return !inspector.snapshot().rings[0].rows.empty(); }), "ring read");
    s = inspector.snapshot();
    CHECK(s.rings[0].name == "button" && s.rings[0].fields.size() == 2 && s.rings[0].count == 3
            && s.rings[0].rows.size() == 3 && s.rings[0].rows[2].values[0] == 125,
          "ring decoded");

    CHECK(inspector.completions("count", 10) == std::vector<std::string>{"counter"},
          "completions: data symbols");
    CHECK(inspector.addWatch("cou.*er").empty(), "watch by regex");
    CHECK(!inspector.addWatch("Kvasir").empty(), "an ambiguous watch is refused");
    inspector.setPage(TargetInspector::Page::watch);
    CHECK(eventually([&] {
              auto const w = inspector.snapshot().watches;
              return w.size() == 1 && records::word(w[0].value, 0) == 41;
          }),
          "watch read");
    target.put(0x20000108U, words({42}));
    CHECK(eventually([&] {
              auto const w = inspector.snapshot().watches;
              return records::word(w[0].value, 0) == 42 && w[0].changes == 1;
          }),
          "watch sees a change");

    // profile: every sample in ResetISR
    target.put(0xE000EDFCU, words({1U << 24}));
    inspector.startProfile();
    CHECK(eventually([&] {
              auto const p = inspector.snapshot();
              return !p.profile.empty() && p.profileSamples >= 128;
          }),
          "profile samples");
    inspector.stopProfile();
    s = inspector.snapshot();
    CHECK(s.profile.front().name == "ResetISR" && s.profileError.empty(), "profile ranks ResetISR");

    // panic: halt, R0 = cause, xPSR without IT/ICI, PC = raise, resume
    target.regs[16] = 0x0100'2800U;   // a halt inside an IT block
    inspector.raisePanic(2);
    CHECK(eventually([&] { return !inspector.snapshot().panicRaise.empty(); }), "raise done");
    s = inspector.snapshot();
    CHECK(s.panicRaise.starts_with("raise(stack smash)"), s.panicRaise.c_str());
    CHECK(target.regs[0] == 2 && target.regs[15] == 0x10000148U && target.regs[16] == 0x0100'0000U
            && !target.halted,
          "panic registers written, core resumed");

    // a halt message
    inspector.noteHalt(
      "core halted: pc=0x100001f0 lr=0x100001c9 sp=0x20000030 xpsr=0x21000003 "
      "msp=0x20000030 psp=0x00000000 exception=3 stack: 00000001 100001ed ????????");
    s = inspector.snapshot();
    CHECK(s.halt && s.halt->exception == 3 && s.halt->pcWhere == "ResetISR+0x8"
            && s.halt->codeWords.size() == 1 && s.halt->codeWords[0].offset == 4,
          "halt parsed and symbolised");

    // a rebuilt image: the map moves the counter
    {
        std::string text{LldMap};
        text.replace(text.find("20000108 20000108"), 17, "20000104 20000104");
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        std::ofstream{mapPath} << text;
        std::filesystem::last_write_time(mapPath,
                                         std::filesystem::file_time_type::clock::now()
                                           + std::chrono::seconds{1});
    }
    CHECK(eventually([&] { return inspector.snapshot().watches[0].address == 0x20000104U; }),
          "watch follows the map");
    std::filesystem::remove_all(dir);
}

int main() {
    mapTests();
    recordTests();
    inspectorTests();
    if(failures == 0) { std::printf("all target inspector tests passed\n"); }
    return failures == 0 ? 0 : 1;
}
