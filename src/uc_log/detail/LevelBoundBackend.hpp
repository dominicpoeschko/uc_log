#pragma once

#include "../ComBackend.hpp"
#include "../LogLevel.hpp"

#include <cstddef>
#include <span>

namespace uc_log::detail {

template<typename Backend, LogLevel Level>
struct LevelBoundBackend {
    static void write(std::span<std::byte const> span) {
        if constexpr(requires { Backend::template write<Level>(span); }) {
            Backend::template write<Level>(span);
        } else {
            Backend::write(span);
        }
    }

    // remote_fmt::Printer brackets one entry with these; forwarded so a backend that
    // assembles whole entries (a queue with a drop-whole-entry policy) can tell where
    // one starts and ends. Optional on the backend, level-templated when it wants it.
    static void initTransfer() {
        if constexpr(requires { Backend::template initTransfer<Level>(); }) {
            Backend::template initTransfer<Level>();
        } else if constexpr(requires { Backend::initTransfer(); }) {
            Backend::initTransfer();
        }
    }

    static void finalizeTransfer() {
        if constexpr(requires { Backend::template finalizeTransfer<Level>(); }) {
            Backend::template finalizeTransfer<Level>();
        } else if constexpr(requires { Backend::finalizeTransfer(); }) {
            Backend::finalizeTransfer();
        }
    }
};

template<typename Tag, LogLevel Level>
using ResolveBackend = LevelBoundBackend<ComBackend<Tag>, Level>;

}   // namespace uc_log::detail
