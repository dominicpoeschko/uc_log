#pragma once

// A halt the core made itself (the fault handler's bkpt, a panic), captured on the reader thread: the registers, the
// mainline fault registers and the stack words (printer/HaltCapture.hpp has the struct and its `core halted:` text).
// Only reads; whoever halted the core resumes it. Included only by JLinkRttReaderImpl.hpp.

#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/HaltCapture.hpp"
#include "uc_log/printer/Le32.hpp"

#include <cstdint>

namespace uc_log::printer::reader {

inline constexpr std::uint32_t haltStackWords = 24;

template<typename TransportT>
HaltCapture captureHalt(TransportT& jlink) {
    using Reg = typename TransportT::CoreRegister;
    auto const readWord
      = [&](std::uint32_t address) { return uc_log::printer::tryReadWord(jlink, address); };
    HaltCapture capture{.pc   = jlink.readRegister(Reg::pc),
                        .lr   = jlink.readRegister(Reg::lr),
                        .sp   = jlink.readRegister(Reg::sp),
                        .xpsr = jlink.readRegister(Reg::xpsr),
                        .msp  = jlink.readRegister(Reg::msp),
                        .psp  = jlink.readRegister(Reg::psp)};
    // CPUID.ARCHITECTURE 0xF = mainline, which has CFSR/HFSR/MMFAR/BFAR; on Armv6-M (0xC)
    // those addresses are reserved (RP2040 data sheet table 104, RP2350 table 200).
    namespace cm = uc_log::printer::cortex_m;
    if(auto const cpuid = readWord(cm::cpuid);
       cpuid
       && ((*cpuid >> cm::cpuidArchitectureShift) & cm::cpuidArchitectureMask) == cm::cpuidMainline)
    {
        capture.fault = HaltCapture::FaultRegisters{.cfsr  = readWord(cm::cfsr),
                                                    .hfsr  = readWord(cm::hfsr),
                                                    .mmfar = readWord(cm::mmfar),
                                                    .bfar  = readWord(cm::bfar)};
    }
    capture.stack.reserve(haltStackWords);
    for(std::uint32_t i = 0; i != haltStackWords; ++i) {
        capture.stack.push_back(readWord(capture.sp + 4U * i));
    }
    return capture;
}

}   // namespace uc_log::printer::reader
