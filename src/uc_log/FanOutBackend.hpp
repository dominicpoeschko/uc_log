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
//   without one (a queue whose contexts nest) asks nothing of them; two sinks that declare one are an error. Its
//   perCoreIsrContexts is handed on too when Contexts tells the cores apart (Contexts::cores > 1): the fan-out's own
//   flags are then per core as well.
// - Duplex channels (numDuplexChannels, DuplexChannel<I>): the primary's, handed on; another sink may have none.
// - kLevelFloor: handed on when every level-aware sink declares the same one (a router declares none).
// - Nothing here blocks; whatever a sink does when it is full or nobody reads it stays with that sink.
//
// Contexts is how the backend learns where it runs (CortexM.hpp), the same type an RTT backend's RingLayout names:
// SingleCore (IPSR, one core) by default; FanOutBackendMulticore<CoreId, ...> takes TwoCores<CoreId>, and the host tests
// pass a fake. Every hook a sink may have is listed in Sink.hpp.

#include "ComBackend.hpp"
#include "CortexM.hpp"
#include "LogLevel.hpp"
#include "Sink.hpp"
#include "detail/LevelBoundBackend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace uc_log {

namespace fanout {
    // The one guard the sinks name (void if none does); two different ones are an error.
    template<typename Found, typename... Sinks>
    struct CommonGuard {
        using type = Found;
    };

    template<typename Found, typename S, typename... Sinks>
    struct CommonGuard<Found, S, Sinks...> {
        using Mine = typename RecordGuardOf<S>::type;
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

    // perCoreIsrContexts, when the sink declares it and the fan-out's contexts are per core too
    template<typename S, typename Contexts>
    struct PerCoreMember {};

    template<typename S, typename Contexts>
        requires(requires { S::perCoreIsrContexts; } && Contexts::cores > 1)
    struct PerCoreMember<S, Contexts> {
        static constexpr bool perCoreIsrContexts = S::perCoreIsrContexts;
    };

    template<typename S>
    concept HasIsrContract
      = requires { S::isrPriorityLevels; } || requires { S::singleIsrPriorityLevel; };

    // A sink's Startup ISR contract, as members of the fan-out
    template<typename S, typename Contexts>
    struct IsrContractMember : PerCoreMember<S, Contexts> {};

    template<typename S, typename Contexts>
        requires requires { S::isrPriorityLevels; }
    struct IsrContractMember<S, Contexts> : PerCoreMember<S, Contexts> {
        static constexpr auto isrPriorityLevels       = S::isrPriorityLevels;
        static constexpr auto silentIsrPriorityLevels = S::silentIsrPriorityLevels;
    };

    template<typename S, typename Contexts>
        requires requires { S::singleIsrPriorityLevel; }
    struct IsrContractMember<S, Contexts> : PerCoreMember<S, Contexts> {
        static constexpr bool singleIsrPriorityLevel  = S::singleIsrPriorityLevel;
        static constexpr auto silentIsrPriorityLevels = S::silentIsrPriorityLevels;
    };

    struct NoIsrContract {};

    // the primary: no sink left (the specialisations below take one off the front)
    template<typename Contexts, typename... Sinks>
    struct IsrContractOf {
        using type = NoIsrContract;
    };

    template<typename Contexts, typename S, typename... Sinks>
        requires HasIsrContract<S>
    struct IsrContractOf<Contexts, S, Sinks...> : IsrContractOf<Contexts> {
        static_assert(
          !(
            requires { S::isrPriorityLevels; } && requires { S::singleIsrPriorityLevel; }),
          "FanOutBackend: a sink declares both isrPriorityLevels and singleIsrPriorityLevel");
        static_assert(!(HasIsrContract<Sinks> || ...),
                      "FanOutBackend: more than one sink declares a Startup ISR contract "
                      "(isrPriorityLevels / singleIsrPriorityLevel); at most one may");
        using type = IsrContractMember<S, Contexts>;
    };

    template<typename Contexts, typename S, typename... Sinks>
    struct IsrContractOf<Contexts, S, Sinks...> : IsrContractOf<Contexts, Sinks...> {};

    template<typename S>
    concept HasDuplex = requires { S::numDuplexChannels; };

    template<typename S>
    concept HasDuplexChannels = HasDuplex<S> && (S::numDuplexChannels > 0);

    // The primary's duplex channels, as members of the fan-out
    template<typename Primary>
    struct DuplexMember {};

    template<typename Primary>
        requires HasDuplex<Primary>
    struct DuplexMember<Primary> {
        static constexpr std::size_t numDuplexChannels = Primary::numDuplexChannels;

        template<std::size_t I>
        using DuplexChannel = typename Primary::template DuplexChannel<I>;
    };

    template<typename S>
    concept HasLevelFloor = requires { S::kLevelFloor; };

    struct FloorOf {
        bool     one{true};   // every level-aware sink so far declares the same floor
        bool     any{false};
        LogLevel floor{};
    };

    template<typename S>
    consteval FloorOf joinFloor(FloorOf const f) {
        if constexpr(!detail::LevelAware<S>) {
            return f;
        } else if constexpr(!HasLevelFloor<S>) {
            return {false, f.any, f.floor};
        } else {
            return {f.one && (!f.any || f.floor == S::kLevelFloor), true, S::kLevelFloor};
        }
    }

    template<typename... Sinks>
    consteval FloorOf floorOf() {
        FloorOf f{};
        ((f = joinFloor<Sinks>(f)), ...);
        return f;
    }

