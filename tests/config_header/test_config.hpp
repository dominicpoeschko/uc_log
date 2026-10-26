#pragma once
// The config header of uc_log_test_config_header (UC_LOG_CONFIG_HEADER, set by tests/CMakeLists.txt): what an image's
// kvasir_config.hpp holds - the log sink and the log clock, both complete - and nothing that includes uc_log.hpp.
#include "uc_log/ComBackend.hpp"
#include "uc_log/LogClock.hpp"
#include "uc_log/Tag.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace config_header_test {
// Leaked on purpose: avoids global-constructor / exit-time-destructor warnings.
inline std::vector<std::byte>& written() {
    static auto& v = *new std::vector<std::byte>{};
    return v;
}

inline int& clockReads() {
    static int n{};
    return n;
}
}   // namespace config_header_test

namespace uc_log {
template<>
struct ComBackend<Tag::User> {
    static constexpr bool fromConfigHeader = true;

    static void write(std::span<std::byte const> bytes) {
        config_header_test::written().insert(config_header_test::written().end(),
                                             bytes.begin(),
                                             bytes.end());
    }
};

template<>
struct LogClock<Tag::User> {
    static std::chrono::milliseconds now() {
        ++config_header_test::clockReads();
        return std::chrono::milliseconds{7};
    }
};
}   // namespace uc_log

#ifdef UC_LOG_CONFIG_HEADER_FAIL
    // the case the hook must refuse: a config header that reaches uc_log.hpp
    #include "uc_log/uc_log.hpp"
#endif
