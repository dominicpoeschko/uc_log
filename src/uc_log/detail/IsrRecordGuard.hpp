#pragma once

#include <cstdint>

namespace uc_log::detail {

// A record written from an exception is written with PRIMASK set, for the whole record:
// nested ISRs share one ring, and a preempting handler's record would otherwise land inside
// the preempted one's. uc_log::detail::Log holds this around a record when the backend names
// it as RecordGuard (IsrPolicy::MaskedRecord). NMI and HardFault are not masked by PRIMASK.
// The cost is interrupt latency: every other interrupt waits for one ISR log record.
struct IsrRecordGuard {
    IsrRecordGuard() {
        std::uint32_t ipsr{};
        asm volatile("mrs %0, ipsr" : "=r"(ipsr));
        if(ipsr == 0) { return; }
        std::uint32_t primask{};
        asm volatile("mrs %0, primask" : "=r"(primask)::"memory");
        if(primask != 0) { return; }   // already masked: the caller unmasks
        asm("cpsid i" : : : "memory");
        masked_ = true;
    }

    ~IsrRecordGuard() {
        if(masked_) { asm("cpsie i" : : : "memory"); }
    }

    IsrRecordGuard(IsrRecordGuard const&)            = delete;
    IsrRecordGuard& operator=(IsrRecordGuard const&) = delete;

private:
    bool masked_{false};
};

}   // namespace uc_log::detail
