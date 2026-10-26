#pragma once

// The last of a list of types a predicate holds for, else a default: a scope's log settings (LogEnv.hpp: the
// innermost UC_LOG_ENV setting of a kind wins) and RttLog's options (each kind at most once, so the last is the one).

#include <type_traits>

namespace uc_log::detail {

template<template<typename> class Is, typename Default, typename... Ts>
struct LastOf {
    using type = Default;
};

template<template<typename> class Is, typename Default, typename T, typename... Ts>
struct LastOf<Is, Default, T, Ts...>
  : LastOf<Is, std::conditional_t<Is<T>::value, T, Default>, Ts...> {};

template<template<typename> class Is, typename Default, typename... Ts>
using LastOfT = typename LastOf<Is, Default, Ts...>::type;

}   // namespace uc_log::detail
