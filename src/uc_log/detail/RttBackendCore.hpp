#pragma once

// What the RTT log backend is apart from how its layout picks a ring (RttComBackend.hpp: the policy's ring in the group
// of the calling core and the level's logical channel): the control block in .noInit, the write - plain or
// LossPolicy::Counted -, the Counted bracket and lost-record mark, the panic wait, the duplex channels and the Startup
// hooks. CRTP: `Backend` names its ring with `static std::size_t ring()` - the core's write() / initTransfer() /
// finalizeTransfer() / lossMark() use it; a layout whose ring depends on the level gets level-templated ones from
// detail::LevelRings (they hide these) on top of writeOn<RingOf>() and friends.
// The helpers are always_inline and take the ring as a type, not a lambda: a lambda moved the inliner's decisions and
// changed the images.

#include "../LossPolicy.hpp"
#include "CountedRings.hpp"
#include "LogRing.hpp"
#include "RttConfigBuilder.hpp"
#include "RttEpoch.hpp"
#include "rtt/rtt.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uc_log::detail {

// ConfigBuilder: the RttConfigBuilder of the backend's rings (the log rings first, one ledger slot each)
template<typename Backend,
         typename DebuggerPresentFunction,
         rtt::BufferMode Mode,
         typename ConfigBuilder,
         typename Loss>
struct RttBackendCore : LossMembers<Backend, Loss, ConfigBuilder::numLogUpBuffers> {
private:
    using RttConfig = typename ConfigBuilder::Config;
    using RttType   = rtt::ControlBlock<RttConfig>;

    // gcc LTO drops the section attribute of an inline member without a second attribute, and the buffer lands in
    // .bss. gnu::externally_visible keeps the section and emits nothing unused; gnu::used would emit the buffer in
    // every image that names the backend - a release one too, where nothing logs - and the linker keeps it, because
    // the whole .noInit.inline input section is live for the records that share it (32 KiB of RAM). gcc warns on
    // externally_visible when a template argument is file-local (a type in an unnamed namespace): keep the backend's
    // types at namespace scope. clang keeps the section by itself and emits the member only when it is used.
    // Target only: a host build has no .noInit, and gcc refuses one TU whose instantiations of this member mix a
    // COMDAT one (external template arguments) with a local one in one named section ("section type conflict"); host
    // tests do both. .noInit.inline, not .noInit: the same rule refuses this COMDAT member beside a firmware's own
    // plain .noInit variable (Kvasir's KVASIR_NOINIT, the linker's *(.noInit*) collects both).
#if defined(__arm__) && defined(__clang__)
    [[gnu::section(".noInit.inline"), gnu::used]]
#elifdef __arm__
    [[gnu::section(".noInit.inline"), gnu::externally_visible]]
#endif
    static inline constinit typename RttType::Storage_t rttStorage;

    static inline constinit RttType rttControlBlock{rttStorage};

    using Losses = LossMembers<Backend, Loss, ConfigBuilder::numLogUpBuffers>;
    using Count  = Counting<Loss, Mode, ConfigBuilder::numLogUpBuffers>;

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
                writeLogRing<ConfigBuilder::numLogUpBuffers>(rttControlBlock, ring, span);
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
      typename LossLedger<ConfigBuilder::numLogUpBuffers>::Mark
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

    // uc_log::bootsThatLogged() (Runtime.hpp): the count noteRecord() moves, as the printer reads it
    static std::uint16_t bootsThatLogged() { return detail::loggedBootCount(); }

    static constexpr std::size_t numDuplexChannels = ConfigBuilder::numDuplexChannels;

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

    static auto lossMark() -> typename LossLedger<ConfigBuilder::numLogUpBuffers>::Mark
        requires counted
    {
        return markOn(Backend::ring());
    }

    static void lossSettle(auto const mark)
        requires counted
    {
        Losses::lossLedger.settle(mark);
    }

    // uc_log::panicMode() / drain() (Runtime.hpp); the panic wait is the backend's, for every ring
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
        static_assert(I < ConfigBuilder::numDuplexChannels,
                      "duplex channel index out of range");

        static constexpr std::string_view name
          = BufferName<ConfigBuilder::template DuplexConfigAt<I>::name.value>{};

        // host -> uc, returns the bytes read
        static std::span<std::byte> read(std::span<std::byte> buffer) {
            return rttControlBlock.template read<I>(buffer);
        }

        // uc -> host, returns the unwritten remainder
        static std::span<std::byte const> write(std::span<std::byte const> buffer) {
            if(__builtin_expect(DebuggerPresentFunction{}(), true)) {
                return rttControlBlock.template write<ConfigBuilder::numLogUpBuffers + I>(buffer);
            }
            return buffer;
        }
    };
};

}   // namespace uc_log::detail
