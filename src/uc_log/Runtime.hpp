#pragma once

// What an application (or the Kvasir SDK) calls on the log while the image runs. Each call goes to the tag's sink
// (ComBackend<Tag>, the hooks in Sink.hpp's table); panicMode() and drain() are nothing for a sink without the hook (a
// host test has no backend at all), so they compile in every image. bootsThatLogged() needs a sink that counts boots
// (an RTT backend): elsewhere there is no count to read, and asking is a compile error.

#include "ComBackend.hpp"
#include "Tag.hpp"

#include <cstdint>

namespace uc_log {
namespace detail {
    // the tag's backend, if it is complete (a host test has none)
    template<typename T>
    concept HasPanicMode
      = requires { sizeof(ComBackend<T>); } && requires { ComBackend<T>::enterPanicMode(); };
    template<typename T>
    concept HasDrain = requires { sizeof(ComBackend<T>); } && requires { ComBackend<T>::drain(); };
    template<typename T>
    concept HasBootCount
      = requires { sizeof(ComBackend<T>); } && requires { ComBackend<T>::bootsThatLogged(); };
}   // namespace detail

/// Zephyr's log_panic(): from here to the reset, a record that does not fit waits while the host
/// keeps reading (LossPolicy::Counted with PanicStallUs != 0). Nothing for any other backend.
/// Kvasir::Panic::beginFatal() calls it before every fatal log line.
template<typename T = Tag::User>
inline void panicMode() {
    if constexpr(detail::HasPanicMode<T>) { ComBackend<T>::enterPanicMode(); }
}

/// Wait until the host has read every log ring, or has not read for the stall time. For a reset no
/// printer was told about (SYSRESETREQ, a panic handler's reset); AnnouncedReset::announce() has
/// the printer drain by itself. Nothing for a backend that does not count.
template<typename T = Tag::User>
inline void drain() {
    if constexpr(detail::HasDrain<T>) { ComBackend<T>::drain(); }
}

/// This board's count of boots that logged - this boot included once it wrote its first record - as the printer
/// reads it to say how many boots passed unread: 16 bits, wraps. Kept across resets and across flashes of images built
/// for the same chip; undefined after a power-on. Only a backend that counts boots has it (the RTT backend:
/// detail/RttEpoch.hpp; a fan-out: its primary's).
template<typename T = Tag::User>
inline std::uint16_t bootsThatLogged() {
    static_assert(
      detail::HasBootCount<T>,
      "uc_log::bootsThatLogged(): the tag's backend counts no boots - only an RTT backend does "
      "(RttLog, or a FanOutBackend whose primary is one)");
    return ComBackend<T>::bootsThatLogged();
}
}   // namespace uc_log