    // kLevelFloor: the one every level-aware sink declares; none when one of them declares none or they differ
    template<typename... Sinks>
    struct LevelFloorMember {};

    template<typename... Sinks>
        requires(floorOf<Sinks...>().one && floorOf<Sinks...>().any)
    struct LevelFloorMember<Sinks...> {
        static constexpr LogLevel kLevelFloor = floorOf<Sinks...>().floor;
    };
}   // namespace fanout

template<typename Contexts, typename Primary, typename... Others>
struct FanOutBackendWith
  : fanout::GuardMember<typename fanout::CommonGuard<void, Primary, Others...>::type>
  , fanout::IsrContractOf<Contexts, Primary, Others...>::type
  , fanout::DuplexMember<Primary>
  , fanout::LevelFloorMember<Primary, Others...> {
    static_assert(Contexts::cores > 1 || !(requires {
                      Primary::perCoreIsrContexts;
                  } || (requires { Others::perCoreIsrContexts; } || ...)),
                  "FanOutBackend: a sink's ISR contexts are per core - give FanOutBackendWith a "
                  "multicore Contexts "
                  "(FanOutBackendMulticore<CoreId, ...>, TwoCores<CoreId>)");
    static_assert(!(fanout::HasDuplexChannels<Others> || ...),
                  "FanOutBackend: duplex channels only on the primary sink");

    // Listed in the Startup list like the backends it feeds, with nothing for Startup to run.
    static constexpr bool isStartupEntry = true;

    // Takes the record's level (see the top of the file): only when a sink does.
    static constexpr bool levelAware
      = (detail::LevelAware<Primary> || ... || detail::LevelAware<Others>);

    static constexpr std::size_t slots = Contexts::cores * contextsPerCore;

    // Per context: the record being written now is the primary's lost-record line.
    static inline std::array<bool, slots> primaryOnly{};

    static std::size_t slot() {
        return Contexts::core() * contextsPerCore + static_cast<std::size_t>(Contexts::context());
    }

    static void initTransfer()
        requires(!levelAware)
    {
        initAll<LogLevel::info>();
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
        writeAll<LogLevel::info>(span);
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
        finalizeAll<LogLevel::info>();
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

    // uc_log::bootsThatLogged(): the primary's count (the RTT backend's), as for the lost-record line
    static auto bootsThatLogged()
        requires requires { Primary::bootsThatLogged(); }
    {
        return Primary::bootsThatLogged();
    }

    // Startup's hooks around the .data/.bss setup (an RTT backend bumps its ring epoch there, RttEpoch.hpp), RecordFrame's
    // noteRecord, the panic wait: every sink that has one gets it - a sink's own state must not depend on whether it
    // sits behind a fan-out.
#define UC_LOG_DETAIL_FAN_HOOK(name)                                                     \
public:                                                                                  \
    static void name()                                                                   \
        requires(requires { Primary::name(); } || (requires { Others::name(); } || ...)) \
    {                                                                                    \
        name##Of<Primary>();                                                             \
        (name##Of<Others>(), ...);                                                       \
    }                                                                                    \
                                                                                         \
private:                                                                                 \
    template<typename S>                                                                 \
    static void name##Of() {                                                             \
        if constexpr(requires { S::name(); }) { S::name(); }                             \
    }

    UC_LOG_DETAIL_FAN_HOOK(preMemoryInit)
    UC_LOG_DETAIL_FAN_HOOK(noteRecord)
    UC_LOG_DETAIL_FAN_HOOK(postMemoryInit)
    UC_LOG_DETAIL_FAN_HOOK(enterPanicMode)
    UC_LOG_DETAIL_FAN_HOOK(drain)
#undef UC_LOG_DETAIL_FAN_HOOK

private:
    template<typename Mark>
    static Mark marked(Mark const mark) {
        if(mark.pending != 0) { primaryOnly[slot()] = true; }
        return mark;
    }

    // Level: LogLevel::info for a level-blind fan-out (one record path for every level), else the record's level.
    // LevelBoundBackend hands it to a sink that takes the level, as uc_log does for a sink that is no fan-out.
    template<LogLevel Level>
    static void initAll() {
        detail::LevelBoundBackend<Primary, Level>::initTransfer();
        if(primaryOnly[slot()]) { return; }
        (detail::LevelBoundBackend<Others, Level>::initTransfer(), ...);
    }

    template<LogLevel Level>
    static void writeAll(std::span<std::byte const> span) {
        detail::LevelBoundBackend<Primary, Level>::write(span);
        if(primaryOnly[slot()]) { return; }
        (detail::LevelBoundBackend<Others, Level>::write(span), ...);
    }

    template<LogLevel Level>
    static void finalizeAll() {
        detail::LevelBoundBackend<Primary, Level>::finalizeTransfer();
        if(primaryOnly[slot()]) { return; }
        (detail::LevelBoundBackend<Others, Level>::finalizeTransfer(), ...);
    }
};

#ifdef __arm__
template<typename Primary, typename... Others>
using FanOutBackend = FanOutBackendWith<SingleCore, Primary, Others...>;

// Two cores told apart by CoreId (Kvasir::Sio::CpuId on the RP chips): per core and context a "primary only" flag, and
// a primary's perCoreIsrContexts handed on to Startup.
template<typename CoreId, typename Primary, typename... Others>
using FanOutBackendMulticore = FanOutBackendWith<TwoCores<CoreId>, Primary, Others...>;
#endif

}   // namespace uc_log
