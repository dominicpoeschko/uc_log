// What an RTT backend's LossPolicy parameter (LossPolicy.hpp) must reject, one case per UC_LOG_FAIL_CASE, each a
// hard compile error; case 0 is the control that must build. CMakeLists.txt builds every case as its own excluded
// target. Keep both lists in step.
#include "uc_log/DefaultRttComBackend.hpp"

#if UC_LOG_FAIL_CASE == 0
// the control: the counting policy
using Loss = uc_log::LossPolicy::Counted<>;

#elif UC_LOG_FAIL_CASE == 1
// a type that is no loss policy
using Loss = int;

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif

// naming the backend checks its parameters; nothing is instantiated (rtt wants 4-byte pointers)
using Backend = uc_log::DefaultRttComBackend<uc_log::AlwaysAttached,
                                             64,
                                             64,
                                             rtt::BufferMode::skip,
                                             uc_log::DuplexChannels<>,
                                             uc_log::IsrPolicy::SingleLevel<>,
                                             Loss>;

[[maybe_unused]] Backend const* backend = nullptr;

int main() {}
