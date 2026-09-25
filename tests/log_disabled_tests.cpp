// The build without USE_UC_LOG: every UC_LOG call site must still compile, evaluate nothing,
// and count as a use of its arguments, so a value or helper that exists only to be logged
// raises no unused warning (this target builds with the project's warning flags). The
// argument shapes here are the awkward ones: a packed member, a bit-field, a volatile lvalue,
// a braced argument with a comma, a static helper only a log calls. main() exists so ctest has
// something to run, and checks that nothing was evaluated.
#include "uc_log/uc_log.hpp"

#include <cstdint>
#include <cstdio>

#ifdef USE_UC_LOG
    #error "this target must build without USE_UC_LOG"
#endif

namespace {

int evaluations = 0;

int counted() {
    ++evaluations;
    return 42;
}

struct [[gnu::packed]] Packed {
    std::uint8_t  a;
    std::uint32_t b;
    std::uint16_t arr[3];
};

struct Bits {
    unsigned x : 3;
    unsigned y : 5;
};

struct Point {
    int x;
    int y;
};

// used only by a log call: -Wunneeded-internal-declaration territory on clang
int helperOnlyLogged() { return 7; }

}   // namespace

int main() {
    Packed packed{
      1,
      2,
      {3, 4, 5}
    };
    Bits bits{1, 2};
    int volatile vol = 9;
    int const only   = 5;   // only ever logged: -Wunused-variable
    int       set    = 0;   // set, only logged: -Wunused-but-set-variable
    set              = 3;

    UC_LOG_T("plain");
    UC_LOG_D("{}", only);
    UC_LOG_I("{} {}", set, helperOnlyLogged());
    UC_LOG_W("{} {} {}", packed.b, packed.arr, bits.x);
    UC_LOG_E("{} {}", vol, Point{1, 2});
    UC_LOG_C("{}", counted());

    UC_LOG_ENV(UC_LOG_MODULE("disabled"), ::uc_log::setting::MinLevel<::uc_log::LogLevel::warn>);
    UC_LOG_WITH_ENV(::uc_log::setting::MinLevel<::uc_log::LogLevel::error>) {
        UC_LOG_E("{}", counted());
    }

    if(evaluations != 0) {
        std::printf("FAIL: a disabled log call evaluated its argument\n");
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
