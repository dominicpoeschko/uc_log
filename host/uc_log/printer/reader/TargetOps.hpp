#pragma once

// What other threads ask of the target (printer/TargetOp.hpp: the control socket, the TUI, the inspector), queued for
// the reader thread - the J-Link is only ever driven from there - and the core's halt state those ops and the read
// passes share. Included only by JLinkRttReaderImpl.hpp.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/reader/FlashGate.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::printer::reader {

template<typename TransportT>
class TargetOps {
    using Clock = uc_log::printer::Clock;

public:
    using TargetOp = uc_log::printer::TargetOp;
    using Result   = uc_log::printer::TargetPort::Result;

    /// Reads in the chip's flash window go through the gate, as the firmware check's do.
    explicit TargetOps(FlashGate<TransportT>& gate UC_LOG_LIFETIMEBOUND) : gate_{gate} {}

    // The core's halt as the reader sees it: `halted` is the last look (any thread reads it); `haltRequested` - the
    // halt came from a halt request; `resumed` - a go request ran the core since the reader noted the halt (reader
    // thread only, both).
    std::atomic<bool> halted{false};
    bool              haltRequested{false};
    bool              resumed{false};
    // SetResetType's: what the ordinary resets use
    std::atomic<std::uint8_t> resetType{};

    /// Runs `ops` on the reader thread (serve). From any thread; blocks until done or `timeout`.
    Result access(std::span<TargetOp const> ops,
                  std::chrono::milliseconds timeout) {
        auto pending = std::make_shared<Pending>();
        pending->ops.assign(ops.begin(), ops.end());
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            queue_.push_back(pending);
        }
        std::unique_lock<std::mutex> lock{pending->mutex};
        auto const answered = [&pending]() { return pending->result.has_value(); };
        if(!pending->done.wait_for(lock, timeout, answered) && pending->claim(Pending::abandoned)) {
            return std::unexpected{std::string{"timed out: is the target connected?"}};
        }
        // done, or the reader has started it: its result is the truth, a late one included
        pending->done.wait(lock, answered);
        // answered(): it holds one
        if(!pending->result) { return std::unexpected{std::string{"no answer"}}; }
        return std::move(*pending->result);
    }

    /// Serves only what is queued now: clients asking in a loop would otherwise starve the RTT reads.
    void serve(TransportT& jlink) {
        std::deque<std::shared_ptr<Pending>> batch;
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            batch.swap(queue_);
        }
        for(auto const& pending : batch) {
            // the requester may give up at the same moment: whoever moves the state first wins
            if(!pending->claim(Pending::taken)) { continue; }
            std::size_t done = 0;
            try {
                std::vector<std::vector<std::byte>> answers;
                answers.reserve(pending->ops.size());
                for(; done != pending->ops.size(); ++done) {
                    answers.push_back(run(jlink, pending->ops[done]));
                }
                pending->setResult(std::move(answers));
            } catch(std::exception const& e) {
                pending->setResult(std::unexpected{
                  fmt::format("{} ({})", e.what(), uc_log::printer::describe(pending->ops[done]))});
            }
        }
    }

    /// Everything queued fails with `why` (no target to run it on).
    void failAll(std::string const& why) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        for(auto& pending : queue_) {
            if(pending->claim(Pending::taken)) { pending->setResult(std::unexpected{why}); }
        }
        queue_.clear();
    }

private:
    // queued -> taken (the reader thread runs it, the requester waits for its result) or queued -> abandoned (the
    // requester timed out first, the reader skips it). Never both: a write the reader has started is not reported as
    // timed out (that was possible once: a poke "failed" and took effect).
    struct Pending {
        enum State : int { queued, taken, abandoned };

        std::vector<TargetOp>    ops;
        std::atomic<int>         state{queued};
        std::mutex               mutex;
        Clock::ConditionVariable done;
        std::optional<Result>    result;   // mutex

        void setResult(Result r) {
            {
                std::scoped_lock<std::mutex> const lock{mutex};
                result = std::move(r);
            }
            done.notify_all();
        }

        bool claim(State to) {
            int expected = queued;
            return state.compare_exchange_strong(expected, to, std::memory_order_acq_rel);
        }
    };

    // One op on the reader thread; its answer's bytes.
    std::vector<std::byte> run(TransportT&     jlink,
                               TargetOp const& op) {
        namespace to          = uc_log::printer::target_op;
        using Reg             = typename TransportT::CoreRegister;
        auto const haltedOnly = [&jlink] {
            if(!jlink.isHalted()) { throw std::runtime_error{"the core runs: halt it first"}; }
        };
        if(auto const* r = std::get_if<to::Read>(&op)) {
            std::vector<std::byte> out(r->size);
            if(gate_.inWindow(
                 r->address,
                 r->size)) {   // through the flash's gate, as the firmware check's reads
                if(!gate_.read(jlink)(r->address, out)) {
                    throw std::runtime_error{
                      "not read: the flash is not mapped now (a boot ROM drives it) or the core "
                      "was "
                      "just reset"};
                }
                return out;
            }
            jlink.readMemory(r->address, out);
            return out;
        }
        if(auto const* w = std::get_if<to::WriteWord>(&op)) {
            if(w->address == uc_log::printer::cortex_m::dhcsr) {
                throw std::runtime_error{
                  "DHCSR is the probe's: halt and resume through the halt / go requests "
                  "(a halt written behind the J-Link DLL's back hangs it)"};
            }
            jlink.writeWord(w->address, w->value);
            std::vector<std::byte> out(4);
            jlink.readMemory(w->address, out);
            return out;
        }
        if(std::holds_alternative<to::Halt>(op) || std::holds_alternative<to::Go>(op)) {
            bool const  wasHalted = jlink.isHalted();
            auto const* halt      = std::get_if<to::Halt>(&op);
            if(halt != nullptr && !wasHalted) {
                jlink.halt();
                // a captured halt is noted as one the core made itself (the reader's noteHalt: `core halted:`)
                haltRequested = !halt->capture;
            }
            if(std::holds_alternative<to::Go>(op) && wasHalted) {
                jlink.go();
                haltRequested = false;
                resumed       = true;
            }
            halted = jlink.isHalted();
            return std::vector<std::byte>(1U,
                                          std::byte{halted ? std::uint8_t{1} : std::uint8_t{0}});
        }
        if(std::holds_alternative<to::ReadRegisters>(op)) {
            haltedOnly();
            std::vector<std::byte> out;
            out.reserve(std::size_t{uc_log::printer::cortex_m::coreRegisterCount} * 4U);
            for(std::uint32_t reg = 0; reg != uc_log::printer::cortex_m::coreRegisterCount; ++reg) {
                uc_log::printer::appendLe32(out, jlink.readRegister(static_cast<Reg>(reg)));
            }
            return out;
        }
        if(std::holds_alternative<to::ClearBreakpoints>(op)) {
            jlink.clearAllBreakpoints();
            return {};
        }
        if(auto const* r = std::get_if<to::SetResetType>(&op)) {
            resetType.store(r->type, std::memory_order_relaxed);
            jlink.setResetType(r->type);
            return {};
        }
        auto const& w = std::get<to::WriteRegister>(op);
        haltedOnly();
        jlink.writeRegister(static_cast<Reg>(w.index), w.value);
        return {};
    }

    FlashGate<TransportT>&               gate_;
    std::mutex                           mutex_;
    std::deque<std::shared_ptr<Pending>> queue_;   // mutex_
};

}   // namespace uc_log::printer::reader
