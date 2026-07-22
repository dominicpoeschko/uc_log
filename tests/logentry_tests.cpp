// Unit tests for LogEntry parsing (parsedOk, uint32 line, integer UcTime) and
// extractMetrics (from_chars, no exceptions).
#include "uc_log/detail/LogEntry.hpp"

#include "uc_log/metric_utils.hpp"

#include <cstdio>
#include <limits>
#include <string>

using uc_log::detail::LogEntry;

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

int main() {
    // producer format round trip
    {
        LogEntry const e{2, R"(("main.cpp", 42, 2, 123ms, """foo""")hello world)"};
        CHECK(e.parsedOk, "well-formed entry parses");
        CHECK(e.channel.channel == 2, "channel");
        CHECK(e.fileName == "main.cpp", "fileName");
        CHECK(e.line == 42, "line");
        CHECK(e.logLevel == uc_log::LogLevel::info, "level");
        CHECK(e.ucTime.time == std::chrono::milliseconds{123}, "ucTime");
        CHECK(e.functionName == "foo", "function");
        CHECK(e.logMsg == "hello world", "message");
    }

    // the reader's RTT overflow marker must parse as a proper error-level entry
    {
        LogEntry const e{
          0,
          R"(("uc_log", 0, 4, 0ns, """rtt""")⚠ RTT host overflow, log data lost (1 event))"};
        CHECK(e.parsedOk, "overflow marker parses");
        CHECK(e.logLevel == uc_log::LogLevel::error, "overflow marker level is error");
        CHECK(e.fileName == "uc_log", "overflow marker file");
    }

    // parse failures keep the raw message and set parsedOk = false
    {
        LogEntry const plain{0, "no header at all"};
        CHECK(!plain.parsedOk, "headerless message flagged");
        CHECK(plain.logMsg == "no header at all", "raw message preserved");

        LogEntry const badLine{0, R"(("f.cpp", xx, 2, 1ms, """f""")msg)"};
        CHECK(!badLine.parsedOk, "non-numeric line flagged");

        LogEntry const badTime{0, R"(("f.cpp", 1, 2, zz, """f""")msg)"};
        CHECK(!badTime.parsedOk, "bad time flagged");

        LogEntry const truncated{0, R"(("f.cpp", 1, 2)"};
        CHECK(!truncated.parsedOk, "truncated header flagged");
    }

    // line numbers above 65535 no longer discard the whole header
    {
        LogEntry const e{0, R"(("gen.cpp", 100000, 3, 5us, """g""")big file)"};
        CHECK(e.parsedOk, "line > 65535 parses");
        CHECK(e.line == 100000, "large line value kept");
    }

    // [num/den]s time form
    {
        LogEntry const e{0, R"(("f.cpp", 1, 1, 250[1/1000]s, """f""")m)"};
        CHECK(e.parsedOk, "ratio time parses");
        CHECK(e.ucTime.time == std::chrono::milliseconds{250}, "ratio time value");
    }

    // UcTime integer math: exact beyond double's 53-bit mantissa
    {
        constexpr std::uint64_t BigNs = (1ULL << 53) + 1;
        LogEntry::UcTime const  t{BigNs, 1, 1'000'000'000};
        CHECK(t.time.count() == static_cast<std::int64_t>(BigNs), "ns value exact beyond 2^53");
    }

    // UcTime saturation instead of overflow
    {
        LogEntry::UcTime const t{std::numeric_limits<std::uint64_t>::max(), 1'000'000, 1};
        CHECK(t.time.count() == std::numeric_limits<std::chrono::nanoseconds::rep>::max(),
              "saturates at max representable");
        LogEntry::UcTime const zeroDen{1, 1, 0};
        CHECK(zeroDen.time.count() == 0, "zero denominator yields zero, not UB");
    }

    // metrics: from_chars path, no exceptions
    {
        auto const now = std::chrono::system_clock::now();
        LogEntry   e{0, ""};
        e.parsedOk = true;
        e.fileName = "m.cpp";
        e.line     = 1;

        e.logMsg     = "temp @METRIC(env::temp[C]=23.5) done";
        auto metrics = uc_log::extractMetrics(now, e);
        CHECK(metrics.size() == 1, "one metric extracted");
        CHECK(metrics.size() == 1 && metrics[0].second.value == 23.5, "metric value");
        CHECK(metrics.size() == 1 && metrics[0].first.unit == "C", "metric unit");

        e.logMsg = "@METRIC(env::bad=notanumber)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.empty(), "non-numeric value skipped");

        // out_of_range used to escape std::stod's invalid_argument-only catch and
        // terminate the printer
        e.logMsg = "@METRIC(env::huge=1e99999)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.empty(), "out-of-range value skipped without throwing");

        e.logMsg = "@METRIC(env::x=1) @METRIC(env::y=2)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.size() == 2, "two metrics in one message");

        e.logMsg = "@METRIC(noscope=1)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.empty(), "missing :: skipped");
    }

    if(failures == 0) {
        std::printf("ALL OK\n");
        return 0;
    }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
