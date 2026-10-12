#pragma once

// Where a session's RTT bytes come from, behind one interface so the reader's loop has no transport branches:
// - DirectSource: the printer's own reads (detail/DirectRtt.hpp) - every byte once, every loss said. Used wherever the
//   map gives the control block's address.
// - DllRtt: the J-Link DLL's RTT - for a block found only by the DLL's RAM search, or by choice (UC_LOG_RTT_VIA_DLL=1,
//   the probe's full throughput). Its known weaknesses: a stop drops what the DLL had read and not handed over, and
//   after an unannounced reset it can deliver old lines again; its only loss signals are its own counters.

#include "uc_log/detail/DirectRtt.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/RttControlBlock.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::detail {

template<typename Transport>
class RttSource {
public:
    using Status     = typename Transport::Status;
    using BufferDesc = typename Transport::BufferDesc;
    using Outs       = std::span<std::span<std::byte> const>;

    virtual ~RttSource() = default;

    /// The buffer counts the session started with.
    virtual Status started() const = 0;

    /// A buffer's name (detail/RttChannelMap.hpp pairs up and down buffers by it).
    virtual std::optional<BufferDesc> rttBufferDesc(bool          down,
                                                    std::uint32_t index) = 0;

    /// One live pass: up ring i's bytes into outs[i] (an empty out: ring i is not read). The result: the bytes of each.
    virtual std::vector<std::span<std::byte>> readPass(Outs outs) = 0;

    /// What each ring in `outs` holds now, whole: the end of a session.
    virtual std::vector<std::span<std::byte>> drain(Outs outs) = 0;

    /// Into down ring `index`, as much as fits; the bytes taken.
    virtual std::size_t rttWrite(std::uint32_t              index,
                                 std::span<std::byte const> data) = 0;

    /// The block's state after the last pass (running, buffer counts, bytes moved).
    virtual Status status() = 0;

    /// Nothing reads the rings from now on but drain().
    virtual void stop() {}

    /// A boot re-initialised the block during the session (the session ends; DirectRtt only).
    virtual bool reinitSeen() const { return false; }

    /// A probe failure a pass kept after handing out what it had read; taken once.
    virtual std::exception_ptr takeFailure() { return nullptr; }

    /// The RTT epoch the session began with; nullopt: no epoch fences this session.
    virtual std::optional<std::uint32_t> sessionEpoch() const { return std::nullopt; }

    /// The size of up ring `index`; 0: not known.
    virtual std::uint32_t upSize(std::size_t /*index*/) const { return 0; }

    /// The RTT epoch stayed odd at open: the bytes skipped unread then (DirectRtt::skipUnread); nullopt: it settled.
    virtual std::optional<std::uint32_t> staleSkipped() const { return std::nullopt; }
};

template<typename Transport>
class DirectSource final : public RttSource<Transport> {
public:
    using Base = RttSource<Transport>;

    // `nameRead`: the reads of the ring names (in flash: gated, DirectRtt::bufferDesc)
    DirectSource(Transport& jlink                                   UC_LOG_LIFETIMEBOUND,
                 DirectRtt<Transport>                               rtt,
                 std::function<bool(std::uint32_t,
                                    std::span<std::byte>)> nameRead UC_LOG_LIFETIMEBOUND = {})
      : jlink_{jlink}
      , rtt_{std::move(rtt)}
      , nameRead_{std::move(nameRead)} {}

    typename Base::Status started() const override { return rtt_.started(); }

    std::optional<typename Base::BufferDesc> rttBufferDesc(bool          down,
                                                           std::uint32_t index) override {
        return rtt_.bufferDesc(jlink_, down, index, nameRead_);
    }

    std::vector<std::span<std::byte>> readPass(typename Base::Outs outs) override {
        return rtt_.readAll(jlink_, outs);
    }

    std::vector<std::span<std::byte>> drain(typename Base::Outs outs) override {
        return rtt_.readAll(jlink_, outs, DirectRtt<Transport>::Cap::none);
    }

    std::size_t rttWrite(std::uint32_t              index,
                         std::span<std::byte const> data) override {
        return rtt_.write(jlink_, index, data);
    }

    typename Base::Status status() override { return rtt_.status(); }

    bool reinitSeen() const override { return rtt_.reinitSeen(); }

    std::exception_ptr takeFailure() override { return rtt_.takeFailure(); }

    std::optional<std::uint32_t> sessionEpoch() const override {
        return rtt_.epochKnown() ? std::optional<std::uint32_t>{rtt_.sessionEpoch()} : std::nullopt;
    }

