#pragma once

// What other threads ask the printer's reader thread to do on the target (the J-Link is only ever driven from that
// thread): the control socket's read / write / halt / go / registers / write_registers, the TUI's debugger buttons
// and inspector, the loss-ledger watcher. Each op is answered with bytes: what was read, the word read back after a
// write, one byte (1 = halted) after halt / go, 19 little-endian words of registers, nothing after a register
// write, a breakpoint clear or a reset type.

#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <variant>

namespace uc_log::printer {

namespace target_op {
    struct Read {
        std::uint32_t address;
        std::uint32_t size;
    };

    /// Written while the core runs, then read back. DHCSR is refused: a halt written behind the J-Link DLL's back
    /// hung it for good - halt and go are ops of their own.
    struct WriteWord {
        std::uint32_t address;
        std::uint32_t value;
    };

    /// Through the probe's own calls, and only when they change something (JLINK_Go on a running core hung).
    /// `capture`: the halt is reported as if the core had stopped by itself (a `core halted:` error with its PC,
    /// the inspector's crash capture) - the TUI's Halt button; a client's halt (`capture` false) is a requested
    /// halt and captures nothing.
    struct Halt {
        bool capture{};
    };

    struct Go {};

    /// r0..r12, sp, lr, pc, xpsr, msp, psp (cortex_m::coreRegisterCount words, CortexM.hpp); only while halted.
    struct ReadRegisters {};

    /// J-Link register `index` = `value`; only while halted.
    struct WriteRegister {
        std::uint32_t index;
        std::uint32_t value;
    };

    /// Every breakpoint the probe set.
    struct ClearBreakpoints {};

    /// The J-Link reset type of the printer's ordinary resets from now on (JLINK_SetResetType; 2 = the reset
    /// line).
    struct SetResetType {
        std::uint8_t type;
    };
}   // namespace target_op

using TargetOp = std::variant<target_op::Read,
                              target_op::WriteWord,
                              target_op::Halt,
                              target_op::Go,
                              target_op::ReadRegisters,
                              target_op::WriteRegister,
                              target_op::ClearBreakpoints,
                              target_op::SetResetType>;

/// For an error message: which op failed.
inline std::string describe(TargetOp const& op) {
    struct Describe {
        std::string operator()(target_op::Read const& r) const {
            return fmt::format("reading {} bytes at {:#010x}", r.size, r.address);
        }

        std::string operator()(target_op::WriteWord const& w) const {
            return fmt::format("writing {:#010x} at {:#010x}", w.value, w.address);
        }

        std::string operator()(target_op::Halt const&) const { return "halting the core"; }

        std::string operator()(target_op::Go) const { return "resuming the core"; }

        std::string operator()(target_op::ReadRegisters) const {
            return "reading the core registers";
        }

        std::string operator()(target_op::WriteRegister const& w) const {
            return fmt::format("writing {:#010x} into register {}", w.value, w.index);
        }

        std::string operator()(target_op::ClearBreakpoints) const {
            return "clearing the breakpoints";
        }

        std::string operator()(target_op::SetResetType const& r) const {
            return fmt::format("setting reset type {}", r.type);
        }
    };

    return std::visit(Describe{}, op);
}

}   // namespace uc_log::printer
