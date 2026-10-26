// What RttLog (RttLog.hpp) and a fan-out (FanOutBackend.hpp) must reject, one case per UC_LOG_FAIL_CASE, each a hard
// compile error; case 0 is the control that must build. CMakeLists.txt builds every case as its own excluded target.
// Keep both lists in step.
#include "uc_log/AlwaysAttached.hpp"
#include "uc_log/FanOutBackend.hpp"
#include "uc_log/RttLog.hpp"

#include <cstddef>
#include <cstdint>

struct CoreZero {
    std::uint32_t operator()() const { return 0; }
};

template<std::size_t N>
struct Contexts {
    static constexpr std::size_t cores = N;

    static uc_log::Context context() { return uc_log::Context::thread; }

    static std::size_t core() { return 0; }
};

using TwoCoreRtt = uc_log::
  RttLog<uc_log::AlwaysAttached, uc_log::Rings<64>, uc_log::RingLayout<uc_log::TwoCores<CoreZero>>>;

#if UC_LOG_FAIL_CASE == 0
// the control: options in any order, and a two-core RTT primary behind two-core fan-out contexts
using Backend = uc_log::RttLog<uc_log::AlwaysAttached,
                               uc_log::LossPolicy::Counted<200'000'000>,
                               uc_log::Rings<64, 32>,
                               uc_log::IsrPolicy::MaskedRecord>;
using Fan     = uc_log::FanOutBackendWith<Contexts<2>, TwoCoreRtt>;

#elif UC_LOG_FAIL_CASE == 1
// the same kind twice: two loss policies
using Backend = uc_log::RttLog<uc_log::AlwaysAttached,
                               uc_log::Rings<64>,
                               uc_log::LossPolicy::Silent,
                               uc_log::LossPolicy::Counted<200'000'000>>;
using Fan     = uc_log::FanOutBackendWith<Contexts<2>, TwoCoreRtt>;

#elif UC_LOG_FAIL_CASE == 2
// a type that is no option
using Backend = uc_log::RttLog<uc_log::AlwaysAttached, uc_log::Rings<64>, int>;
using Fan     = uc_log::FanOutBackendWith<Contexts<2>, TwoCoreRtt>;

#elif UC_LOG_FAIL_CASE == 3
// no ring sizes
using Backend = uc_log::RttLog<uc_log::AlwaysAttached, uc_log::IsrPolicy::MaskedRecord>;
using Fan     = uc_log::FanOutBackendWith<Contexts<2>, TwoCoreRtt>;

#elif UC_LOG_FAIL_CASE == 4
// a sink with per-core ISR contexts behind single-core fan-out contexts (Q-5.12-5)
using Backend = TwoCoreRtt;
using Fan     = uc_log::FanOutBackendWith<Contexts<1>, TwoCoreRtt>;

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif

// completing the types runs their checks; nothing is called (rtt wants 4-byte pointers)
static_assert(sizeof(Backend) > 0 && sizeof(Fan) > 0);

int main() {}
