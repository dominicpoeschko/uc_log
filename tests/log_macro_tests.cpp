// End-to-end test of the UC_LOG macros: log into a capturing ComBackend, decode with
// remote_fmt::parse, check the result as a LogEntry - the printer's path minus the RTT transport.
// Catches disagreements between the header the macros assemble, the wire encoding and the parser,
// which tests on LogEntry alone cannot. USE_UC_LOG and REMOTE_FMT_USE_CATALOG come from the target.
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/metric_utils.hpp"
#include "uc_log/uc_log.hpp"

#if REMOTE_FMT_USE_MP_UNITS
    #include <mp-units/systems/si.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

#define CHECK_EQ(actual, expected, msg)                                     \
    do {                                                                    \
        auto const& actual_   = (actual);                                   \
        auto const& expected_ = (expected);                                 \
        if(!(actual_ == expected_)) {                                       \
            std::printf("FAIL: %s: got \"%s\" expected \"%s\" (line %d)\n", \
                        msg,                                                \
                        std::string{actual_}.c_str(),                       \
                        std::string{expected_}.c_str(),                     \
                        __LINE__);                                          \
            ++failures;                                                     \
        }                                                                   \
    } while(0)

// Leaked on purpose: a namespace-scope vector needs a global and an exit-time destructor.
std::vector<std::byte>& captured() {
    static auto& bytes = *new std::vector<std::byte>{};
    return bytes;
}

// The clock the macros stamp every entry with; fixed so the header is comparable.
constexpr std::chrono::milliseconds LogTime{123};

}   // namespace

namespace uc_log {

template<>
struct ComBackend<Tag::User> {
    static void write(std::span<std::byte const> data) {
        captured().insert(captured().end(), data.begin(), data.end());
    }
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return LogTime; }
};

}   // namespace uc_log

// A backend counts as level-aware whatever level its write<> accepts.
namespace level_aware {
struct Plain {
    static void write(std::span<std::byte const>) {}
};

struct WarnAndUp {
    template<uc_log::LogLevel L>
        requires(L >= uc_log::LogLevel::warn)
    static void write(std::span<std::byte const>) {}
};

static_assert(!uc_log::detail::LevelAware<Plain>);
static_assert(uc_log::detail::LevelAware<WarnAndUp>);
}   // namespace level_aware

// Reflectable types to log. At global scope on purpose: glaze's type_name, which the @TYPENAME
// marker carries, includes the enclosing namespace, and the expectations below spell it out.
enum class Color : std::uint8_t { red, green, blue };

struct Point {
    int   x;
    float y;
};

struct Item {
    Point            position;
    Color            color;
    std::string_view name;
};

// Trivially copyable arguments are taken by value, so members of a packed struct - whose
// addresses may be misaligned - are loggable directly.
struct __attribute__((packed)) PackedFrame {
    std::uint8_t  header;
    std::uint32_t counter;
    float         value;
};

static_assert(alignof(PackedFrame) == 1);

struct Flags {
    std::uint8_t ready : 1;
    std::uint8_t error : 3;
};

// Mirrors chip's CDC-ACM LineCoding: a static inline packed object whose members are logged
// from inside a template, i.e. a dependent context.
struct __attribute__((packed)) LineCodingLike {
    std::uint32_t dwDTERate{9600};
    std::uint8_t  bCharFormat{};
    std::uint8_t  bParityType{};
    std::uint8_t  bDataBits{8};
};

