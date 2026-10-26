// What an RTT backend's LossPolicy parameter (LossPolicy.hpp) must reject, one case per UC_LOG_FAIL_CASE, each a
// hard compile error; case 0 is the control that must build. CMakeLists.txt builds every case as its own excluded
// target. Keep both lists in step.
#include "uc_log/RttLog.hpp"

#if UC_LOG_FAIL_CASE == 0
// the control: the counting policy at a 200 MHz CPU
using Loss = uc_log::LossPolicy::Counted<200'000'000>;

#elif UC_LOG_FAIL_CASE == 1
// a type that is no loss policy
using Loss = int;

#elif UC_LOG_FAIL_CASE == 2
// a CPU clock ceiling of 0 (what Kvasir's Register::CpuHzCeiling primary holds when no chip specialised it):
//   static assertion failed: LossPolicy::Counted<CpuHzCeiling>: the fastest CPU clock of the image in Hz, not 0
using Loss = uc_log::LossPolicy::Counted<0>;

#elif UC_LOG_FAIL_CASE == 3
// no CPU clock ceiling at all: there is no default (no silent 200 MHz)
using Loss = uc_log::LossPolicy::Counted<>;

#else
    #error "unknown UC_LOG_FAIL_CASE"
#endif

// naming the backend checks its parameters; nothing is instantiated (rtt wants 4-byte pointers)
using Backend = uc_log::RttComBackend<uc_log::AlwaysAttached,
                                      uc_log::RingLayout<>,
                                      uc_log::ChannelSizes<64, 64>,
                                      rtt::BufferMode::skip,
                                      uc_log::DuplexChannels<>,
                                      uc_log::IsrPolicy::SingleLevel<>,
                                      Loss>;

[[maybe_unused]] static Backend const* backend = nullptr;

int main() {}
