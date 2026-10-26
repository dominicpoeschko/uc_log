#pragma once

// What the reader captures when the core halts on its own (a fault handler's or a panic's bkpt, a lockup) or on the
// TUI's captured halt: the registers, the mainline fault registers and the words on the stack. The front ends take the
// struct (the Health tab's crash view, TargetInspector::noteHalt); `text()` is the status message every script reads
// (kvasir_bench's `crash`, `wait-for` ending on "core halted"), byte for byte as before the struct.

#include "uc_log/printer/CortexM.hpp"

#include <array>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

struct HaltCapture {
    /// CFSR, HFSR, MMFAR, BFAR: only on a mainline core (CPUID.ARCHITECTURE 0xF; reserved on Armv6-M); each nullopt
    /// when its read failed.
    struct FaultRegisters {
        std::optional<std::uint32_t> cfsr;
        std::optional<std::uint32_t> hfsr;
        std::optional<std::uint32_t> mmfar;
        std::optional<std::uint32_t> bfar;
    };

    std::uint32_t                 pc{};
    std::uint32_t                 lr{};
    std::uint32_t                 sp{};
    std::uint32_t                 xpsr{};
    std::uint32_t                 msp{};
    std::uint32_t                 psp{};
    std::optional<FaultRegisters> fault{};   // nullopt: an Armv6-M core, or CPUID unreadable
    /// the words from sp up; nullopt: unreadable
    std::vector<std::optional<std::uint32_t>> stack{};

    /// The exception the core was in (0: Thread mode).
    [[nodiscard]] std::uint32_t exception() const {
        return xpsr & uc_log::printer::cortex_m::ipsrExceptionMask;
    }

    /// Every register that was read, by the name the message gives it: pc, lr, sp, xpsr, msp, psp, then the
    /// readable fault registers.
    [[nodiscard]] std::vector<std::pair<std::string_view,
                                        std::uint32_t>>
    registers() const {
        std::vector<std::pair<std::string_view, std::uint32_t>> out{
          {  "pc",   pc},
          {  "lr",   lr},
          {  "sp",   sp},
          {"xpsr", xpsr},
          { "msp",  msp},
          { "psp",  psp}
        };
        if(fault) {
            for(auto const& [name, value] : faultRegisters(*fault)) {
                if(value) { out.emplace_back(name, *value); }
            }
        }
        return out;
    }

    /// The status message: "core halted: pc=0x... lr=... sp=... xpsr=... msp=... psp=... exception=N [cfsr=...
    /// hfsr=... mmfar=... bfar=...] stack: xxxxxxxx ????????"
    [[nodiscard]] std::string text() const {
        auto out = fmt::format(
          "core halted: pc={:#010x} lr={:#010x} sp={:#010x} "
          "xpsr={:#010x} msp={:#010x} psp={:#010x} exception={}",
          pc,
          lr,
          sp,
          xpsr,
          msp,
          psp,
          exception());
        auto const word = [](std::optional<std::uint32_t> v) {
            return v ? fmt::format("{:#010x}", *v) : std::string{"????????"};
        };
        if(fault) {
            out += fmt::format(" cfsr={} hfsr={} mmfar={} bfar={}",
                               word(fault->cfsr),
                               word(fault->hfsr),
                               word(fault->mmfar),
                               word(fault->bfar));
        }
        out += " stack:";
        for(auto const& v : stack) {
            out += v ? fmt::format(" {:08x}", *v) : std::string{" ????????"};
        }
        return out;
    }

private:
    static std::array<std::pair<std::string_view,
                                std::optional<std::uint32_t>>,
                      4>
    faultRegisters(FaultRegisters const& f) {
        return {
          {{"cfsr", f.cfsr}, {"hfsr", f.hfsr}, {"mmfar", f.mmfar}, {"bfar", f.bfar}}
        };
    }
};

}   // namespace uc_log::printer
