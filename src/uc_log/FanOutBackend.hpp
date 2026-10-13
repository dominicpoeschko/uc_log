#pragma once

// Every record to several backends at once: the RTT backend the printer reads AND, say, a USB backend another tool
// reads. Each sink gets every record whole or loses it whole, and counts and announces its own losses; none waits for
// another.
//
//   template<> struct uc_log::ComBackend<uc_log::Tag::User>
//     : uc_log::FanOutBackend<RttBackend, UsbBackend> {};
//
// - The record is produced once (one remote_fmt Printer, one call-site id), so every sink carries the same bytes and
//   one string catalog decodes all of them.
// - initTransfer / write / finalizeTransfer go to every sink that has them: a sink that stages its records (a
//   LossPolicy::Counted RTT backend, a queue that commits whole records) keeps doing so on its own.
// - Losses: the PRIMARY's in-band lost-record line (LossPolicy::Counted: uc_log's RecordFrame asks lossMark(), writes
//   "N log records lost on this channel" through the backend, then lossSettle()) is about the primary's ring and goes
//   to the primary only - in another sink it would be a record about someone else's losses. lossMark() forwards to
//   the primary and, when the mark has records pending, routes that context's next record to the primary alone until
//   lossSettle(). The other sinks announce their losses their own way. The primary's panic wait (enterPanicMode /
//   drain) is forwarded to every sink that has one.
// - Contexts: per core, the thread, the interrupts and NMI/HardFault each have their own "primary only" flag. An
//   interrupt's record runs with PRIMASK set (RecordGuard), so interrupt records do not nest; a thread record - a
//   lost-line too - can be preempted by any of them, which is why the flag is per context. NMI/HardFault ignore
//   PRIMASK and get the third slot.
// - RecordGuard: the sinks' own (they must agree); none if no sink has one.
// - Levels: a sink that takes the record's level (write<Level>, initTransfer<Level>, finalizeTransfer<Level>,
//   lossMark<Level> - an RTT backend that routes levels to channels, a sink that only takes warn and up) gets it.
//   The fan-out is level-aware only when one of its sinks is, so a fan-out of level-blind sinks keeps one record path
//   for every level (LevelBoundBackend.hpp); a level-blind sink in a level-aware fan-out gets every level.
// - The Startup ISR contract (isrPriorityLevels / singleIsrPriorityLevel with silentIsrPriorityLevels, IsrPolicy.hpp):
//   the one sink that declares it hands it on, so Startup still checks the enabled interrupts against it. A sink
//   without one (a queue whose contexts nest) asks nothing of them; two sinks that declare one are an error.
// - Nothing here blocks; whatever a sink does when it is full or nobody reads it stays with that sink.
//
// Contexts is how the backend learns where it runs: CortexMContexts (IPSR, one core) by default; a multicore build
// passes one whose core() tells the cores apart (FanOutBackendWith<Contexts, ...>), and the host tests a fake.

#include "ComBackend.hpp"
#include "LogLevel.hpp"
#include "detail/LevelBoundBackend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace uc_log {

namespace fanout {
    enum class Context : std::uint8_t { thread = 0, isr = 1, fault = 2 };

    inline constexpr std::size_t ContextsPerCore = 3;

#if defined(__arm__)
    // IPSR: 0 thread, 2 NMI, 3 HardFault, anything else an exception or interrupt (ARMv6-M ARM DDI 0419E B1.4.2,
    // ARMv8-M ARM DDI 0553B.y B3.30).
    struct CortexMContexts {
        static constexpr std::size_t Cores = 1;

        static Context context() {
            std::uint32_t ipsr{};
            asm volatile("mrs %0, ipsr" : "=r"(ipsr));
            if(ipsr == 0) { return Context::thread; }
            if(ipsr == 2 || ipsr == 3) { return Context::fault; }
            return Context::isr;
        }

        static std::size_t core() { return 0; }
    };
#endif

    template<typename T>
    struct GuardOf {
        using type = void;
    };

    template<typename T>
        requires requires { typename T::RecordGuard; }
    struct GuardOf<T> {
        using type = typename T::RecordGuard;
    };

    // The one guard the sinks name (void if none does); two different ones are an error.
    template<typename Found, typename... Sinks>
    struct CommonGuard {
        using type = Found;
    };

