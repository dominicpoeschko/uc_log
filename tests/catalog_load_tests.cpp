// The printer's catalog loader (printer/Catalog.hpp) on hand-written files: a whole catalog loads and its checked
// frames decode; a catalog without one of its keys ("Sites", "FrameCheck", "Types", an entry's "args") or with a type
// reference without its entry is refused at once, the key or the string named - the build writes the catalog once,
// whole, after the link, so there is no half-written one to wait for.
#include "uc_log/printer/Catalog.hpp"

#include "remote_fmt/parser.hpp"
#include "remote_fmt/wire.hpp"
#include "support/Check.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::filesystem::path write(std::string_view name,
                            std::string_view text) {
    auto const dir = std::filesystem::temp_directory_path() / "uc_log_catalog_load_tests";
    std::filesystem::create_directories(dir);
    auto const path = dir / name;
    std::ofstream{path} << text;
    return path;
}
}   // namespace

int main() {
    using uc_log::printer::loadCatalog;
    {
        auto const c = loadCatalog(write(
          "ok.json",
          R"j({"StringConstants": [{"id": 32769, "fmt": "x {}", "args": ["u4"]}], "Sites": {"32769": "void f()"}, "FrameCheck": true, "Types": {}})j"));
        CHECK(c && c->strings.at(32769) == "x {}\x1eu4", "a catalog loads");
        CHECK(c && c->frameCheck == remote_fmt::FrameCheck::required, "its frame check is read");
        // a checked frame (REMOTE_FMT_FRAME_CHECK=1, the default) of the site's string: x 7
        std::vector<std::byte> frame{remote_fmt::protocol::checkedStartMarker,
                                     std::byte{0x01},
                                     std::byte{0x80},
                                     std::byte{7}};
        frame.push_back(static_cast<std::byte>(remote_fmt::protocol::crcFinal(
          remote_fmt::protocol::crcOver(remote_fmt::protocol::crcInit, std::span{frame}))));
        frame.push_back(remote_fmt::protocol::endMarker);
        std::string refused;
        auto const  parsed = c ? remote_fmt::parseMessage(
                                   std::span<std::byte const>{frame},
                                   c->strings,
                                   [&refused](std::string_view error) { refused = error; },
                                   c->frameCheck)
                               : remote_fmt::ParsedMessage{};
        CHECK(parsed.message == std::optional<std::string>{"x 7"} && refused.empty(),
              "and its checked frame decodes");
    }

    // a key missing: refused at once as malformed, the key named (a session that starts while the tree links reads
    // the previous, whole catalog)
    struct Missing {
        std::string_view key;
        std::string_view text;
    };

    for(
      auto const& [key, text] :
      std::array{
        Missing{
                "Sites",R"j({"StringConstants": [{"id": 5, "fmt": "a", "args": []}], "FrameCheck": true, "Types": {}})j"                             },
        Missing{"FrameCheck",
                R"j({"StringConstants": [], "Sites": {"32769": "void f()"}, "Types": {}})j"                                    },
        Missing{     "Types",                                   R"j({"StringConstants": [], "Sites": {}, "FrameCheck": true})j"},
        Missing{
                "args", R"j({"StringConstants": [{"id": 5, "fmt": "a"}], "Sites": {}, "FrameCheck": true, "Types": {}})j"      },
    })
    {
        auto const c = loadCatalog(write("missing_key.json", text));
        CHECK(!c && c.error().contains(key), "a catalog without a key: refused, the key named");
    }
    {
        auto const c = loadCatalog(write(
          "types.json",
          R"j({"StringConstants": [{"id": 5, "fmt": "{}", "args": ["#00000000000000aa"]}], "Sites": {}, "FrameCheck": true, "Types": {"00000000000000aa": "Eu11:1,1:a"}})j"));
        CHECK(c && c->strings.at(5) == "{}\x1e" "Eu11:1,1:a", "a type reference is resolved");
        auto const missing = loadCatalog(write(
          "missing.json",
          R"j({"StringConstants": [{"id": 5, "fmt": "{}", "args": ["#00000000000000bb"]}], "Sites": {}, "FrameCheck": true, "Types": {}})j"));
        CHECK(!missing && missing.error().contains("type entry")
                && missing.error().contains("string 5"),
              "a reference without its entry is refused, the string named");
    }
    return uc_log::test::report();
}
