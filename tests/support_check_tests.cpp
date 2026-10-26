// The test support itself (support/Check.hpp, support/CheckFmt.hpp): what passes passes, what fails fails with
// file:line in its text, and the case runner's selection. The failing cases are ctest entries with WILL_FAIL or a
// PASS_REGULAR_EXPRESSION (tests/CMakeLists.txt).
#include "support/CheckFmt.hpp"

#include <stdexcept>
#include <string>

namespace {
void passes() {
    int const before = uc_log::test::checks.load();
    CHECK(1 + 1 == 2, "a true condition passes");
    CHECKF(2 * 2 == 4, "a true condition passes with a printf message (%d)", 4);
    CHECK_EQ(std::string{"abc"}, std::string{"abc"}, "equal strings pass");
    CHECK_EQ(42, 42, "equal numbers pass");
    int const after = uc_log::test::checks.load();
    CHECK(after == before + 4, "every check is counted");
    CHECK(uc_log::test::failures.load() == 0, "nothing failed");
}

void failsOnPurpose() { CHECK(false, "on purpose"); }

void checkEqPrintsBothSides() { CHECK_EQ(41, 42, "eq on purpose"); }

[[noreturn]] void throwsOnPurpose() { throw std::runtime_error{"thrown on purpose"}; }
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {                "passes",                 passes},
                               {        "failsOnPurpose",         failsOnPurpose},
                               {"checkEqPrintsBothSides", checkEqPrintsBothSides},
                               {       "throwsOnPurpose",        throwsOnPurpose},
    });
}
