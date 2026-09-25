#pragma once

#include <cstdint>

namespace uc_log::detail {

// A record written from an exception is written with PRIMASK set, for the whole record:
// nested ISRs share one ring, and a preempting handler's record would otherwise land inside
// the preempted one's. uc_log::detail::Log holds this around a record when the backend names
// it as RecordGuard (IsrPolicy::MaskedRecord). NMI and HardFault are not masked by PRIMASK.
// The cost is interrupt latency: every other interrupt waits for one ISR log record.
// Branch-free on purpose: a flag member made clang emit every call site twice.
// MRS/MSR: RP2040 data sheet 2.4.3.3.
struct IsrRecordGuard {
    IsrRecordGuard() {
        std::uint32_t ipsr{};
        asm volatile("mrs %0, ipsr" : "=r"(ipsr));
        asm volatile("mrs %0, primask" : "=r"(saved_)::"memory");
        std::uint32_t const mask = saved_ | static_cast<std::uint32_t>(ipsr != 0);
        asm volatile("msr primask, %0" : : "r"(mask) : "memory");
    }

    ~IsrRecordGuard() { asm volatile("msr primask, %0" : : "r"(saved_) : "memory"); }

    IsrRecordGuard(IsrRecordGuard const&)            = delete;
    IsrRecordGuard& operator=(IsrRecordGuard const&) = delete;

private:
    std::uint32_t saved_;
};

}   // namespace uc_log::detail
