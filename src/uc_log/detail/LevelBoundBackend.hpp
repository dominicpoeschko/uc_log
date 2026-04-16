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
};

template<typename Tag, LogLevel Level>
using ResolveBackend = LevelBoundBackend<ComBackend<Tag>, Level>;

}   // namespace uc_log::detail
