#pragma once

// What the printer reads and writes over the probe on every Cortex-M it serves: the System Control Space registers
// (the same addresses on Armv6-M, Armv7-M and Armv8-M) and the core registers of a halted core as the J-Link DLL
// numbers them. Each fact once, with where it is written down: Armv7-M ARM DDI 0403E.e (md line of the converted
// datasheet in brackets), the Armv6-M and Armv8-M ARMs where they differ.

#include <cstdint>

namespace uc_log::printer::cortex_m {

// SCB, Table B3-4 "Summary of SCB registers" (B3.2.2) [20657]
inline constexpr std::uint32_t cpuid = 0xE000'ED00;   // [20661]
inline constexpr std::uint32_t aircr = 0xE000'ED0C;   // [20664]
// Armv7-M / Armv8-M Mainline only: reserved on Armv6-M (CPUID.ARCHITECTURE 0xC)
inline constexpr std::uint32_t cfsr  = 0xE000'ED28;   // [20671]
inline constexpr std::uint32_t hfsr  = 0xE000'ED2C;   // [20687]
inline constexpr std::uint32_t mmfar = 0xE000'ED34;   // [20689]
inline constexpr std::uint32_t bfar  = 0xE000'ED38;   // [20690]

// CPUID.ARCHITECTURE, bits[19:16] (B3.2.3) [20806]: 0xF on Armv7-M and Armv8-M Mainline, 0xC on Armv6-M
inline constexpr unsigned      cpuidArchitectureShift = 16;
inline constexpr std::uint32_t cpuidArchitectureMask  = 0xF;
inline constexpr std::uint32_t cpuidMainline          = 0xF;

// AIRCR (B3.2.6): VECTKEY 0x05FA in bits[31:16] [20985], SYSRESETREQ bit 2 [20999]
inline constexpr std::uint32_t aircrSysResetReq = 0x05FA'0004;

// xPSR's IPSR part, Figure B1-1 [17146] and "The IPSR" (B1.4.2) [17158]: the exception number in bits[8:0], 0 in Thread
// mode
inline constexpr std::uint32_t ipsrExceptionMask = 0x1FF;

// DHCSR (C1.6.2) [25328], Debug Control Block, Armv6-M ARM DDI 0419E Table C1-10: S_SLEEP bit 18, S_LOCKUP bit 19,
// S_RESET_ST bit 25 (sticky, cleared by a read of DHCSR) [25363]
inline constexpr std::uint32_t dhcsr        = 0xE000'EDF0;
inline constexpr std::uint32_t dhcsrSleep   = 1U << 18;
inline constexpr std::uint32_t dhcsrLockup  = 1U << 19;
inline constexpr std::uint32_t dhcsrResetSt = 1U << 25;

// DEMCR (C1.6.5) [25603]: TRCENA bit 24 turns the DWT on [25627]
inline constexpr std::uint32_t demcr       = 0xE000'EDFC;
inline constexpr std::uint32_t demcrTrcena = 1U << 24;

// DWT_PCSR, Table C1-21 "DWT register summary" [27237] (Armv8-M ARM D1.2.68; Cortex-M33 TRM 100230 C3.2)
inline constexpr std::uint32_t dwtPcsr = 0xE000'101C;

// The core registers of target_op::ReadRegisters / WriteRegister, in the J-Link DLL's numbering: r0..r12, sp, lr, pc,
// xpsr, msp, psp - ReadRegisters answers coreRegisterCount little-endian words in this order
inline constexpr std::uint32_t coreRegisterCount = 19;
inline constexpr std::uint32_t regR0             = 0;
inline constexpr std::uint32_t regSp             = 13;
inline constexpr std::uint32_t regLr             = 14;
inline constexpr std::uint32_t regPc             = 15;
inline constexpr std::uint32_t regXpsr           = 16;

}   // namespace uc_log::printer::cortex_m