    template<typename Found, typename S, typename... Sinks>
    struct CommonGuard<Found, S, Sinks...> {
        using Mine = typename GuardOf<S>::type;
        static_assert(std::is_void_v<Found> || std::is_void_v<Mine>
                        || std::is_same_v<Found,
                                          Mine>,
                      "FanOutBackend: the sinks name different RecordGuards");
        using type = typename CommonGuard<std::conditional_t<std::is_void_v<Found>, Mine, Found>,
                                          Sinks...>::type;
    };

    template<typename Guard>
    struct GuardMember {
        using RecordGuard = Guard;
    };

    template<>
    struct GuardMember<void> {};

    template<typename S>
    concept HasIsrContract
      = requires { S::isrPriorityLevels; } || requires { S::singleIsrPriorityLevel; };

    // A sink's Startup ISR contract, as members of the fan-out
    template<typename S>
    struct IsrContractMember {};

    template<typename S>
        requires requires { S::isrPriorityLevels; }
    struct IsrContractMember<S> {
        static constexpr auto isrPriorityLevels       = S::isrPriorityLevels;
        static constexpr auto silentIsrPriorityLevels = S::silentIsrPriorityLevels;
    };

    template<typename S>
        requires requires { S::singleIsrPriorityLevel; }
    struct IsrContractMember<S> {
        static constexpr bool singleIsrPriorityLevel  = S::singleIsrPriorityLevel;
        static constexpr auto silentIsrPriorityLevels = S::silentIsrPriorityLevels;
    };

    struct NoIsrContract {};

    template<typename... Sinks>
    struct IsrContractOf {
        static_assert((static_cast<int>(HasIsrContract<Sinks>) + ... + 0) <= 1,
                      "FanOutBackend: more than one sink declares a Startup ISR contract "
                      "(isrPriorityLevels / singleIsrPriorityLevel); at most one may");
        using type = NoIsrContract;
    };

    template<typename S, typename... Sinks>
        requires HasIsrContract<S>
    struct IsrContractOf<S, Sinks...> : IsrContractOf<> {
        static_assert(
          !(
            requires { S::isrPriorityLevels; } && requires { S::singleIsrPriorityLevel; }),
          "FanOutBackend: a sink declares both isrPriorityLevels and singleIsrPriorityLevel");
        static_assert(!(HasIsrContract<Sinks> || ...),
                      "FanOutBackend: more than one sink declares a Startup ISR contract "
                      "(isrPriorityLevels / singleIsrPriorityLevel); at most one may");
        using type = IsrContractMember<S>;
    };

    template<typename S, typename... Sinks>
    struct IsrContractOf<S, Sinks...> : IsrContractOf<Sinks...> {};
}   // namespace fanout

