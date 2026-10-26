// What uc_log::bootsThatLogged() (Runtime.hpp) must reject: a tag whose backend counts no boots, one case per
// UC_LOG_FAIL_CASE, each a hard compile error (the static_assert's text); case 0 is the control that must build - a
// counting backend, and panicMode() / drain() on a backend with the hooks and on a tag without any backend (nothing for
// those). CMakeLists.txt builds every case as its own excluded target. Keep both lists in step.
#include "uc_log/Runtime.hpp"

#include <cstdint>

namespace counting {
struct Tag {};
}   // namespace counting

namespace quiet {
struct Tag {};
}   // namespace quiet

// a tag no backend is declared for (a host test)
struct NoBackend {};

// a backend that counts boots (what the RTT backend's bootsThatLogged() is)
template<>
struct uc_log::ComBackend<counting::Tag> {
    static std::uint16_t bootsThatLogged() { return 3; }
};

// a complete backend with the panic and drain hooks, but no count (a USB-only sink)
template<>
struct uc_log::ComBackend<quiet::Tag> {
    static void enterPanicMode() {}

    static void drain() {}
};

#if UC_LOG_FAIL_CASE == 0
int main() {
    uc_log::panicMode<quiet::Tag>();
    uc_log::drain<quiet::Tag>();
    uc_log::panicMode<NoBackend>();
    uc_log::drain<NoBackend>();
    return uc_log::bootsThatLogged<counting::Tag>() == 3 ? 0 : 1;
}

#elif UC_LOG_FAIL_CASE == 1
// a backend without the count
int main() { return uc_log::bootsThatLogged<quiet::Tag>(); }

#elif UC_LOG_FAIL_CASE == 2
// no backend at all
int main() { return uc_log::bootsThatLogged<NoBackend>(); }

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif
