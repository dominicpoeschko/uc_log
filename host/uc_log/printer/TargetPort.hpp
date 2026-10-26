#pragma once

// The printer's one door to the target, as the front ends, the control socket and the inspector see it. The reader
// (BasicJLinkRttReader) implements it; every request that touches the core or memory goes through `accessTarget`,
// the reader thread's TargetOp queue (printer/TargetOp.hpp: queued -> taken | abandoned). The phase-level requests
// (flash, reset, J-Link reset, probe list) are flags the reader serves outside its sessions: a flash with no
// session running must work, and nobody waits for them here.

#include "uc_log/printer/TargetOp.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace uc_log::printer {

class TargetPort {
public:
    /// Bytes per op, or why not.
    using Result = std::expected<std::vector<std::vector<std::byte>>, std::string>;

    /// Long enough for a busy probe: on an RP2040 at 2 MHz SWD under a log flood one pass of the reader took over
    /// 250 ms (the DLL's own RTT polling holds the probe), and pokes failed.
    static constexpr std::chrono::milliseconds defaultTimeout{2000};

    /// The RTT link as the probe reports it (JLINK_RTTERMINAL_Control's status).
    struct LinkStatus {
        std::uint32_t numBytesTransferred{};
        std::uint32_t numBytesRead{};
        int           hostOverflowCount{};
        int           isRunning{};
        int           numUpBuffers{};
        int           numDownBuffers{};
    };

    struct Probe {
        std::uint32_t serialNumber{};
        std::string   product{};
        std::string   nickName{};
        bool          onUsb{};
    };

    /// What the transport is told to open: `host` non-empty means J-Link over IP at that address, otherwise
    /// `probe` names a J-Link by serial number or nickname (empty: the only one on USB).
    struct ConnectionSettings {
        std::string host{};
        std::string probe{};
    };

    TargetPort()                             = default;
    TargetPort(TargetPort const&)            = delete;
    TargetPort& operator=(TargetPort const&) = delete;
    virtual ~TargetPort()                    = default;

    /// Runs `ops` on the reader thread, in order, the core running unless an op halts it. From any thread; blocks
    /// until done or `timeout` (then the ops are withdrawn, never half run).
    virtual Result accessTarget(std::span<printer::TargetOp const> ops,
                                std::chrono::milliseconds          timeout) = 0;

    [[nodiscard]] virtual LinkStatus    linkStatus() const = 0;
    [[nodiscard]] virtual bool          isHalted() const   = 0;
    [[nodiscard]] virtual bool          isFlashing() const = 0;
    [[nodiscard]] virtual std::uint64_t sessionCount() const
      = 0;   // a new session: records and map again

    virtual void flash()       = 0;
    virtual void resetTarget() = 0;
    virtual void resetJLink()  = 0;

    /// Asks the reader thread to list the probes; getProbes() has the result once getProbesVersion() moved.
    virtual void                             refreshProbes()          = 0;
    [[nodiscard]] virtual std::vector<Probe> getProbes() const        = 0;
    [[nodiscard]] virtual std::uint64_t      getProbesVersion() const = 0;

    virtual void setConnection(ConnectionSettings settings) = 0;
    virtual void setNoLogTimeout(std::uint32_t seconds)     = 0;
};

}   // namespace uc_log::printer
