#pragma once
// The host tests' check macros and case runner. printf only: the -m32 targets (loss_ledger_tests) include it too, and
// they build with -Wall -Wextra -Wconversion -Wsign-conversion -Werror and without fmt.
//
//   CHECK(cond, msg)            counts a check; on a miss prints "FAIL: msg (file:line)" and counts a failure
//   CHECKF(cond, format, ...)   the same with a printf message
//   uc_log::test::run(argc, argv, {{"name", fn}, ...})   runs the cases (all, or those named by --case), prints one
//                               summary line and returns the exit code; a span of cases works the same (a main that
//                               joins several files' tables: rtt_reader/rtt_reader_tests.cpp)
//   uc_log::test::report()      the summary line and exit code alone, for a test whose main is one script
//   uc_log::test::need(opt, what)  the value of an optional the test needs to go on; an empty one throws, which ends
//                               the case as failed (run() catches it)
//
// The counters are atomic: a CHECK may run on any thread. A FAIL is flushed at once, so a test that hangs after it
// (and is killed by ctest's TIMEOUT) still shows it.
#include "uc_log/detail/Lifetimebound.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::test {
inline std::atomic<int> failures{0};
inline std::atomic<int> checks{0};

inline void fail(char const* file,
                 int         line,
                 char const* msg) {
    std::printf("FAIL: %s (%s:%d)\n", msg, file, line);
    std::fflush(stdout);
    ++failures;
}

template<typename T>
T& need(std::optional<T>& o UC_LOG_LIFETIMEBOUND,
        char const*         what) {
    if(!o) { throw std::runtime_error(what); }
    return *o;
}

template<typename T>
T const& need(std::optional<T> const& o UC_LOG_LIFETIMEBOUND,
              char const*               what) {
    if(!o) { throw std::runtime_error(what); }
    return *o;
}

template<typename T>
T need(std::optional<T>&& o,
       char const*        what) {
    if(!o) { throw std::runtime_error(what); }
    return std::move(*o);
}

struct Case {
    std::string_view name;
    void (*fn)();
};

/// One summary line; exit code 0 when no check failed, 1 otherwise.
inline int report() {
    int const failed = failures.load();
    int const total  = checks.load();
    if(failed == 0) {
        std::printf("ALL OK (%d checks)\n", total);
    } else {
        std::printf("%d FAILURES (%d checks)\n", failed, total);
    }
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

/// The case runner. Arguments:
///   (none)                every case, in order
///   --case <name>         only this case (repeatable, run in the order given); an unknown name is exit code 2
///   --list                the case names, one per line
///   --expect-cases a b c  exit code 0 only if the cases are exactly a b c in this order (ctest's guard that every
///                         case of the source is also registered in CMake's CASES list)
/// Call it after anything that must outlive every case (a test's VirtualTime is the first local of main).
inline int run(int                   argc,
               char**                argv,
               std::span<Case const> cases) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto const known = [&](std::string_view name) {
        for(Case const& c : cases) {
            if(c.name == name) { return true; }
        }
        return false;
    };
    auto const        argAt = [&](int i) { return std::string_view{*std::next(argv, i)}; };
    std::vector<Case> selected;
    for(int i = 1; i < argc; ++i) {
        std::string_view const arg = argAt(i);
        if(arg == "--list") {
            for(Case const& c : cases) {
                std::printf("%.*s\n", static_cast<int>(c.name.size()), c.name.data());
            }
            return 0;
        }
        if(arg == "--expect-cases") {
            std::vector<std::string_view> expected;
            for(int e = i + 1; e < argc; ++e) { expected.push_back(argAt(e)); }
            bool        same  = expected.size() == cases.size();
            std::size_t index = 0;
            for(Case const& c : cases) {
                if(!same) { break; }
                same = expected[index] == c.name;
                ++index;
            }
            if(!same) {
                std::printf(
                  "FAIL: the cases in the source and in CMake's CASES list differ\n  source:");
                for(Case const& c : cases) {
                    std::printf(" %.*s", static_cast<int>(c.name.size()), c.name.data());
                }
                std::printf("\n  CMake: ");
                for(std::string_view const e : expected) {
                    std::printf(" %.*s", static_cast<int>(e.size()), e.data());
                }
                std::printf("\n");
                return 1;
            }
            std::printf("ALL OK (%zu cases registered)\n", cases.size());
            return 0;
        }
        if(arg == "--case" && i + 1 < argc) {
            std::string_view const name = argAt(++i);
            if(!known(name)) {
                std::printf("unknown case '%.*s'; --list shows the cases\n",
                            static_cast<int>(name.size()),
                            name.data());
                return 2;
            }
            for(Case const& c : cases) {
                if(c.name == name) { selected.push_back(c); }
            }
            continue;
        }
        std::printf(
          "unknown argument '%.*s'; arguments: [--case <name>]... | --list | --expect-cases "
          "<name>...\n",
          static_cast<int>(arg.size()),
          arg.data());
        return 2;
    }
    if(selected.empty()) { selected.assign(cases.begin(), cases.end()); }
    for(Case const& c : selected) {
        std::printf("-- %.*s\n", static_cast<int>(c.name.size()), c.name.data());
        try {
            c.fn();
        } catch(std::exception const& e) {
            std::printf("FAIL: case %.*s threw: %s\n",
                        static_cast<int>(c.name.size()),
                        c.name.data(),
                        e.what());
            ++failures;
        } catch(...) {
            std::printf("FAIL: case %.*s threw\n", static_cast<int>(c.name.size()), c.name.data());
            ++failures;
        }
    }
    return report();
}

/// The same for a table written at the call: run(argc, argv, {{"name", fn}, ...}).
inline int run(int                         argc,
               char**                      argv,
               std::initializer_list<Case> cases) {
    return run(argc, argv, std::span<Case const>{cases});
}
}   // namespace uc_log::test

#define CHECK(cond, msg)                                               \
    do {                                                               \
        ++::uc_log::test::checks;                                      \
        if(!(cond)) { ::uc_log::test::fail(__FILE__, __LINE__, msg); } \
    } while(0)

// printf-style message
#define CHECKF(cond, format, ...)                              \
    do {                                                       \
        ++::uc_log::test::checks;                              \
        if(!(cond)) {                                          \
            std::printf("FAIL (%s:%d): ", __FILE__, __LINE__); \
            std::printf(format __VA_OPT__(, ) __VA_ARGS__);    \
            std::printf("\n");                                 \
            std::fflush(stdout);                               \
            ++::uc_log::test::failures;                        \
        }                                                      \
    } while(0)
