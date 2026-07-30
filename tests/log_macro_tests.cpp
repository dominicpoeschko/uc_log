// End-to-end test of the UC_LOG macros: log into a capturing ComBackend, decode with
// remote_fmt::parse, check the result as a LogEntry - the printer's path minus the RTT transport.
// Catches disagreements between the header the macros assemble, the wire encoding and the parser,
// which tests on LogEntry alone cannot. USE_UC_LOG and REMOTE_FMT_USE_CATALOG come from the target.
#include "remote_fmt/parser.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/metric_utils.hpp"
#include "uc_log/uc_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
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

namespace {

std::unordered_map<std::uint16_t,
                   std::string> const&
emptyCatalog() {
    // Leaked on purpose: avoids the global-constructor and exit-time-destructor warnings.
    static auto const& catalog = *new std::unordered_map<std::uint16_t, std::string>{};
    return catalog;
}

// Decodes everything the macros wrote since the last call and hands back the parsed entry.
std::optional<uc_log::detail::LogEntry> takeEntry() {
    auto const [message, remaining, discarded]
      = remote_fmt::parse(std::span{captured()}, emptyCatalog(), [](std::string_view error) {
            std::printf("parser error: %.*s\n", static_cast<int>(error.size()), error.data());
        });
    bool const clean = remaining.empty() && discarded == 0;
    captured().clear();
    if(!message || !clean) { return std::nullopt; }
    return uc_log::detail::LogEntry{0, *message};
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

std::optional<uc_log::detail::LogEntry> logMetric() {
    using namespace sc::literals;
    UC_LOG_I("temperature {}", uc_log::metric<"temp"_sc, "C"_sc, "board"_sc>(42));
    return takeEntry();
}

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

}   // namespace

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

    if(failures != 0) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