template<typename Contexts, typename Primary, typename... Others>
struct FanOutBackendWith
  : fanout::GuardMember<typename fanout::CommonGuard<void, Primary, Others...>::type>
  , fanout::IsrContractOf<Primary, Others...>::type {
    // Listed in the Startup list like the backends it feeds, with nothing for Startup to run.
    static constexpr bool isStartupEntry = true;

    // Takes the record's level (see the top of the file): only when a sink does.
    static constexpr bool levelAware
      = (detail::LevelAware<Primary> || ... || detail::LevelAware<Others>);

    static constexpr std::size_t Slots = Contexts::Cores * fanout::ContextsPerCore;

    // Per context: the record being written now is the primary's lost-record line.
    static inline std::array<bool, Slots> primaryOnly{};

    static std::size_t slot() {
        return Contexts::core() * fanout::ContextsPerCore
             + static_cast<std::size_t>(Contexts::context());
    }

    static void initTransfer()
        requires(!levelAware)
    {
        initAll<>();
    }

    template<LogLevel Level>
    static void initTransfer()
        requires levelAware
    {
        initAll<Level>();
    }

    static void write(std::span<std::byte const> span)
        requires(!levelAware)
    {
        writeAll<>(span);
    }

    template<LogLevel Level>
    static void write(std::span<std::byte const> span)
        requires levelAware
    {
        writeAll<Level>(span);
    }

    static void finalizeTransfer()
        requires(!levelAware)
    {
        finalizeAll<>();
    }

    template<LogLevel Level>
    static void finalizeTransfer()
        requires levelAware
    {
        finalizeAll<Level>();
    }

    // The primary's lost-record line (see the top of the file): only when the primary counts losses - per level when
    // the primary keeps a ring per level (lossMark<Level>), else one for all.
    static auto lossMark()
        requires requires { Primary::lossMark(); }
    {
        return marked(Primary::lossMark());
    }

    template<LogLevel Level>
    static auto lossMark()
        requires requires { Primary::template lossMark<Level>(); }
    {
        return marked(Primary::template lossMark<Level>());
    }

    template<typename Mark>
    static void lossSettle(Mark const mark)
        requires requires { Primary::lossSettle(mark); }
    {
        Primary::lossSettle(mark);
        primaryOnly[slot()] = false;
    }

    // Startup's hook before .data/.bss are set up (an RTT backend bumps its ring epoch there, RttEpoch.hpp): every
    // sink that has one gets it - a sink's own state must not depend on whether it sits behind a fan-out.
    static void preMemoryInit()
        requires(requires { Primary::preMemoryInit(); }
                 || (requires { Others::preMemoryInit(); } || ...))
    {
        preMemoryInitOf<Primary>();
        (preMemoryInitOf<Others>(), ...);
    }

    static void noteRecord()
        requires(requires { Primary::noteRecord(); } || (requires { Others::noteRecord(); } || ...))
    {
        noteRecordOf<Primary>();
        (noteRecordOf<Others>(), ...);
    }

    static void postMemoryInit()
        requires(requires { Primary::postMemoryInit(); }
                 || (requires { Others::postMemoryInit(); } || ...))
    {
        postMemoryInitOf<Primary>();
        (postMemoryInitOf<Others>(), ...);
    }

    static void enterPanicMode()
        requires(requires { Primary::enterPanicMode(); }
                 || (requires { Others::enterPanicMode(); } || ...))
    {
        panicOf<Primary>();
        (panicOf<Others>(), ...);
    }

    static void drain()
        requires(requires { Primary::drain(); } || (requires { Others::drain(); } || ...))
    {
        drainOf<Primary>();
        (drainOf<Others>(), ...);
    }

private:
    template<typename Mark>
    static Mark marked(Mark const mark) {
        if(mark.pending != 0) { primaryOnly[slot()] = true; }
        return mark;
    }

    // Level: empty for a level-blind fan-out, else the record's one level. A sink that takes the level gets it.
    template<LogLevel... Level>
    static void initAll() {
        initOf<Primary, Level...>();
        if(primaryOnly[slot()]) { return; }
        (initOf<Others, Level...>(), ...);
    }

    template<LogLevel... Level>
    static void writeAll(std::span<std::byte const> span) {
        writeOf<Primary, Level...>(span);
        if(primaryOnly[slot()]) { return; }
        (writeOf<Others, Level...>(span), ...);
    }

    template<LogLevel... Level>
    static void finalizeAll() {
        finalizeOf<Primary, Level...>();
        if(primaryOnly[slot()]) { return; }
        (finalizeOf<Others, Level...>(), ...);
    }

    template<typename S,
             LogLevel... Level>
    static void initOf() {
        if constexpr(sizeof...(Level) == 1 && requires { S::template initTransfer<Level...>(); }) {
            S::template initTransfer<Level...>();
        } else if constexpr(requires { S::initTransfer(); }) {
            S::initTransfer();
        }
    }

    template<typename S,
             LogLevel... Level>
    static void writeOf(std::span<std::byte const> span) {
        if constexpr(sizeof...(Level) == 1 && requires { S::template write<Level...>(span); }) {
            S::template write<Level...>(span);
        } else {
            S::write(span);
        }
    }

    template<typename S,
             LogLevel... Level>
    static void finalizeOf() {
        if constexpr(sizeof...(Level) == 1
                     && requires { S::template finalizeTransfer<Level...>(); })
        {
            S::template finalizeTransfer<Level...>();
        } else if constexpr(requires { S::finalizeTransfer(); }) {
            S::finalizeTransfer();
        }
    }

    template<typename S>
    static void preMemoryInitOf() {
        if constexpr(requires { S::preMemoryInit(); }) { S::preMemoryInit(); }
    }

    template<typename S>
    static void noteRecordOf() {
        if constexpr(requires { S::noteRecord(); }) { S::noteRecord(); }
    }

    template<typename S>
    static void postMemoryInitOf() {
        if constexpr(requires { S::postMemoryInit(); }) { S::postMemoryInit(); }
    }

    template<typename S>
    static void panicOf() {
        if constexpr(requires { S::enterPanicMode(); }) { S::enterPanicMode(); }
    }

    template<typename S>
    static void drainOf() {
        if constexpr(requires { S::drain(); }) { S::drain(); }
    }
};

#if defined(__arm__)
template<typename Primary, typename... Others>
using FanOutBackend = FanOutBackendWith<fanout::CortexMContexts, Primary, Others...>;
#endif

}   // namespace uc_log