namespace {

// This binary's own catalog, written next to it by the post-build step.
std::unordered_map<std::uint16_t,
                   std::string> const&
catalog() {
    // Leaked on purpose: avoids the global-constructor and exit-time-destructor warnings.
    static auto const& map = *new std::unordered_map<std::uint16_t, std::string>{[] {
        auto result = remote_fmt::parseStringConstantsFromJsonFile(UC_LOG_TEST_CATALOG_JSON);
        if(!result) {
            std::printf("FAIL: %s\n", result.error().c_str());
            return std::unordered_map<std::uint16_t, std::string>{};
        }
        return *std::move(result);
    }()};
    return map;
}

uc_log::detail::SignatureTable const& signatures() {
    static auto const& table = *new uc_log::detail::SignatureTable{[] {
        auto result = uc_log::detail::SignatureTable::fromJsonFile(UC_LOG_TEST_CATALOG_JSON);
        if(!result) {
            std::printf("FAIL: %s\n", result.error().c_str());
            return uc_log::detail::SignatureTable{};
        }
        return *std::move(result);
    }()};
    return table;
}

// Decodes everything the macros wrote since the last call and hands back the parsed entry.
std::optional<uc_log::detail::LogEntry> takeEntry() {
    auto const parsed
      = remote_fmt::parseMessage(std::span{captured()}, catalog(), [](std::string_view error) {
            std::printf("parser error: %.*s\n", static_cast<int>(error.size()), error.data());
        });
    bool const clean = parsed.remaining.empty() && parsed.discarded == 0;
    captured().clear();
    if(!parsed.message || !clean) { return std::nullopt; }
    return uc_log::detail::LogEntry{0, *parsed.message, signatures().find(parsed.catalogId)};
}

// Carries the log statement's own __LINE__ back to the check.
struct Logged {
    uc_log::detail::LogEntry entry;
    unsigned                 line;
};

std::optional<Logged> logPlainMessage() {
    UC_LOG_I("hello {} {}", 42, "world");
    unsigned const line = __LINE__ - 1;
    auto           e    = takeEntry();
    if(!e) { return std::nullopt; }
    return Logged{*e, line};
}

std::optional<uc_log::detail::LogEntry> logStruct() {
    UC_LOG_D("point {}", Point{1, 2.5f});
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logNestedStruct() {
    UC_LOG_W("item {}",
             Item{
               Point{-3, 0.5f},
               Color::green,
               "sensor"
    });
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logPackedMembers() {
    PackedFrame const frame{0xAA, 1234567, 2.5f};
    UC_LOG_I("counter {} value {}", frame.counter, frame.value);
    return takeEntry();
}

// Counts allocations, so the test below can prove that logging a non-trivially-copyable
// container never copies it: a copy of a non-empty vector must allocate.
int countingAllocations = 0;

template<typename T>
struct CountingAllocator {
    using value_type = T;

    CountingAllocator() = default;

    template<typename U>
    constexpr CountingAllocator(CountingAllocator<U> const&) {}

    T* allocate(std::size_t n) {
        ++countingAllocations;
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T*          p,
                    std::size_t n) {
        std::allocator<T>{}.deallocate(p, n);
    }

    friend bool operator==(CountingAllocator,
                           CountingAllocator) {
        return true;
    }
};

// Returns nullopt if the entry did not round trip, false if logging copied the vector.
std::optional<bool> logVectorWithoutCopy(std::optional<uc_log::detail::LogEntry>& entry) {
    std::vector<int, CountingAllocator<int>> const values{1, 2, 3};
    int const                                      allocationsBefore = countingAllocations;
    UC_LOG_I("values {}", values);
    bool const copyFree = countingAllocations == allocationsBefore;
    entry               = takeEntry();
    if(!entry) { return std::nullopt; }
    return copyFree;
}

// Nested braces, commas inside template arguments and inside initializers - the torture test
// for the macro's handling of __VA_ARGS__ (only parentheses protect commas from the
// preprocessor, so the expansion must never split the argument list).
std::optional<uc_log::detail::LogEntry> logBracedMap() {
    UC_LOG_I("map {}",
             std::map<int, int>{
               {1, 2},
               {3, 4}
    });
    return takeEntry();
}

template<typename>
struct MixinLike {
    static inline LineCodingLike lineCoding{};

    static std::optional<uc_log::detail::LogEntry> log() {
        lineCoding.dwDTERate = 115200;
        UC_LOG_I("baud {} bits {}", lineCoding.dwDTERate, lineCoding.bDataBits);
        return takeEntry();
    }
};

std::optional<uc_log::detail::LogEntry> logBitfieldMembers() {
    Flags const flags{1, 5};
    UC_LOG_I("ready {} error {}", flags.ready, flags.error);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logVolatileValue() {
    // stands in for a memory-mapped register read; the by-value copy drops the qualifier
    std::uint32_t volatile reg = 7;
    UC_LOG_I("reg {}", reg);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logCharBufferWithoutNul() {
    // no nul terminator anywhere: only formatter<char[N]>'s N-1 semantics can log this safely
    // (a strlen-based path would run off the end); the last element is not printed
    char const buf[5]{'h', 'e', 'l', 'l', 'o'};
    UC_LOG_I("buf {}", buf);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logByteArray() {
    std::uint8_t const mac[4]{1, 2, 3, 4};
    UC_LOG_I("mac {}", mac);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logWordArray() {
    std::uint32_t const values[3]{10, 20, 30};
    UC_LOG_I("values {}", values);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logVoidPointer() {
    // the shape kvasir's ubsan minimal runtime logs: an address, as void*
    void const* const address = reinterpret_cast<void const*>(std::uintptr_t{0x1234});
    UC_LOG_C("at {}", address);
    return takeEntry();
}

std::optional<uc_log::detail::LogEntry> logMetric() {
    using namespace sc::literals;
    UC_LOG_I("temperature {}", uc_log::metric<"temp"_sc, "C"_sc, "board"_sc>(42));
    return takeEntry();
}

#if REMOTE_FMT_USE_MP_UNITS
std::optional<uc_log::detail::LogEntry> logQuantityMetric() {
    using namespace sc::literals;
    namespace si = mp_units::si;
    UC_LOG_I("t {} rate {} water {}",
             uc_log::metric<"temp"_sc, "board"_sc>(mp_units::delta<si::milli<si::kelvin>>(23500)),
             uc_log::metric<"rate"_sc, "board"_sc>(mp_units::delta<si::milli<si::kelvin>>(-7)
                                                   / (1 * si::second)),
             uc_log::metric<"water"_sc>(mp_units::delta<si::deci<si::degree_Celsius>>(385)));
    return takeEntry();
}
#endif

template<uc_log::LogLevel Level>
std::optional<uc_log::detail::LogEntry> logAtLevel();

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::trace>() {
    UC_LOG_T("lvl");
    return takeEntry();
}

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::debug>() {
    UC_LOG_D("lvl");
    return takeEntry();
}

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::info>() {
    UC_LOG_I("lvl");
    return takeEntry();
}

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::warn>() {
    UC_LOG_W("lvl");
    return takeEntry();
}

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::error>() {
    UC_LOG_E("lvl");
    return takeEntry();
}

template<>
std::optional<uc_log::detail::LogEntry> logAtLevel<uc_log::LogLevel::crit>() {
    UC_LOG_C("lvl");
    return takeEntry();
}

template<uc_log::LogLevel Level>
void checkLevel(char const* what) {
    auto const entry = logAtLevel<Level>();
    if(!entry) {
        std::printf("FAIL: %s did not round trip\n", what);
        ++failures;
        return;
    }
    CHECK(entry->parsedOk, what);
    CHECK(entry->logLevel == Level, what);
    CHECK(entry->logMsg == "lvl", what);
}

// The firmware's filter derives the module from __PRETTY_FUNCTION__, the printer from the
// demangled tag: the two must agree.
std::string targetModule(std::string_view signature) {
    return std::string{
      uc_log::detail::moduleOf(uc_log::detail::scanSignature(signature), signature).view()};
}

namespace water_mix::control {
    template<typename T>
    struct Regulator {
        static std::optional<uc_log::detail::LogEntry> step(T value) {
            UC_LOG_I("step {}", value);
            return takeEntry();
        }

        static std::string module() { return targetModule(__PRETTY_FUNCTION__); }
    };
}   // namespace water_mix::control

namespace usb_driver {
    UC_LOG_SCOPE_MODULE("usb");

    std::optional<uc_log::detail::LogEntry> logInModule() {
        UC_LOG_W("stall on ep {}", 3);
        return takeEntry();
    }

    // an inner scope's module replaces the outer one; the other settings combine with it
    std::optional<uc_log::detail::LogEntry> logInInnerModule() {
        UC_LOG_ENV(UC_LOG_MODULE("usb/ep0"), ::uc_log::setting::MinLevel<uc_log::LogLevel::info>);
        UC_LOG_I("setup {}", 8);
        return takeEntry();
    }
}   // namespace usb_driver

}   // namespace

// clang glues the `*` of `static const char *valve::pointers::Table::name()` to the scope.
namespace valve::pointers {
struct Table {
    static char const* name() {
        UC_LOG_I("name");
        return "valve";
    }

    static int& counter() {
        static int value{};
        UC_LOG_I("counter");
        return value;
    }

    static std::string module() { return targetModule(__PRETTY_FUNCTION__); }
};
}   // namespace valve::pointers

int main() {
    // full header round trip: file, line, level, uc time and function name all survive
    {
        auto const logged = logPlainMessage();
        if(!logged) {
            std::printf("FAIL: plain message did not round trip\n");
            ++failures;
        } else {
            auto const& e = logged->entry;
            CHECK(e.parsedOk, "plain message parses");
            CHECK_EQ(e.fileName, std::string{"log_macro_tests.cpp"}, "file name");
            CHECK(e.line == logged->line, "line number");
            CHECK(e.logLevel == uc_log::LogLevel::info, "level");
            CHECK(e.ucTime.time == LogTime, "uc time");
            CHECK_EQ(e.functionName, std::string{"logPlainMessage"}, "function name");
            CHECK_EQ(e.logMsg, std::string{"hello 42 world"}, "message");
        }
    }

    {
        auto const e = usb_driver::logInModule();
        CHECK(e && e->parsedOk, "a module scope's line parses");
        if(e) {
            CHECK_EQ(e->module, std::string{"usb"}, "module");
            CHECK_EQ(e->functionName,
                     std::string{"usb_driver::logInModule"},
                     "function after the module");
            CHECK(e->logLevel == uc_log::LogLevel::warn, "level with a module");
            CHECK_EQ(e->logMsg, std::string{"stall on ep 3"}, "message with a module");
        }
        auto const inner = usb_driver::logInInnerModule();
        CHECK(inner && inner->parsedOk, "inner module parses");
        if(inner) {
            CHECK_EQ(inner->module, std::string{"usb/ep0"}, "inner module replaces outer");
        }

        auto const plain = logPlainMessage();
        CHECK(plain && plain->entry.module.empty(),
              "no module in an anonymous namespace at the top (nothing to derive)");

        auto const derived = water_mix::control::Regulator<int>::step(5);
        CHECK(derived && derived->parsedOk, "a derived module's line parses");
        if(derived) {
            CHECK_EQ(derived->module, std::string{"water_mix.control.regulator"}, "derived module");
            CHECK_EQ(derived->module,
                     water_mix::control::Regulator<int>::module(),
                     "the printer's module is the filter's");
            // the demangled tag: the class template's arguments with either compiler
            CHECK_EQ(derived->functionName,
                     std::string{"Regulator<int>::step"},
                     "function name: the class with its template arguments");
            CHECK_EQ(derived->logMsg, std::string{"step 5"}, "message with a derived module");
        }

        (void)valve::pointers::Table::name();
        auto const pointer = takeEntry();
        CHECK(pointer && pointer->parsedOk, "a pointer-returning function's line parses");
        if(pointer) {
            CHECK_EQ(pointer->module, std::string{"valve.pointers.table"}, "module under `T*`");
            CHECK_EQ(pointer->module,
                     valve::pointers::Table::module(),
                     "the filter's module under `T*`");
            CHECK_EQ(pointer->functionName, std::string{"Table::name"}, "function under `T*`");
        }
        (void)valve::pointers::Table::counter();
        auto const reference = takeEntry();
        CHECK(reference && reference->parsedOk, "a reference-returning function's line parses");
        if(reference) {
            CHECK_EQ(reference->module, std::string{"valve.pointers.table"}, "module under `T&`");
            CHECK_EQ(reference->functionName, std::string{"Table::counter"}, "function under `T&`");
        }
    }

    // one entry per level, so a mis-mapped macro cannot hide behind the others
    checkLevel<uc_log::LogLevel::trace>("trace");
    checkLevel<uc_log::LogLevel::debug>("debug");
    checkLevel<uc_log::LogLevel::info>("info");
    checkLevel<uc_log::LogLevel::warn>("warn");
    checkLevel<uc_log::LogLevel::error>("error");
    checkLevel<uc_log::LogLevel::crit>("crit");

    // aglio: a reflectable struct logs field by field, behind the @TYPENAME marker the GUI strips
    {
        auto const entry = logStruct();
        if(!entry) {
            std::printf("FAIL: struct did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "struct entry parses");
            CHECK_EQ(entry->logMsg,
                     std::string{"point @TYPENAME(Point){x: 1, y: 2.5}"},
                     "struct message");
        }
    }

    // nested struct, an enum (by name, via enchantum) and a string as fields
    {
        auto const entry = logNestedStruct();
        if(!entry) {
            std::printf("FAIL: nested struct did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "nested struct entry parses");
            CHECK_EQ(entry->logMsg,
                     std::string{"item @TYPENAME(Item){position: @TYPENAME(Point){x: -3, y: 0.5}, "
                                 "color: green, name: sensor}"},
                     "nested struct message");
        }
    }

    // char arrays keep their compile-time length: N-1 bytes, no nul needed
    {
        auto const entry = logCharBufferWithoutNul();
        if(!entry) {
            std::printf("FAIL: char buffer did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "char buffer entry parses");
            CHECK_EQ(entry->logMsg, std::string{"buf hell"}, "char buffer message");
        }
    }

    // the CDC-ACM shape: packed members of a static inline object, logged from a template
    {
        auto const entry = MixinLike<int>::log();
        if(!entry) {
            std::printf("FAIL: line coding did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "line coding entry parses");
            CHECK_EQ(entry->logMsg, std::string{"baud 115200 bits 8"}, "line coding message");
        }
    }

    // arrays log as ranges
    {
        auto const entry = logByteArray();
        if(!entry) {
            std::printf("FAIL: byte array did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "byte array entry parses");
            CHECK_EQ(entry->logMsg, std::string{"mac [1, 2, 3, 4]"}, "byte array message");
        }
    }
    {
        auto const entry = logWordArray();
        if(!entry) {
            std::printf("FAIL: word array did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "word array entry parses");
            CHECK_EQ(entry->logMsg, std::string{"values [10, 20, 30]"}, "word array message");
        }
    }

    // void pointers log as addresses (what kvasir's ubsan runtime relies on)
    {
        auto const entry = logVoidPointer();
        if(!entry) {
            std::printf("FAIL: void pointer did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "void pointer entry parses");
            CHECK_EQ(entry->logMsg, std::string{"at 0x1234"}, "void pointer");
        }
    }

    // packed struct members: taken by value (trivially copyable), so no reference ever binds
    // to the misaligned address and the load at the call site is well-defined
    {
        auto const entry = logPackedMembers();
        if(!entry) {
            std::printf("FAIL: packed members did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "packed member entry parses");
            CHECK_EQ(entry->logMsg, std::string{"counter 1234567 value 2.5"}, "packed members");
        }
    }

    // a non-trivially-copyable container is passed by reference: correct output, zero copies
    {
        std::optional<uc_log::detail::LogEntry> entry;
        auto const                              copyFree = logVectorWithoutCopy(entry);
        if(!copyFree) {
            std::printf("FAIL: vector did not round trip\n");
            ++failures;
        } else {
            CHECK(*copyFree, "logging the vector must not copy it");
            CHECK(entry->parsedOk, "vector entry parses");
            CHECK_EQ(entry->logMsg, std::string{"values [1, 2, 3]"}, "vector message");
        }
    }

    // braces and commas nested every which way must survive the macro expansion
    {
        auto const entry = logBracedMap();
        if(!entry) {
            std::printf("FAIL: braced map did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "braced map entry parses");
            CHECK_EQ(entry->logMsg, std::string{"map {1: 2, 3: 4}"}, "braced map message");
        }
    }

    // bit-field members: not addressable at all, but fine to log as values
    {
        auto const entry = logBitfieldMembers();
        if(!entry) {
            std::printf("FAIL: bit-field members did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "bit-field entry parses");
            CHECK_EQ(entry->logMsg, std::string{"ready 1 error 5"}, "bit-field members");
        }
    }

    // volatile lvalues: the copy reads once and logs the plain value
    {
        auto const entry = logVolatileValue();
        if(!entry) {
            std::printf("FAIL: volatile value did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "volatile entry parses");
            CHECK_EQ(entry->logMsg, std::string{"reg 7"}, "volatile value");
        }
    }

    // metric: the macro wraps the replacement field in @METRIC(...), extractMetrics finds it again
    {
        auto const entry = logMetric();
        if(!entry) {
            std::printf("FAIL: metric did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "metric entry parses");
            CHECK_EQ(entry->logMsg,
                     std::string{"temperature @METRIC(board::temp[C]=42)"},
                     "metric message");

            auto const metrics
              = uc_log::extractMetrics(std::chrono::system_clock::time_point{}, *entry);
            if(metrics.size() != 1) {
                std::printf("FAIL: expected 1 metric, got %zu\n", metrics.size());
                ++failures;
            } else {
                CHECK_EQ(metrics[0].first.scope, std::string{"board"}, "metric scope");
                CHECK_EQ(metrics[0].first.name, std::string{"temp"}, "metric name");
                CHECK_EQ(metrics[0].first.unit, std::string{"C"}, "metric unit");
                CHECK(metrics[0].second.value == 42.0, "metric value");
                CHECK(metrics[0].second.level == uc_log::LogLevel::info, "metric level");
                CHECK(metrics[0].second.uc_time.time == LogTime, "metric uc time");
            }
        }
    }

#if REMOTE_FMT_USE_MP_UNITS
    {
        auto const entry = logQuantityMetric();
        if(!entry) {
            std::printf("FAIL: quantity metric did not round trip\n");
            ++failures;
        } else {
            CHECK(entry->parsedOk, "quantity metric entry parses");
            // cataloged, so the unit is the quantity's own UTF-8 symbol (inline it is ASCII: ddegC)
            CHECK_EQ(entry->logMsg,
                     std::string{"t @METRIC(board::temp[mK]=23500) rate "
                                 "@METRIC(board::rate[mK/s]=-7) water @METRIC(::water[d℃]=385)"},
                     "quantity metric message: the unit is the quantity's");
            auto const metrics
              = uc_log::extractMetrics(std::chrono::system_clock::time_point{}, *entry);
            CHECK(metrics.size() == 3, "three quantity metrics");
            if(metrics.size() == 3) {
                CHECK_EQ(metrics[1].first.unit, std::string{"mK/s"}, "derived unit");
                CHECK(metrics[1].second.value == -7.0, "negative value");
            }
        }
    }
#else
    std::printf("note: built without mp-units, the quantity metric case is skipped\n");
#endif
    {
        // a unit may hold a parenthesis; the value ends at the one behind the '='
        std::string_view const msg{"c @METRIC(a::heat[J/(kg K)]=4182) and @METRIC(a::plain=3)"};
        auto const             first = uc_log::nextMetricMarker(msg, 0);
        CHECK(first.has_value(), "marker with a parenthesis in its unit");
        if(first) {
            CHECK_EQ(std::string{first->unit}, std::string{"J/(kg K)"}, "unit with parenthesis");
            CHECK_EQ(std::string{first->value}, std::string{"4182"}, "its value");
            auto const second = uc_log::nextMetricMarker(msg, first->end);
            CHECK(second.has_value(), "marker without a unit");
            if(second) {
                CHECK_EQ(std::string{second->name}, std::string{"plain"}, "its name");
                CHECK_EQ(std::string{second->value}, std::string{"3"}, "its value");
                CHECK(second->end == msg.size(), "ends behind the parenthesis");
            }
        }
    }

    if(failures != 0) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