    std::uint32_t upSize(std::size_t index) const override { return rtt_.upSize(index); }

    std::optional<std::uint32_t> staleSkipped() const override { return rtt_.staleSkipped(); }

private:
    Transport&                                               jlink_;
    DirectRtt<Transport>                                     rtt_;
    std::function<bool(std::uint32_t, std::span<std::byte>)> nameRead_;
};

template<typename Transport>
class DllRtt final : public RttSource<Transport> {
public:
    using Base     = RttSource<Transport>;
    using Clock    = std::chrono::steady_clock;
    using MessageF = std::function<void(std::string_view)>;

    /// `blockAddress` 0: the DLL found the block by its RAM search (no repair or dump of it then).
    DllRtt(Transport& jlink      UC_LOG_LIFETIMEBOUND,
           std::uint32_t         blockAddress,
           std::uint32_t         totalBuffers,
           MessageF errorMessage UC_LOG_LIFETIMEBOUND)
      : jlink_{jlink}
      , blockAddress_{blockAddress}
      , totalBuffers_{totalBuffers}
      , errorMessage_{std::move(errorMessage)}
      , started_{jlink.startRtt(totalBuffers,
                                blockAddress)}
      , previous_{started_} {}

    typename Base::Status started() const override { return started_; }

    std::optional<typename Base::BufferDesc> rttBufferDesc(bool          down,
                                                           std::uint32_t index) override {
        return jlink_.rttBufferDesc(down, index);
    }

    std::vector<std::span<std::byte>> readPass(typename Base::Outs outs) override {
        std::vector<std::span<std::byte>> got(outs.size());
        for(std::size_t i = 0; i != outs.size(); ++i) {
            got[i]
              = outs[i].empty() ? outs[i] : jlink_.rttRead(static_cast<std::uint32_t>(i), outs[i]);
        }
        return got;
    }

    // As far as the DLL hands out, at most DrainReads reads a ring: its host buffer can hold more than a ring.
    std::vector<std::span<std::byte>> drain(typename Base::Outs outs) override {
        drained_.assign(outs.size(), {});
        std::vector<std::span<std::byte>> got(outs.size());
        for(std::size_t i = 0; i != outs.size(); ++i) {
            if(outs[i].empty()) { continue; }
            for(int n = 0; n != DrainReads; ++n) {
                auto const data = jlink_.rttRead(static_cast<std::uint32_t>(i), outs[i]);
                if(data.empty()) { break; }
                drained_[i].insert(drained_[i].end(), data.begin(), data.end());
            }
            got[i] = drained_[i];
        }
        return got;
    }

    std::size_t rttWrite(std::uint32_t              index,
                         std::span<std::byte const> data) override {
        return jlink_.rttWrite(index, data);
    }

    // The DLL's counters, read each pass: its host overflows and target-side overflow bits are its only sign of loss.
    // After a loss the host-owned offsets are repaired and (once per ControlBlockDumpPeriod) the block is dumped.
    typename Base::Status status() override {
        auto const current = jlink_.readStatus();
        if(reportLoss(previous_, current)) {
            repairHostOffsets();
            if(Clock::now() > lastBlockDump_ + ControlBlockDumpPeriod) {
                lastBlockDump_ = Clock::now();
                dumpControlBlock();
            }
        }
        previous_ = current;
        return current;
    }

    void stop() override { jlink_.stopRtt(); }

private:
    static constexpr int  DrainReads             = 64;
    static constexpr auto OverflowReportPeriod   = std::chrono::seconds{5};
    static constexpr auto ControlBlockDumpPeriod = std::chrono::seconds{60};

    // Host overflows come once per read pass: summed up, reported once per OverflowReportPeriod.
    bool reportLoss(typename Base::Status const& previous,
                    typename Base::Status const& current) {
        // a counter that went down was restarted by the DLL
        auto const delta = [](std::uint32_t before, std::uint32_t now) {
            return now >= before ? now - before : now;
        };
        bool lost = false;
        transferred_ += delta(previous.numBytesTransferred, current.numBytesTransferred);
        read_ += delta(previous.numBytesRead, current.numBytesRead);
        if(current.hostOverflowCount > previous.hostOverflowCount) {
            lost = true;
            events_ += current.hostOverflowCount - previous.hostOverflowCount;
        }
        if(events_ != 0 && Clock::now() > lastReport_ + OverflowReportPeriod) {
            errorMessage_(
              fmt::format("⚠ RTT host overflow, log data lost ({} event{}; since the last report "
                          "{} bytes from the target, {} read by the printer)",
                          events_,
                          events_ == 1 ? "" : "s",
                          transferred_,
                          read_));
            events_     = 0;
            lastReport_ = Clock::now();
        }
        if(events_ == 0) {
            transferred_ = 0;
            read_        = 0;
        }
        auto const newOverflows = current.overflowMask & ~previous.overflowMask;
        for(std::uint32_t bit{}; bit < 32; ++bit) {
            if((newOverflows & (1U << bit)) == 0) { continue; }
            errorMessage_(fmt::format("⚠ RTT buffer {} overflow, target-side data lost", bit));
            lost = true;
        }
        return lost;
    }

