#pragma once
// CHECK_EQ(actual, expected, msg): both sides evaluated once, compared with ==, and on a miss both printed through
// fmt ("FAIL: msg: got "<actual>" expected "<expected>" (file:line)"). Any type fmt can format; Check.hpp stays
// printf-only for the targets without fmt.
#include "support/Check.hpp"

#include <fmt/format.h>
#include <string>

namespace uc_log::test {
inline void failEq(char const*        file,
                   int                line,
                   char const*        msg,
                   std::string const& actual,
                   std::string const& expected) {
    std::printf("FAIL: %s: got \"%s\" expected \"%s\" (%s:%d)\n",
                msg,
                actual.c_str(),
                expected.c_str(),
                file,
                line);
    std::fflush(stdout);
    ++failures;
}
}   // namespace uc_log::test

#define CHECK_EQ(actual, expected, msg)                                    \
    do {                                                                   \
        ++::uc_log::test::checks;                                          \
        auto const& checkEqActual_   = (actual);                           \
        auto const& checkEqExpected_ = (expected);                         \
        if(!(checkEqActual_ == checkEqExpected_)) {                        \
            ::uc_log::test::failEq(__FILE__,                               \
                                   __LINE__,                               \
                                   msg,                                    \
                                   ::fmt::format("{}", checkEqActual_),    \
                                   ::fmt::format("{}", checkEqExpected_)); \
        }                                                                  \
    } while(0)
