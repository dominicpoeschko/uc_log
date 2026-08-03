// Argument forms the UC_LOG macros must reject; one per UC_LOG_FAIL_CASE, each a hard compile
// error (the static_assert in normalizeLogArgument), so CMakeLists.txt builds every case as its
// own excluded target with WILL_FAIL. The backend stubs match log_macro_tests.cpp so that the
// only thing failing to compile is the argument under test. Keep both lists in step.
#include "uc_log/uc_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uc_log {

template<>
struct ComBackend<Tag::User> {
    static void write(std::span<std::byte const>) {}
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{0}; }
};

}   // namespace uc_log

void failCase();

void failCase() {
#if UC_LOG_FAIL_CASE == 1
    // a pointer has no loggable value
    int value = 42;
    UC_LOG_I("{}", &value);

#elif UC_LOG_FAIL_CASE == 2
    // a char pointer's length is only knowable via strlen, and a missing nul terminator is
    // undefined behaviour; std::string_view is the caller stating the contract
    char const* name = "runtime string";
    UC_LOG_I("{}", name);

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif
}

int main() {}