    // An offset the HOST owns (an up buffer's RdOff, a down buffer's WrOff) that is out of range kills that channel
    // for good - the target sees no room / no data, and the DLL keeps reading the garbage range and writes a garbage
    // RdOff back. Set it to the target's own offset: the ring's content is dropped, the channel lives. A target-owned
    // offset out of range is left alone (the target's RAM).
    void repairHostOffsets() {
        namespace rb = rtt_block;
        if(blockAddress_ == 0) { return; }
        try {
            std::vector<std::byte> raw(rb::bytesFor(std::min<std::uint32_t>(totalBuffers_, 16U)));
            jlink_.readMemory(blockAddress_, raw);
            rb::View const block{raw};
            if(!block.hasId()) { return; }
            for(std::size_t b = 0; b != block.descriptorsRead(); ++b) {
                auto const d            = block.descriptor(b);
                bool const up           = b < block.numUp();
                auto const hostOffset   = up ? d.readOffset : d.writeOffset;
                auto const targetOffset = up ? d.writeOffset : d.readOffset;
                if(d.size == 0 || targetOffset >= d.size || hostOffset < d.size) { continue; }
                jlink_.writeWord(blockAddress_ + rb::descriptorAt(b)
                                   + (up ? rb::ReadOffsetField : rb::WriteOffsetField),
                                 targetOffset);
                errorMessage_(
                  fmt::format("RTT buffer {} ({}): the host's {} was {} in a {}-byte ring - set to "
                              "the target's {} ({}), its content is lost",
                              b,
                              up ? "up" : "down",
                              up ? "read offset" : "write offset",
                              hostOffset,
                              d.size,
                              up ? "write offset" : "read offset",
                              targetOffset));
            }
        } catch(std::exception const&) {}   // the dump that follows says why it cannot be read
    }

    // Is the control block still sane after a loss?
    void dumpControlBlock() {
        namespace rb = rtt_block;
        if(blockAddress_ == 0) { return; }
        std::string text;
        try {
            std::vector<std::byte> raw(rb::bytesFor(std::min<std::uint32_t>(totalBuffers_, 16U)));
            jlink_.readMemory(blockAddress_, raw);
            rb::View const block{raw};
            auto const     id   = block.printableId();
            bool           sane = id == rb::Id;
            text = fmt::format("RTT control block at {:#010x}: id {:?} up {} down {}",
                               blockAddress_,
                               id,
                               block.numUp(),
                               block.numDown());
            for(std::size_t b = 0; b != block.descriptorsRead(); ++b) {
                auto const d   = block.descriptor(b);
                bool const bad = !d.offsetsInRange();
                sane           = sane && !bad;
                text += fmt::format(" | {} buf={:#010x} size={} wr={} rd={} flags={}{}",
                                    b,
                                    d.buffer,
                                    d.size,
                                    d.writeOffset,
                                    d.readOffset,
                                    d.flags,
                                    bad ? " OUT OF RANGE" : "");
            }
            text += sane ? " - looks sane" : " - CORRUPT";
        } catch(std::exception const& e) {
            text = fmt::format("RTT control block at {:#010x} could not be read: {}",
                               blockAddress_,
                               e.what());
        }
        errorMessage_(text);
    }

    Transport&                          jlink_;
    std::uint32_t                       blockAddress_;
    std::uint32_t                       totalBuffers_;
    MessageF                            errorMessage_;
    typename Base::Status               started_;
    typename Base::Status               previous_;
    int                                 events_{};
    std::uint32_t                       transferred_{};
    std::uint32_t                       read_{};
    Clock::time_point                   lastReport_{};
    Clock::time_point                   lastBlockDump_{};
    std::vector<std::vector<std::byte>> drained_;
};

}   // namespace uc_log::detail
