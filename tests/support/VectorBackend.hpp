#pragma once

// A remote_fmt::Printer backend that keeps every byte written: the frames a firmware's Printer would put into its
// ring, for the tests and fuzzers that build streams on the host.

#include <cstddef>
#include <span>
#include <vector>

namespace uc_log::test {

struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

}   // namespace uc_log::test
