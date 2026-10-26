#pragma once

#include "remote_fmt/catalog.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/Signature.hpp"

#include <charconv>
#include <cstdint>
#include <expected>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace uc_log { namespace printer {

    // Catalog id of a call site -> its demangled signature ("Sites" in the catalog json), with
    // module and short name derived once per site.
    struct SignatureInfo {
        std::string module;      // empty for a global function
        std::string function;    // Class<abbreviated args>::name
        std::string signature;   // the demangled signature
    };

    class SignatureTable {
        std::unordered_map<remote_fmt::catalog_id, SignatureInfo> entries;

    public:
        void add(remote_fmt::catalog_id id,
                 std::string_view       signature) {
            auto const names = uc_log::detail::namesOfDemangled(signature);
            entries.insert_or_assign(id,
                                     SignatureInfo{std::string{names.module.view()},
                                                   std::string{names.function.view()},
                                                   std::string{signature}});
        }

        SignatureInfo const*
        find(std::optional<remote_fmt::catalog_id> id) const UC_LOG_LIFETIMEBOUND {
            if(!id) { return nullptr; }
            auto const it = entries.find(*id);
            return it == entries.end() ? nullptr : &it->second;
        }

        std::size_t size() const { return entries.size(); }

        bool empty() const { return entries.empty(); }

        // the "Sites" object as remote_fmt::CatalogFile hands it over: id (decimal) -> signature
        static std::expected<SignatureTable,
                             std::string>
        fromSites(std::map<std::string,
                           std::string> const& sites) {
            SignatureTable table;
            for(auto const& [key, signature] : sites) {
                remote_fmt::catalog_id id{};
                auto const             end = std::to_address(key.end());
                auto const [ptr, ec]       = std::from_chars(key.data(), end, id);
                if(key.empty() || ec != std::errc{} || ptr != end) {
                    return std::unexpected("a site id is not a 16-bit number: " + key);
                }
                table.add(id, signature);
            }
            return table;
        }
    };
}}   // namespace uc_log::printer
