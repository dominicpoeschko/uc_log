// Two things this TU pins down, both through the real UC_LOG macros:
//  * UC_LOG_MIN_LEVEL (=warn, set by the target): call sites below the floor must write
//    nothing at all, those at or above it must write as usual.
//  * LevelBoundBackend's optional initTransfer/finalizeTransfer forwarding: the level-templated
//    form is preferred, the plain form is the fallback, and a backend with neither still
//    builds. The Printer brackets each entry with them, so the observable order is
//    init, write..., finalize.
//  * The backend's RecordGuard is held by Log::log around the whole record: constructed
//    before initTransfer, destroyed after finalizeTransfer.
#include "uc_log/detail/LevelBoundBackend.hpp"
#include "uc_log/uc_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

static_assert(uc_log::minLevel == uc_log::LogLevel::warn,
              "target must set UC_LOG_MIN_LEVEL=warn");

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
std::vector<std::string>& events() {
    static auto& v = *new std::vector<std::string>{};
    return v;
}

}   // namespace

namespace uc_log {

// The macro backend: level-templated hooks, so the recorded level tells which call site fired,
// and a guard that records its own lifetime.
template<>
struct ComBackend<Tag::User> {
    struct RecordGuard {
        RecordGuard() { events().push_back("guard-begin"); }

        ~RecordGuard() { events().push_back("guard-end"); }

        RecordGuard(RecordGuard const&)            = delete;
        RecordGuard& operator=(RecordGuard const&) = delete;
    };

    template<LogLevel Level>
    static void initTransfer() {
        events().push_back("init" + std::to_string(static_cast<int>(Level)));
    }

    static void write(std::span<std::byte const> data) {
        events().push_back("write" + std::to_string(data.size()));
    }

    template<LogLevel Level>
    static void finalizeTransfer() {
        events().push_back("fini" + std::to_string(static_cast<int>(Level)));
    }
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{1}; }
};

}   // namespace uc_log

namespace {

// Direct LevelBoundBackend probes for the fallback shapes the macro backend above does not cover.
struct PlainHooks {
    static void write(std::span<std::byte const>) { events().push_back("plain-write"); }

    static void initTransfer() { events().push_back("plain-init"); }

    static void finalizeTransfer() { events().push_back("plain-fini"); }
};

struct NoHooks {
    static void write(std::span<std::byte const>) { events().push_back("nohooks-write"); }
};

bool wroteSomething() {
    for(auto const& e : events()) {
        if(e.starts_with("write")) { return true; }
    }
    return false;
}

}   // namespace

int main() {
    using uc_log::LogLevel;

    // Below the floor: nothing at all, not even init/finalize.
    events().clear();
    UC_LOG_T("trace {}", 1);
    UC_LOG_D("debug {}", 2);
    UC_LOG_I("info {}", 3);
    CHECK(events().empty(), "levels below UC_LOG_MIN_LEVEL emit nothing");

    // At the floor: one bracketed entry with the level-templated hooks, inside the guard.
    events().clear();
    UC_LOG_W("warn {}", 4);
    CHECK(wroteSomething(), "warn writes");
    CHECK(events().size() >= 4 && events()[0] == "guard-begin" && events()[1] == "init3",
          "warn: the guard is taken before initTransfer<warn>");
    CHECK(events().size() >= 4 && events()[events().size() - 2] == "fini3"
            && events().back() == "guard-end",
          "warn: the guard is released after finalizeTransfer<warn>");

    events().clear();
    UC_LOG_E("error");
    CHECK(wroteSomething(), "error writes");
    CHECK(events().size() >= 2 && events()[1] == "init4", "error: initTransfer<error> first");

    events().clear();
    UC_LOG_C("crit");
    CHECK(wroteSomething(), "crit writes");
    CHECK(events().size() >= 2 && events()[events().size() - 2] == "fini5",
          "crit: finalizeTransfer<crit> last");

    // Plain (non-templated) hooks are forwarded.
    events().clear();
    using Plain = uc_log::detail::LevelBoundBackend<PlainHooks, LogLevel::info>;
    Plain::initTransfer();
    Plain::write({});
    Plain::finalizeTransfer();
    CHECK((events() == std::vector<std::string>{"plain-init", "plain-write", "plain-fini"}),
          "plain initTransfer/finalizeTransfer forwarded");

    // No hooks at all: the forwarders exist and are no-ops.
    events().clear();
    using None = uc_log::detail::LevelBoundBackend<NoHooks, LogLevel::info>;
    None::initTransfer();
    None::write({});
    None::finalizeTransfer();
    CHECK(events() == std::vector<std::string>{"nohooks-write"}, "missing hooks are no-ops");

    if(failures != 0) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
