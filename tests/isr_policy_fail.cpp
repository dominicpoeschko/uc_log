// Policy configurations IsrPolicy.hpp must reject, one per UC_LOG_FAIL_CASE, each a hard
// compile error; CMakeLists.txt builds every case as its own excluded target with WILL_FAIL.
// Case 0 is the control that must build, so a case cannot pass by failing for an unrelated
// reason. Keep both lists in step.
#include "uc_log/IsrPolicy.hpp"

namespace {

struct FakeActive {
    static int operator()() { return 0; }
};

}   // namespace

#if UC_LOG_FAIL_CASE == 0
// the control: a valid policy
using Policy
  = uc_log::IsrPolicy::PerLevel<FakeActive, uc_log::Levels<1, 3>, uc_log::SilentLevels<0>>;

#elif UC_LOG_FAIL_CASE == 1
// a level listed twice
using Policy = uc_log::IsrPolicy::PerLevel<FakeActive, uc_log::Levels<3, 3>>;

#elif UC_LOG_FAIL_CASE == 2
// a level both served and silent
using Policy = uc_log::IsrPolicy::PerLevel<FakeActive, uc_log::Levels<3>, uc_log::SilentLevels<3>>;

#elif UC_LOG_FAIL_CASE == 3
// a level a Cortex-M cannot have
using Policy = uc_log::IsrPolicy::SingleLevel<uc_log::SilentLevels<16>>;

#elif UC_LOG_FAIL_CASE == 4
// no served level at all
using Policy = uc_log::IsrPolicy::PerLevel<FakeActive, uc_log::Levels<>>;

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif

[[maybe_unused]] constexpr std::size_t rings = Policy::NumIsrRings;

int main() {}
