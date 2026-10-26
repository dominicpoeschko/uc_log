// Argument forms the UC_LOG macros must reject; one per UC_LOG_FAIL_CASE, each a hard compile
// error (the static_assert in normalizeLogArgument), so CMakeLists.txt builds every case as its
// own excluded target (uc_log_add_fail_test: the build must fail with the case's own text). The backend stubs match
// log_macro_tests.cpp so that the only thing failing to compile is the argument under test, and case 0 is the control
// that must build: it proves the stubs and includes are sound. Keep both lists in step. UC_LOG_USE_AGLIO comes from
// CMakeLists.txt per case. Case 4 must build too: with -Wshadow an error, a scope environment and a backend that
// counts its losses (RecordFrame::reportLost instantiated), nothing the macros or the frame declare shadows a name.
#if UC_LOG_FAIL_CASE == 4
    #pragma GCC diagnostic error "-Wshadow"
#endif
#include "uc_log/uc_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#if UC_LOG_FAIL_CASE == 4
namespace test {
struct Counting {};
}   // namespace test
#endif

namespace uc_log {

template<>
struct ComBackend<Tag::User> {
    static void write(std::span<std::byte const>) {}
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{0}; }
};

#if UC_LOG_FAIL_CASE == 4
template<>
struct ComBackend<::test::Counting> {
    struct Mark {
        std::uint32_t pending;
    };

    static void write(std::span<std::byte const>) {}

    static void noteRecord() {}

    static Mark lossMark() { return Mark{1}; }

    static void lossSettle(Mark) {}
};

template<>
struct LogClock<::test::Counting> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{0}; }
};
#endif

}   // namespace uc_log

#if UC_LOG_FAIL_CASE == 4
namespace counted_scope {
UC_LOG_ENV(::uc_log::setting::Backend<::test::Counting>,
           ::uc_log::setting::MinLevel<::uc_log::LogLevel::info>);

inline void log(int value) {
    UC_LOG_W("{}", value);
    UC_LOG_AT(::uc_log::LogLevel::error, 12, "caller.cpp", "{} {}", value, value);
    {
        UC_LOG_SCOPE_MIN_LEVEL(warn);
        UC_LOG_E("inner {}", value);
    }
}
}   // namespace counted_scope
#endif

#if UC_LOG_FAIL_CASE == 3
// at namespace scope: glaze reflects only a type with linkage
struct Reflected {
    int a;
    int b;
};
#endif

void failCase();

void failCase() {
#if UC_LOG_FAIL_CASE == 0
    // the control: a loggable argument
    int value = 42;
    UC_LOG_I("{}", value);

#elif UC_LOG_FAIL_CASE == 1
    // a pointer has no loggable value
    int value = 42;
    UC_LOG_I("{}", &value);

#elif UC_LOG_FAIL_CASE == 2
    // a char pointer's length is only knowable via strlen, and a missing nul terminator is
    // undefined behaviour; std::string_view is the caller stating the contract
    char const* name = "runtime string";
    UC_LOG_I("{}", name);

#elif UC_LOG_FAIL_CASE == 3
    // a reflectable struct where the image has no aglio formatter (UC_LOG_USE_AGLIO=0, LOG_REFLECTED_STRUCTS OFF):
    // "implicit instantiation of undefined template remote_fmt::formatter<Reflected>". With UC_LOG_USE_AGLIO=1 the
    // same line is the control log_macro_accepts_reflected (CMakeLists.txt).
    Reflected const value{1, 2};
    UC_LOG_I("{}", value);

#elif UC_LOG_FAIL_CASE == 4
    counted_scope::log(42);

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif
}

int main() {}
