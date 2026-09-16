#pragma once

namespace uc_log {

/// The DebuggerPresentFunction for a target that always assumes a host is listening. With
/// rtt::BufferMode::skip a full buffer drops lines rather than stalling, so an unattached
/// board keeps running either way, and most applications need nothing more than this.
struct AlwaysAttached {
    [[nodiscard]] static constexpr bool operator()() noexcept { return true; }
};

}   // namespace uc_log
