#pragma once

// An image's string catalog, `<target>_string_constants.json`, as the build writes it: its format strings
// ("StringConstants") before the link, its call sites ("Sites") and whether the image checks its frames ("FrameCheck",
// remote_fmt's marker symbol read by extract_sites.py) after it. The printer reads it once per session.

#include "remote_fmt/parser.hpp"
#include "uc_log/detail/SignatureTable.hpp"

#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace uc_log::detail {

struct Catalog {
    std::string                                    name;   // the file it came from, for messages
    std::unordered_map<std::uint16_t, std::string> strings;
    SignatureTable                                 signatures;
    // required: the image checks its frames (a CRC-8 per frame, start 0x56) and an unchecked one is refused; none: an
    // image without the check (or built before it: no key) - a checked frame is refused; either: nothing known
    remote_fmt::FrameCheck frameCheck{remote_fmt::FrameCheck::either};
    // false: the build has not written its call sites yet (read between its two writes: a build in progress)
    bool hasSites{};
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
    Catalog catalog{};
    catalog.name = file.string();
    try {
        auto const data = nlohmann::json::parse(text);
        catalog.strings
          = data.at("StringConstants").get<std::unordered_map<std::uint16_t, std::string>>();
        catalog.hasSites   = data.contains("Sites");
        bool const checked = data.contains("FrameCheck") && data["FrameCheck"].is_boolean()
                          && data["FrameCheck"].get<bool>();
        catalog.frameCheck
          = checked ? remote_fmt::FrameCheck::required : remote_fmt::FrameCheck::none;
    } catch(std::exception const& e) {
        return std::unexpected{fmt::format("the string catalog {:?}: {}", file.string(), e.what())};
    }
    if(!catalog.hasSites) { return catalog; }
    return SignatureTable::fromJson(text)
      .transform([&catalog](SignatureTable&& table) {
          catalog.signatures = std::move(table);
          return std::move(catalog);
      })
      .transform_error([&file](std::string const& error) {
          return fmt::format("the string catalog {:?}: {}", file.string(), error);
      });
}

}   // namespace uc_log::detail
