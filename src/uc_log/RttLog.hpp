#pragma once

// How an application declares its RTT log backend:
//
//   template<> struct uc_log::ComBackend<uc_log::Tag::User>
//     : uc_log::RttLog<Kvasir::Chip::DebuggerPresent, uc_log::Rings<1024, 256>,
//                      uc_log::LossPolicy::Counted<Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User>::value>> {};
//
// After the debugger-present functor (AlwaysAttached.hpp) come options, in any order, each kind at most once; a kind
// left out takes its default:
//
//   kind     option                                   default
//   sizes    Rings<Main, Isr = Main>: per group a     - (required: Rings or ChannelSizes)
//            Main thread ring and Isr per ISR ring;
//            or ChannelSizes<...>, every ring's size
//   layout   RingLayout<TwoCores<CoreId>, Router>     RingLayout<> (one core, one channel)
//   mode     Mode<rtt::BufferMode::...>               Mode<rtt::BufferMode::skip>: a full ring drops the record
//   duplex   DuplexChannels<DuplexChannelConfig...>   DuplexChannels<> (none)
//   policy   IsrPolicy::SingleLevel<...>, PerLevel,   IsrPolicy::SingleLevel<>
//            MaskedRecord (IsrPolicy.hpp)
//   loss     LossPolicy::Silent / Counted<...>        LossPolicy::Silent
//
// It names exactly the RttComBackend<...> (RttComBackend.hpp) those options spell.

#include "RttComBackend.hpp"
#include "detail/LastOf.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace uc_log {

// Per group (core x logical channel) a thread ring of Main bytes and Policy::numIsrRings ISR rings of Isr bytes.
template<std::size_t Main, std::size_t Isr = Main>
struct Rings {};

template<rtt::BufferMode M>
struct Mode {
    static constexpr rtt::BufferMode value = M;
};

namespace detail {
    // the kind of an option
    template<typename T>
    struct IsRings : std::false_type {};

    template<std::size_t M, std::size_t I>
    struct IsRings<Rings<M, I>> : std::true_type {};

    template<typename T>
    struct IsChannelSizes : std::false_type {};

    template<std::size_t... S>
    struct IsChannelSizes<ChannelSizes<S...>> : std::true_type {};

    template<typename T>
    struct IsSizes : std::bool_constant<IsRings<T>::value || IsChannelSizes<T>::value> {};

    template<typename T>
    struct IsRingLayout : std::false_type {};

    template<typename C, typename R>
    struct IsRingLayout<RingLayout<C, R>> : std::true_type {};

    template<typename T>
    struct IsMode : std::false_type {};

    template<rtt::BufferMode M>
    struct IsMode<Mode<M>> : std::true_type {};

    template<typename T>
    struct IsDuplex : std::false_type {};

    template<typename... C>
    struct IsDuplex<DuplexChannels<C...>> : std::true_type {};

    template<typename T>
    concept HasIsrRings = requires { T::numIsrRings; };

    template<typename T>
    struct IsIsrPolicy : std::bool_constant<HasIsrRings<T>> {};

    template<typename T>
    struct IsLossPolicy : std::bool_constant<LossPolicy::Policy<T>> {};

    template<typename T>
    inline constexpr bool isRttLogOption
      = IsSizes<T>::value || IsRingLayout<T>::value || IsMode<T>::value || IsDuplex<T>::value
     || IsIsrPolicy<T>::value || IsLossPolicy<T>::value;

    template<typename Sizes, std::size_t Groups, std::size_t NumIsrRings>
    struct SizesOf {
        using type = Sizes;   // ChannelSizes as given
    };

    template<std::size_t Main, std::size_t Isr, std::size_t Groups, std::size_t NumIsrRings>
    struct SizesOf<Rings<Main, Isr>, Groups, NumIsrRings> {
        template<std::size_t... Is>
        static auto make(std::index_sequence<Is...>)
          -> ChannelSizes<(Is % (1 + NumIsrRings) == 0 ? Main : Isr)...>;

        using type = decltype(make(std::make_index_sequence<Groups * (1 + NumIsrRings)>{}));
    };

    template<typename DebuggerPresentFunction, typename... Options>
    struct ResolveRttLog {
        static_assert(
          (isRttLogOption<Options> && ...),
          "RttLog: an option is none of Rings/ChannelSizes, RingLayout, Mode, DuplexChannels, an "
          "IsrPolicy or a LossPolicy");
        static_assert((int{IsSizes<Options>::value} + ... + 0) == 1,
                      "RttLog: give the ring sizes once, as Rings<Main, Isr> or ChannelSizes<...>");
        static_assert((int{IsRingLayout<Options>::value} + ... + 0) <= 1,
                      "RttLog: two RingLayouts");
        static_assert((int{IsMode<Options>::value} + ... + 0) <= 1,
                      "RttLog: two Modes");
        static_assert((int{IsDuplex<Options>::value} + ... + 0) <= 1,
                      "RttLog: two DuplexChannels");
        static_assert((int{IsIsrPolicy<Options>::value} + ... + 0) <= 1,
                      "RttLog: two IsrPolicies");
        static_assert((int{IsLossPolicy<Options>::value} + ... + 0) <= 1,
                      "RttLog: two LossPolicies");

        // the defaults of the table at the top of the file: the only place they are spelled
        using Layout = LastOfT<IsRingLayout, RingLayout<>, Options...>;
        using Policy = LastOfT<IsIsrPolicy, IsrPolicy::SingleLevel<>, Options...>;
        using Sizes  = typename SizesOf<LastOfT<IsSizes, void, Options...>,
                                        Layout::cores * Layout::channels,
                                        Policy::numIsrRings>::type;

        using type = RttComBackend<DebuggerPresentFunction,
                                   Layout,
                                   Sizes,
                                   LastOfT<IsMode, Mode<rtt::BufferMode::skip>, Options...>::value,
                                   LastOfT<IsDuplex, DuplexChannels<>, Options...>,
                                   Policy,
                                   LastOfT<IsLossPolicy, LossPolicy::Silent, Options...>>;
    };
}   // namespace detail

template<typename DebuggerPresentFunction, typename... Options>
using RttLog = typename detail::ResolveRttLog<DebuggerPresentFunction, Options...>::type;

}   // namespace uc_log
