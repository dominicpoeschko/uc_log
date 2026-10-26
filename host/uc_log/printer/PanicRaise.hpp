#pragma once

#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Making a halted core call Kvasir::Panic::raise(cause): the TUI's "Raise panic" and the control socket's
/// `raise_panic` (kvasir_bench.py panic) - halt, R0 = cause, LR = a call from where the core stood, xPSR without
/// EPSR.IT/ICI, PC = raise, run. Through the reader's own ops (a halt written into DHCSR hangs the J-Link DLL).
namespace uc_log::printer::panic_raise {

/// The symbol raise() has in a map.
inline constexpr std::string_view raiseSymbol{"Kvasir::Panic::raise(Kvasir::Panic::Cause)"};

/// EPSR.IT/ICI, xPSR[26:25] and [15:10] (DDI0403E.e B3.5, rule RQLRN in B13.5): a halt can land inside an IT block or
/// an interrupted LDM/STM, and the debugger must keep them consistent with the new PC - so they are cleared. IPSR, T
/// and the flags stay. Armv6-M has neither: the bits read as 0 there.
inline constexpr std::uint32_t epsrItIci = (0b11U << 25) | (0x3FU << 10);

/// The register writes (J-Link register indices). LR is raise()'s return address, which the record keeps as the
/// panic's place: set to "called from where the core was halted" (`pc`), or the record names whatever LR happened to
/// hold. PC without bit 0: xPSR.T stays set.
inline std::vector<target_op::WriteRegister> registerWrites(std::uint32_t raiseAddress,
                                                            std::uint32_t cause,
                                                            std::uint32_t xpsr,
                                                            std::uint32_t pc) {
    return {
      {  cortex_m::regR0,              cause},
      {  cortex_m::regLr,     (pc + 2U) | 1U},
      {cortex_m::regXpsr,  xpsr & ~epsrItIci},
      {  cortex_m::regPc, raiseAddress & ~1U}
    };
}

/// A cause as a command line names it: its record name with dashes for spaces, KVASIR_PANIC's as "user".
inline std::string argumentName(std::size_t cause) {
    if(cause >= records::panicCauses.size()) { return "cause-" + std::to_string(cause); }
    std::string name{records::panicCauses[cause]};
    if(name == "KVASIR_PANIC") { return "user"; }
    for(auto& c : name) {
        if(c == ' ') { c = '-'; }
    }
    return name;
}

/// The cause named by its record name or its argument name; nullopt for neither.
inline std::optional<std::uint32_t> causeOf(std::string_view name) {
    for(std::size_t i = 0; i != records::panicCauses.size(); ++i) {
        if(name == records::panicCauses[i] || name == argumentName(i)) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return std::nullopt;
}

/// Every argument name, comma separated: what an unknown cause's error lists.
inline std::string argumentNames() {
    std::string out;
    for(std::size_t i = 0; i != records::panicCauses.size(); ++i) {
        if(i != 0) { out += ", "; }
        out += argumentName(i);
    }
    return out;
}

using Access = std::function<TargetPort::Result(std::vector<TargetOp> const&)>;

/// Halts the core, writes the registers and resumes it; the core runs again whatever went wrong.
inline std::expected<void,
                     std::string>
raise(Access const& drive,
      std::uint32_t raiseAddress,
      std::uint32_t cause) {
    auto const resume = [&] { static_cast<void>(drive({target_op::Go{}})); };
    auto const halted = drive({target_op::Halt{}});
    if(!halted || halted->empty() || halted->front().empty() || halted->front()[0] != std::byte{1})
    {
        resume();
        return std::unexpected{"the core did not halt"
                               + (halted ? std::string{} : ": " + halted.error())};
    }
    auto const regs = drive({target_op::ReadRegisters{}});
    if(!regs || regs->empty() || regs->front().size() < std::size_t{cortex_m::regXpsr + 1U} * 4U) {
        resume();
        return std::unexpected{"reading the registers"
                               + (regs ? std::string{} : ": " + regs.error())};
    }
    auto const&           words = regs->front();
    std::vector<TargetOp> writes;
    for(auto const& w : registerWrites(raiseAddress,
                                       cause,
                                       records::word(words, std::size_t{cortex_m::regXpsr} * 4U),
                                       records::word(words, std::size_t{cortex_m::regPc} * 4U)))
    {
        writes.emplace_back(w);
    }
    if(auto const written = drive(writes); !written) {
        resume();
        return std::unexpected{"register write: " + written.error()};
    }
    resume();
    return {};
}

}   // namespace uc_log::printer::panic_raise
