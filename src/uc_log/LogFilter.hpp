#pragma once

#include "LogLevel.hpp"
#include "detail/Signature.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The log filter file, read at compile time: call sites below its floor expand to nothing, no
// code and no catalog string.
//
//   # uc_log_filter.txt
//   * = info          # every line: the global floor
//   i2c = warn        # i2c and everything below it (i2c.bus, i2c.device ...)
//   usb.cdc = off     # not one line
//
// Levels: trace debug info warn error crit off. The longest matching module wins and replaces
// the scope's UC_LOG_SCOPE_MIN_LEVEL; nothing goes below `*` or UC_LOG_MIN_LEVEL. Found via
// `--embed-dir=`; without one nothing is filtered. Only module rules make a call site read
// __PRETTY_FUNCTION__ (Signature.hpp scanSignature).

namespace uc_log { namespace detail {
    // One above crit: no line passes.
    inline constexpr std::uint8_t LevelOff = 6;

    struct FilterRule {
        std::array<char, MaxModuleName> module{};
        std::size_t                     size{};
        std::uint8_t                    level{};

        constexpr std::string_view name() const { return {module.data(), size}; }
    };

    struct FilterTable {
        static constexpr std::size_t MaxRules = 64;

        std::array<FilterRule, MaxRules> rules{};
        std::size_t                      count{};
        std::uint8_t                     global{};   // `*`, trace without one

        constexpr bool hasModuleRules() const { return count != 0; }

        // -1 if none.
        constexpr int levelFor(std::string_view module) const {
            int         level = -1;
            std::size_t best  = 0;
            for(std::size_t i = 0; i < count; ++i) {
                auto const name   = rules[i].name();
                bool const covers = module.starts_with(name)
                                 && (module.size() == name.size() || module[name.size()] == '.');
                if(covers && name.size() > best) {
                    best  = name.size();
                    level = rules[i].level;
                }
            }
            return level;
        }
    };

    struct FilterParse {
        FilterTable      table;
        std::string_view error;    // empty when it parsed
        std::size_t      line{};   // 1-based
    };

    inline constexpr std::array<std::string_view, 7>
      FilterLevelNames{"trace", "debug", "info", "warn", "error", "crit", "off"};

    constexpr int filterLevel(std::string_view name) {
        for(std::size_t i = 0; i < FilterLevelNames.size(); ++i) {
            if(FilterLevelNames[i] == name) { return static_cast<int>(i); }
        }
        return -1;
    }

    constexpr std::string_view filterLevelName(std::uint8_t level) {
        return level < FilterLevelNames.size() ? FilterLevelNames[level] : std::string_view{"?"};
    }

    constexpr std::string_view trimmed(std::string_view s) {
        while(!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
            s.remove_prefix(1);
        }
        while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return s;
    }

    constexpr FilterParse parseFilter(std::string_view text) {
        FilterParse result{};
        std::size_t lineNumber = 0;
        bool        sawGlobal  = false;
        while(!text.empty()) {
            ++lineNumber;
            auto const end  = text.find('\n');
            auto       line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if(auto const hash = line.find('#'); hash != std::string_view::npos) {
                line = line.substr(0, hash);
            }
            line = trimmed(line);
            if(line.empty()) { continue; }
            auto fail = [&](std::string_view message) {
                result.error = message;
                result.line  = lineNumber;
                return result;
            };
            auto const eq = line.find('=');
            if(eq == std::string_view::npos) { return fail("a rule is `module = level`"); }
            auto const module = trimmed(line.substr(0, eq));
            auto const level  = filterLevel(trimmed(line.substr(eq + 1)));
            if(level < 0) { return fail("the level is trace debug info warn error crit or off"); }
            if(module == "*") {
                if(sawGlobal) { return fail("a second `*`"); }
                sawGlobal           = true;
                result.table.global = static_cast<std::uint8_t>(level);
                continue;
            }
            if(!validModuleName(module)) {
                return fail("a module is 1-63 characters of a-z 0-9 _ . : / -");
            }
            for(std::size_t i = 0; i < result.table.count; ++i) {
                if(result.table.rules[i].name() == module) { return fail("a module named twice"); }
            }
            if(result.table.count == FilterTable::MaxRules) { return fail("more than 64 rules"); }
            auto& rule = result.table.rules[result.table.count++];
            for(char const c : module) { rule.module[rule.size++] = c; }
            rule.level = static_cast<std::uint8_t>(level);
        }
        return result;
    }

    // never defined: naming it in a constant expression turns a bad file into a compile error
    void logFilterFileIsInvalid(char const* message);

    consteval FilterTable checkedFilter(std::string_view text) {
        auto const parsed = parseFilter(text);
        if(!parsed.error.empty()) { logFilterFileIsInvalid(parsed.error.data()); }
        return parsed.table;
    }
}}   // namespace uc_log::detail

#if defined(__has_embed)
    #if __has_embed(<uc_log_filter.txt>)
        #define UC_LOG_HAS_FILTER_FILE 1
    #endif
#endif

namespace uc_log { namespace detail {
#ifdef UC_LOG_HAS_FILTER_FILE
    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wc23-extensions"
    #endif
    // clang-format would add a space after `<`
    // clang-format off
    inline constexpr unsigned char filterFileBytes[] = {
#embed <uc_log_filter.txt> suffix(, )
      0};
    // clang-format on
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif

    // the text is a local copy, not a namespace-scope array: gcc with -fsanitize=null (part of
    // undefined) cannot fold `pointer into a static != nullptr`, which string_view::find tests
    consteval FilterTable filterFromFile() {
        std::array<char, sizeof(filterFileBytes) - 1> text{};   // not the final 0
        std::ranges::transform(std::span{filterFileBytes}.first(text.size()),
                               text.begin(),
                               [](unsigned char byte) { return static_cast<char>(byte); });
        return checkedFilter(std::string_view{text.data(), text.size()});
    }

    inline constexpr FilterTable filterTable = filterFromFile();
#else
    inline constexpr FilterTable filterTable{};
#endif
}}   // namespace uc_log::detail
