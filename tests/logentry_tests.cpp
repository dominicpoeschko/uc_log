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
        LogEntry const e{2, R"(("main.cpp", 42, 2, 123ms, """""")hello world)"};
        CHECK(e.parsedOk, "well-formed entry parses");
        CHECK(e.channel.channel == 2, "channel");
        CHECK(e.fileName == "main.cpp", "fileName");
        CHECK(e.line == 42, "line");
        CHECK(e.logLevel == uc_log::LogLevel::info, "level");
        CHECK(e.ucTime.time == std::chrono::milliseconds{123}, "ucTime");
        CHECK(e.functionName == "?", "no call site: no function");
        CHECK(e.logMsg == "hello world", "message");
    }

    {
        uc_log::detail::SignatureInfo const site{"i2c.bus",
                                                 "Bus<int>::run",
                                                 "Kvasir::I2C::Bus<int>::run()"};
        LogEntry const                      e{0, R"(("bus.cpp", 5, 2, 1ms, """""")up)", &site};
        CHECK(e.parsedOk && e.functionName == "Bus<int>::run", "function from the site");
        CHECK(e.module == "i2c.bus" && e.signature == "Kvasir::I2C::Bus<int>::run()",
              "module and signature from the site");
        LogEntry const own{0, R"(("bus.cpp", 5, 2, 1ms, "usb", """""")up)", &site};
        CHECK(own.module == "usb", "an explicit module wins over the site's");
    }

    // a module scope's header: `"module", ` between the time and the function name
    {
        LogEntry const e{1, R"(("usb.cpp", 7, 3, 9ms, "usb", """""")stall {})"};
        CHECK(e.parsedOk, "header with a module parses");
        CHECK(e.module == "usb", "module");
        CHECK(e.logLevel == uc_log::LogLevel::warn && e.line == 7, "fields before the module");
        CHECK(e.logMsg == "stall {}", "message after a module");

        LogEntry const none{0, R"(("main.cpp", 42, 2, 123ms, """""")hello)"};
        CHECK(none.parsedOk && none.module.empty(), "no module field: empty module");

        LogEntry const path{0, R"(("a.cpp", 1, 2, 1ms, "kvasir::i2c/bus-0.x", """""")m)"};
        CHECK(path.parsedOk && path.module == "kvasir::i2c/bus-0.x", "module with : / - . chars");

        LogEntry const empty{0, R"(("a.cpp", 1, 2, 1ms, "", """""")m)"};
        CHECK(!empty.parsedOk, "empty module flagged");

        LogEntry const unterminated{0, R"(("a.cpp", 1, 2, 1ms, "usb, """""")m)"};
        CHECK(!unterminated.parsedOk, "unterminated module flagged");

        LogEntry const noComma{0, R"(("a.cpp", 1, 2, 1ms, "usb" """""")m)"};
        CHECK(!noComma.parsedOk, "module without its separator flagged");
    }

    // parse failures keep the raw message and set parsedOk = false
    {
        LogEntry const plain{0, "no header at all"};
        CHECK(!plain.parsedOk, "headerless message flagged");
        CHECK(plain.logMsg == "no header at all", "raw message preserved");

        LogEntry const badLine{0, R"(("f.cpp", xx, 2, 1ms, """""")msg)"};
        CHECK(!badLine.parsedOk, "non-numeric line flagged");

        LogEntry const badTime{0, R"(("f.cpp", 1, 2, zz, """""")msg)"};
        CHECK(!badTime.parsedOk, "bad time flagged");

        LogEntry const truncated{0, R"(("f.cpp", 1, 2)"};
        CHECK(!truncated.parsedOk, "truncated header flagged");
    }

    // line numbers above 65535 no longer discard the whole header
    {
        LogEntry const e{0, R"(("gen.cpp", 100000, 3, 5us, """""")big file)"};
        CHECK(e.parsedOk, "line > 65535 parses");
        CHECK(e.line == 100000, "large line value kept");
    }

    // [num/den]s time form
    {
        LogEntry const e{0, R"(("f.cpp", 1, 1, 250[1/1000]s, """""")m)"};
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

        e.logMsg = "@METRIC(env::huge=1e99999)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.empty(), "out-of-range value skipped without throwing");

        e.logMsg = "@METRIC(env::x=1) @METRIC(env::y=2)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.size() == 2, "two metrics in one message");

        e.logMsg = "@METRIC(env::a[C) @METRIC(env::b[V]=2)";
        metrics  = uc_log::extractMetrics(now, e);
        CHECK(metrics.size() == 1 && metrics[0].first.name == "b" && metrics[0].first.unit == "V",
              "an unclosed unit skips only its own marker");

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
