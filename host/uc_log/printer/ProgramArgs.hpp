#pragma once

// The command line of the window's demo and bench programs (uc_log_gui_test, uc_log_rtxui_bench): `--<name> <number>`
// and `--<name>` flags from the program's own lists, every other word positional. The printer's own command line is
// Options.hpp (cxxopts, in one TU).

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uc_log::printer {

struct ProgramArgs {
    std::vector<std::pair<std::string, std::uint64_t>> numbers;   // in the order given
    std::vector<std::string>                           flags;
    std::vector<std::string>                           positional;

    /// The number given for `name` (the last, if it was given twice); nullopt: not given.
    [[nodiscard]] std::optional<std::uint64_t> number(std::string_view name) const {
        for(auto const& [given, value] : numbers | std::views::reverse) {
            if(given == name) { return value; }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool flag(std::string_view name) const {
        return std::ranges::find(flags, name) != flags.end();
    }
};

/// argc/argv as main gets them (word 0, the program, skipped). The error to say: an option neither list names, or a
/// number missing or not an unsigned decimal.
inline std::expected<ProgramArgs,
                     std::string>
parseProgramArgs(int                               argc,
                 char**                            argv,
                 std::span<std::string_view const> numberNames,
                 std::span<std::string_view const> flagNames) {
    auto const  wordAt = [&](int i) { return std::string_view{*std::next(argv, i)}; };
    ProgramArgs out;
    for(int i = 1; i < argc; ++i) {
        auto const word = wordAt(i);
        if(!word.starts_with("--")) {
            out.positional.emplace_back(word);
            continue;
        }
        auto const name = word.substr(2);
        if(std::ranges::find(flagNames, name) != flagNames.end()) {
            out.flags.emplace_back(name);
            continue;
        }
        if(std::ranges::find(numberNames, name) == numberNames.end()) {
            return std::unexpected{"unknown option " + std::string{word}};
        }
        if(i + 1 == argc) { return std::unexpected{std::string{word} + " needs a number"}; }
        auto const        text  = wordAt(++i);
        auto const* const first = std::to_address(text.begin());
        auto const* const last  = std::to_address(text.end());
        std::uint64_t     value{};
        auto const [end, ec] = std::from_chars(first, last, value);
        if(text.empty() || ec != std::errc{} || end != last) {
            return std::unexpected{std::string{word} + " takes an unsigned number, not \""
                                   + std::string{text} + "\""};
        }
        out.numbers.emplace_back(name, value);
    }
    return out;
}

}   // namespace uc_log::printer
