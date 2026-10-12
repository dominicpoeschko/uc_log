#pragma once

// What every RTT log backend is apart from how it picks a ring (DefaultRttComBackend: the policy's ring of one group,
// MulticoreRttComBackend: the calling core's group, MultiChannelRttComBackend: the level's logical channel): the
// control block in .noInit, the write - plain or LossPolicy::Counted -, the Counted bracket and lost-record mark, the
// panic wait, the duplex channels and the Startup hooks. CRTP: `Backend` names its ring with `static std::size_t
// ring()` - the core's write() / initTransfer() / finalizeTransfer() / lossMark() use it; a backend whose ring depends
// on the level declares its own level-templated ones (they hide these) on top of writeOn<RingOf>() and friends.
// The helpers are always_inline and take the ring as a type: each backend's write() is then the very body it had
// before the core existed - the images stayed byte-identical (a lambda instead moved the inliner's decisions).

#include "../LossPolicy.hpp"
#include "CountedRings.hpp"
#include "LogRing.hpp"
#include "RttConfigBuilder.hpp"
#include "RttEpoch.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace uc_log::detail {

// ConfigBuilder: the RttConfigBuilder of the backend's rings (the log rings first, one ledger slot each)
template<typename Backend,
         typename DebuggerPresentFunction,
         rtt::BufferMode Mode,
         typename ConfigBuilder,
         typename Loss>
struct RttBackendCore : LossMembers<Backend, Loss, ConfigBuilder::NumLogUpBuffers> {
private:
    using RttConfig = typename ConfigBuilder::Config;
    using RttType   = rtt::ControlBlock<RttConfig>;

    // gnu::used: gcc LTO otherwise drops the section attribute and the buffer lands in .bss
    [[gnu::section(".noInit"), gnu::used]] static inline constinit
      typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

    using Losses = LossMembers<Backend, Loss, ConfigBuilder::NumLogUpBuffers>;
    using Count  = Counting<Loss, Mode, ConfigBuilder::NumLogUpBuffers>;

protected:
    static constexpr bool counted = LossPolicy::isCounted<Loss>;

    // the write to the ring `RingOf::ring()` names, asked only when there is something to write (a plain backend skips
    // the policy's look at the core while no debugger is attached)
    template<typename RingOf>
    [[gnu::always_inline]] static void writeOn(std::span<std::byte const> span) {
        if constexpr(!counted) {
            if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
                std::size_t const ring = RingOf::ring();
                if(ring == noLogRing) { return; }
                writeLogRing<ConfigBuilder::NumLogUpBuffers>(rttControlBlock, ring, span);
            }
        } else {
            Count::write(rttControlBlock,
                         Losses::lossLedger,
                         Losses::panicWait,
                         RingOf::ring(),
                         __builtin_expect(DebuggerPresentFunction{}(), true),
                         span);
        }
    }

    [[gnu::always_inline]] static void beginOn(std::size_t const ring)
        requires counted
    {
        Count::begin(rttControlBlock, Losses::lossLedger, ring);
    }

    [[gnu::always_inline]] static void endOn(std::size_t const ring)
        requires counted
    {
        Count::end(rttControlBlock, Losses::lossLedger, ring);
    }

    [[gnu::always_inline]] static auto markOn(std::size_t const ring) ->
      typename LossLedger<ConfigBuilder::NumLogUpBuffers>::Mark
        requires counted
    {
        return Losses::lossLedger.mark(ring);
    }

public:
    // Listed in an application's Startup by convention (the log transport belongs next to the peripherals), with
    // nothing for Startup to run: this is what tells Startup's "every entry is a peripheral" rule that the listing is
    // deliberate. Startup also checks the policy's contract members against the enabled interrupts.
    static constexpr bool isStartupEntry = true;

    // Startup, before and after .data (and with it the control block) is initialised: the epoch odd, then even - a
    // printer that wrote a read offset across the re-init knows it, and none opens the old block meanwhile (RttEpoch.hpp)
    static void preMemoryInit() { epochBlockStale(); }

    static void postMemoryInit() { epochBlockReady(); }

    // RecordFrame, before a record's first byte: the boot's first record counts it as a boot that logged
    // (RttEpoch.hpp), so the printer can say how many such boots nobody read
    static void noteRecord() { detail::noteRecord(); }

    static constexpr std::size_t NumDuplexChannels = ConfigBuilder::NumDuplexChannels;

    static void write(std::span<std::byte const> span) { writeOn<Backend>(span); }

    // LossPolicy::Counted: remote_fmt's Printer brackets each record with these (via LevelBoundBackend), RecordFrame
    // asks lossMark/lossSettle for the lost-record line
    static void initTransfer()
        requires counted
    {
        beginOn(Backend::ring());
    }

    static void finalizeTransfer()
        requires counted
    {
        endOn(Backend::ring());
    }

    static auto lossMark() -> typename LossLedger<ConfigBuilder::NumLogUpBuffers>::Mark
        requires counted
    {
        return markOn(Backend::ring());
    }

    static void lossSettle(auto const mark)
        requires counted
    {
        Losses::lossLedger.settle(mark);
    }

    // uc_log::panicMode() / drain(); the panic wait is the backend's, for every ring
    static void enterPanicMode()
        requires(counted && Count::waits)
    {
        Losses::panicWait.on = true;
    }

    static void drain()
        requires(counted && Count::waits)
    {
        Count::drain(rttControlBlock, Losses::panicWait);
    }

    // a duplex channel is single-producer/single-consumer: concurrent ISR + thread access to the same channel, or
    // access from both cores, is the application's responsibility
    template<std::size_t I>
    struct DuplexChannel {
        static_assert(I < ConfigBuilder::NumDuplexChannels,
                      "duplex channel index out of range");

        static constexpr std::string_view name
          = DuplexBufferName<ConfigBuilder::template DuplexConfigAt<I>::Name>{};

        // host -> uc, returns the bytes read
        static std::span<std::byte> read(std::span<std::byte> buffer) {
            return rttControlBlock.template read<I>(buffer);
        }

        // uc -> host, returns the unwritten remainder
        static std::span<std::byte const> write(std::span<std::byte const> buffer) {
            if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
                return rttControlBlock.template write<ConfigBuilder::NumLogUpBuffers + I>(buffer);
            }
            return buffer;
        }
    };
};

}   // namespace uc_log::detail
