#pragma once

#include "Signature.hpp"
#include "remote_fmt/catalog.hpp"

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

namespace uc_log { namespace detail {

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
            auto const names = namesOfDemangled(signature);
            entries.insert_or_assign(id,
                                     SignatureInfo{std::string{names.module.view()},
                                                   std::string{names.function.view()},
                                                   std::string{signature}});
        }

        SignatureInfo const* find(std::optional<remote_fmt::catalog_id> id) const {
            if(!id) { return nullptr; }
            auto const it = entries.find(*id);
            return it == entries.end() ? nullptr : &it->second;
        }

        std::size_t size() const { return entries.size(); }

        bool empty() const { return entries.empty(); }

        // "Sites": {"<id>": "<signature>"} next to the string constants.
        struct Json {
            std::map<std::string, std::string> Sites;
        };

        static std::expected<SignatureTable,
                             std::string>
        fromJson(std::string const& text) {
            Json data{};
            if(auto const ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(data, text); ec)
            {
                return std::unexpected("read signatures failed: " + glz::format_error(ec, text));
            }
            SignatureTable table;
            for(auto const& [key, signature] : data.Sites) {
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

        static std::expected<SignatureTable,
                             std::string>
        fromJsonFile(std::string const& file) {
            std::ifstream stream(file);
            if(!stream) { return std::unexpected("read signatures failed: cannot open " + file); }
            return fromJson(std::string{std::istreambuf_iterator<char>{stream}, {}});
        }
    };
}}   // namespace uc_log::detail
