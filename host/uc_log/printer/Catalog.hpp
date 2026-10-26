#pragma once

// An image's string catalog, `<target>_string_constants.json`, as the build writes it, once and whole, after the link
// (remote_fmt's extract_sites.py): its format strings ("StringConstants", one {"id", "fmt", "args"} entry each), its
// call sites ("Sites"), whether the image checks its frames ("FrameCheck") and its type entries ("Types"). The printer
// reads it once per session, through remote_fmt's one catalog reader (catalog_helpers.hpp): one parse, the signature
// table built from its "Sites". A session that starts while the tree links reads the previous catalog, which belongs to
// the image still on the board.

#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/wire.hpp"
#include "uc_log/printer/SignatureTable.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace uc_log::printer {

struct Catalog {
    std::string                                    name;   // the file it came from, for messages
    std::unordered_map<std::uint16_t, std::string> strings;
    SignatureTable                                 signatures;
    // required: the image checks its frames (a CRC-8 per frame, start 0x57) and an unchecked one is refused; none: an
    // image without the check - a checked frame is refused; either: no catalog loaded, both decode
    remote_fmt::FrameCheck frameCheck{remote_fmt::FrameCheck::either};
};

inline std::expected<Catalog,
                     std::string>
loadCatalog(std::filesystem::path const& file) {
    std::string text;
    {
        std::ifstream in{file, std::ios::binary};
        if(!in) {
            return std::unexpected{
              fmt::format("cannot open the string catalog {:?}", file.string())};
        }
        text.assign(std::istreambuf_iterator<char>{in}, {});
    }
    auto parsed = remote_fmt::parseCatalogFile(std::move(text));
    if(!parsed) {
        return std::unexpected{
          fmt::format("the string catalog {:?}: {}", file.string(), parsed.error())};
    }
    Catalog catalog{};
    catalog.name       = file.string();
    catalog.strings    = std::move(parsed->strings);
    catalog.frameCheck = parsed->frameCheck;
    return SignatureTable::fromSites(parsed->sites)
      .transform([&catalog](SignatureTable&& table) {
          catalog.signatures = std::move(table);
          return std::move(catalog);
      })
      .transform_error([&file](std::string const& error) {
          return fmt::format("the string catalog {:?}: {}", file.string(), error);
      });
}

}   // namespace uc_log::printer
