#pragma once

// The Filter tab's filter as a JSON file: saved pretty, loaded whole (a file that does not parse changes nothing).

#include "uc_log/printer/GlazeCodecs.hpp"
#include "uc_log/printer/LogFilterState.hpp"

#include <expected>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace uc_log::printer {

/// Nothing, or what went wrong.
inline std::expected<void,
                     std::string>
saveFilter(std::string const& path,
           FilterState const& filter) {
    std::string buffer{};
    if(auto err = glz::write_json(filter, buffer); err) {
        return std::unexpected{"Error serializing: " + glz::format_error(err, buffer)};
    }
    auto const    pretty = glz::prettify_json(buffer);
    std::ofstream out(path);
    if(!out) { return std::unexpected{fmt::format("Error: cannot open '{}' for writing", path)}; }
    out << pretty;
    if(!out) { return std::unexpected{std::string{"Error: write failed"}}; }
    return {};
}

inline std::expected<FilterState,
                     std::string>
loadFilter(std::string const& path) {
    std::ifstream in(path);
    if(!in) { return std::unexpected{fmt::format("Error: cannot open '{}'", path)}; }
    std::string const buffer(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
    FilterState       loaded{};
    if(auto err = glz::read_json(loaded, buffer); err) {
        return std::unexpected{"Error parsing: " + glz::format_error(err, buffer)};
    }
    return loaded;
}

}   // namespace uc_log::printer
