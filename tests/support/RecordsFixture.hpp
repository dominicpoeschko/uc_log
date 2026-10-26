#pragma once
// The fixture target of tests/data/records (its README says what it holds): one image's flash and RAM bytes with
// every record kind, the core registers of a halt, and its maps. A test that needs the directory defines
// UC_LOG_RECORDS_FIXTURE (CMakeLists.txt does, for the tests that read it).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc_log::test {

struct RecordsFixture {
    struct Region {
        std::uint32_t address{};
        std::string   what;
        std::string   hex;
    };

    struct Code {
        std::uint32_t start{};
        std::uint32_t end{};
    };

    std::string                about;
    Code                       code;
    std::vector<std::uint32_t> registers;
    std::vector<Region>        memory;

    /// `size` bytes from `address`, 0 where no region holds them.
    [[nodiscard]] std::vector<std::byte> read(std::uint32_t address,
                                              std::size_t   size) const {
        std::vector<std::byte> out(size);
        for(auto const& r : memory) {
            auto const bytes = r.hex.size() / 2;
            auto const lo    = std::max<std::uint64_t>(r.address, address);
            auto const hi    = std::min<std::uint64_t>(std::uint64_t{r.address} + bytes,
                                                       std::uint64_t{address} + size);
            for(auto a = lo; a < hi; ++a) {
                auto const at    = 2 * (a - r.address);
                out[a - address] = static_cast<std::byte>(
                  std::stoul(r.hex.substr(static_cast<std::size_t>(at), 2), nullptr, 16));
            }
        }
        return out;
    }

    [[nodiscard]] static std::filesystem::path dir() { return UC_LOG_RECORDS_FIXTURE; }

    [[nodiscard]] static std::string text(std::string const& name) {
        std::ifstream      in{dir() / name, std::ios::binary};
        std::ostringstream s;
        s << in.rdbuf();
        if(!in) { throw std::runtime_error{"cannot read " + (dir() / name).string()}; }
        return s.str();
    }

    [[nodiscard]] static RecordsFixture load() {
        RecordsFixture fixture;
        if(auto const ec = glz::read_json(fixture, text("fixture.json")); ec) {
            throw std::runtime_error{"fixture.json: " + glz::format_error(ec)};
        }
        return fixture;
    }
};

}   // namespace uc_log::test
