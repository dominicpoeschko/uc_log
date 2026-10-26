#pragma once

// Where a log write runs on a Cortex-M: the one IPSR read uc_log's backends share (the RTT ring choice, FanOut's
// per-context flags), and the record guard of IsrPolicy::MaskedRecord. uc_log keeps its own asm here instead of using
// a core library: it is used below Kvasir and builds alone.
//
// IPSR (ARMv6-M ARM DDI 0419E B1.4.2 "The IPSR"): 0 in Thread mode, else the exception number of the running
// exception; NMI is 2, HardFault 3 (B1.5.2 "Exception number definition", Table "Exception numbers"). On ARMv8-M
// (DDI 0553B.y B3.5) IPSR also reads 1 while a Secure-to-Non-secure function call is in progress: `ipsr() == 0` means
// "Thread mode, no Non-secure call in progress". MRS IPSR reads the IPSR field alone, so no mask is needed.
// PRIMASK: DDI 0419E B1.4.3 "The special-purpose mask register, PRIMASK".
//
// Contexts (the type a backend asks "where am I" - RttComBackend's RingLayout and FanOutBackendWith alike): static
// `Cores`, `inThread()`, `context()`, `core()`. SingleCore and TwoCores<CoreId> below; host tests pass a fake with the
// same members.

#include <cstddef>
#include <cstdint>

namespace uc_log {

enum class Context : std::uint8_t { thread = 0, isr = 1, fault = 2 };

#ifdef __arm__
[[gnu::always_inline]] inline std::uint32_t ipsr() {
    std::uint32_t value{};
    asm volatile("mrs %0, ipsr" : "=r"(value));
    return value;
}
#else
// The host has no IPSR: declared, never defined, for IsrRecordGuard below (which a host build names but never calls).
std::uint32_t ipsr();
#endif

// thread, interrupts, NMI/HardFault
inline constexpr std::size_t contextsPerCore = 3;

// A record written from an exception is written with PRIMASK set, for the whole record: nested ISRs share one ring,
// and a preempting handler's record would otherwise land inside the preempted one's. uc_log::detail::Log holds this
// around a record when the backend names it as RecordGuard (IsrPolicy::MaskedRecord). NMI and HardFault are not
// masked by PRIMASK. The cost is interrupt latency: every other interrupt waits for one ISR log record.
// Not under __arm__: host tests name it (never instantiated there).
// Branch-free on purpose: a flag member made clang emit every call site twice.
struct IsrRecordGuard {
    IsrRecordGuard() {
        std::uint32_t const exception = ipsr();
        asm volatile("mrs %0, primask" : "=r"(saved_)::"memory");
        std::uint32_t const mask = saved_ | static_cast<std::uint32_t>(exception != 0);
        asm volatile("msr primask, %0" : : "r"(mask) : "memory");
    }

    // PRIMASK back as found, then an isb in the SAME asm: lowering the execution priority by MSR is only guaranteed to
    // be visible after a context synchronization event on Armv6-M and Armv7-M (DDI0419E md l.10143-10149, DDI0403E.e md
    // l.24292-24298), so an interrupt that pended during the record is taken here; on Armv8-M an MSR's change is
    // visible to the instructions after it already (DDI0553B.y B3.3 RXHHC md l.3580-3584; RQZWN, md l.7132-7136, asks
    // for the barrier after CPSIE only) and the isb is harmless. As two asm statements the compiler may schedule other
    // code between them. The raising msr above needs no
    // barrier (same paragraphs). Its own asm, the form of Kvasir::Core::IrqState::restore(): uc_log builds without the
    // SDK and the core packages.
    ~IsrRecordGuard() { asm volatile("msr primask, %0\n isb" : : "r"(saved_) : "memory"); }

    IsrRecordGuard(IsrRecordGuard const&)            = delete;
    IsrRecordGuard& operator=(IsrRecordGuard const&) = delete;

private:
    std::uint32_t saved_;
};

// One core.
struct SingleCore {
    static constexpr std::size_t cores = 1;

#ifdef __arm__
    [[gnu::always_inline]] static bool inThread() { return ipsr() == 0; }

    static Context context() {
        std::uint32_t const exception = ipsr();
        if(exception == 0) { return Context::thread; }
        if(exception == 2 || exception == 3) { return Context::fault; }
        return Context::isr;
    }

    static std::size_t core() { return 0; }
#else
    // The host has no IPSR: a host build that writes through a backend passes a fake Contexts. Declared, never
    // defined: naming them (a test that checks a backend's members instantiates the ring choice) builds, calling them
    // fails the link.
    static bool        inThread();
    static Context     context();
    static std::size_t core();
#endif
};

// Two cores told apart by CoreId, a functor returning 0 or 1 (Kvasir::Sio::CpuId on the RP chips): each core has its
// own rings (RttComBackend) and its own per-context flags (FanOutBackend), written by that core only.
template<typename CoreId>
struct TwoCores : SingleCore {
    static constexpr std::size_t cores = 2;

    static std::size_t core() { return CoreId{}() != 0 ? 1 : 0; }
};

}   // namespace uc_log
