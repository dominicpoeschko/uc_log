// Reads tests/log_filter/uc_log_filter.txt via --embed-dir. The backend records "<level>" per
// entry, so a line the filter compiled out records nothing.
#include "uc_log/uc_log.hpp"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef UC_LOG_HAS_FILTER_FILE
    #error "the target must pass --embed-dir with tests/log_filter"
#endif

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

// Leaked on purpose: avoids global-constructor / exit-time-destructor warnings.
std::vector<int>& events() {
    static auto& v = *new std::vector<int>{};
    return v;
}

std::vector<int> take() {
    auto e = events();
    events().clear();
    return e;
}

using Levels = std::vector<int>;

}   // namespace

namespace uc_log {
template<>
struct ComBackend<Tag::User> {
    template<LogLevel Level>
    static void initTransfer() {
        events().push_back(static_cast<int>(Level));
    }

    static void write(std::span<std::byte const>) {}
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{1}; }
};
}   // namespace uc_log

#define LOG_ALL()      \
    do {               \
        UC_LOG_T("t"); \
        UC_LOG_D("d"); \
        UC_LOG_I("i"); \
        UC_LOG_W("w"); \
        UC_LOG_E("e"); \
        UC_LOG_C("c"); \
    } while(false)

namespace parser {
using uc_log::detail::LevelOff;
using uc_log::detail::parseFilter;

constexpr auto table = parseFilter(R"(
# comment
* = warn
i2c = info   # trailing comment
i2c.bus = off

usb=crit
)");
static_assert(table.error.empty());
static_assert(table.table.global == 3);
static_assert(table.table.count == 3);
static_assert(table.table.levelFor("i2c") == 2);
static_assert(table.table.levelFor("i2c.device") == 2);
static_assert(table.table.levelFor("i2c.bus") == LevelOff);
static_assert(table.table.levelFor("i2c.bus.queue") == LevelOff);
static_assert(table.table.levelFor("i2cx") == -1);
static_assert(table.table.levelFor("i2") == -1);
static_assert(table.table.levelFor("usb") == 5);
static_assert(table.table.levelFor("") == -1);

static_assert(parseFilter("").error.empty() && parseFilter("").table.global == 0);
static_assert(parseFilter("i2c").line == 1);
static_assert(!parseFilter("i2c = loud").error.empty());
static_assert(!parseFilter("* = info\n* = warn").error.empty());
static_assert(parseFilter("\n\nbad module = info").line == 3);
static_assert(!parseFilter("a = info\na = warn").error.empty());
static_assert(!parseFilter("I2C = warn").error.empty());   // modules are lower case
}   // namespace parser

static void global() { LOG_ALL(); }

namespace quiet {
static void f() { LOG_ALL(); }

struct Inner {
    static void f() { LOG_ALL(); }
};

namespace loud {
    static void f() { LOG_ALL(); }
}   // namespace loud
}   // namespace quiet

namespace chatty {
static void f() { LOG_ALL(); }
}   // namespace chatty

namespace driver {
UC_LOG_SCOPE_MODULE("usb");

static void f() { LOG_ALL(); }
}   // namespace driver

namespace floor_scope {
UC_LOG_SCOPE_MIN_LEVEL(error);

static void f() { LOG_ALL(); }
}   // namespace floor_scope

int main() {
    global();
    CHECK((take() == Levels{1, 2, 3, 4, 5}), "`*` = debug: no trace");

    quiet::f();
    CHECK(take().empty(), "quiet = off");
    quiet::Inner::f();
    CHECK(take().empty(), "off covers quiet.inner");
    quiet::loud::f();
    CHECK((take() == Levels{3, 4, 5}), "the longest rule wins: quiet.loud = warn");

    chatty::f();
    CHECK((take() == Levels{1, 2, 3, 4, 5}), "a rule never goes below `*`");

    driver::f();
    CHECK((take() == Levels{4, 5}), "a rule applies to an explicit module");

    floor_scope::f();
    CHECK((take() == Levels{4, 5}), "without a rule the scope's floor stays");

    // what the printer does with the same file
    auto const runtime = uc_log::detail::parseFilter("* = info\nquiet = off\n");
    CHECK(runtime.error.empty() && runtime.table.levelFor("quiet.x") == uc_log::detail::LevelOff,
          "the parser at run time");

    if(failures == 0) { std::printf("all checks passed\n"); }
    return failures == 0 ? 0 : 1;
}
