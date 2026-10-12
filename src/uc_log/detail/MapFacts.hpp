#pragma once

// What the printer takes from the image's linker map (--map_file): the RTT control block (its address and how many
// buffers it has), the announced-reset block, the RTT epoch and the loss ledger. Three map shapes: lld, GNU ld, and
// GNU ld with LTO (the symbol localised, only its input-section line left).

#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/detail/AnnouncedReset.hpp"
#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/detail/TargetLoss.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::detail {

inline std::expected<RttBlockInfo,
                     std::string>
controlBlockFromMap(std::string_view             content,
                    std::filesystem::path const& mapFile) {
    static constexpr std::string_view needle{"::rttControlBlock"};
    static constexpr std::uint32_t    controlBlockHeaderSize{24};   // 16 ID + 4 numUp + 4 numDown
    static constexpr std::uint32_t    bufferControlBlockSize{24};   // per RTT spec

    // Three map-file shapes: lld puts "<vma> <lma> <size> <align> <name>" on one symbol line;
    // GNU ld puts only the address on the symbol line, with the size on the preceding
    // input-section line; GNU ld with LTO localises the symbol, leaving only the input-section
    // line with the mangled name, followed by address and size.
    auto lines = content | std::views::split('\n')
               | std::views::transform([](auto&& rng) { return std::string_view{rng}; });

    auto const parseHex = [](std::string_view sv, std::uint32_t& out) -> std::string_view {
        auto const begin = std::find_if_not(sv.begin(), sv.end(), [](char c) { return c == ' '; });
        std::string_view rest{begin, sv.end()};
        if(rest.starts_with("0x")) { rest.remove_prefix(2); }
        auto const* end = std::to_address(rest.end());
        auto [ptr, ec]  = std::from_chars(rest.data(), end, out, 16);
        if(ec != std::errc{}) { return {}; }
        return std::string_view{ptr, end};
    };
    auto const plausible = [&](std::uint32_t size) {
        return size >= controlBlockHeaderSize
            && (size - controlBlockHeaderSize) % bufferControlBlockSize == 0;
    };

    static constexpr std::string_view mangledNeedle{"rttControlBlockE"};

    std::vector<std::string_view> all{lines.begin(), lines.end()};
    std::vector<std::string_view> candidates{};
    for(std::size_t i = 0; i < all.size(); ++i) {
        auto const    line = all[i];
        std::uint32_t probe{};
        // lld's input-section line also contains the mangled name but always starts with an
        // address; the GNU ld section line never does. Reading it as the GNU ld shape would
        // mistake the LMA for the size.
        bool const startsWithAddress = !parseHex(line, probe).empty();
        bool const demangled         = line.contains(needle);
        bool const section = !demangled && !startsWithAddress && line.contains(mangledNeedle);
        if(!demangled && !section) { continue; }
        candidates.push_back(line);

        std::uint32_t address{};
        std::uint32_t size{};
        if(section) {
            // "<name> <addr> <size> <file>" on one line, or the numbers on the next
            auto const nameEnd = line.find(mangledNeedle) + mangledNeedle.size();
            auto       rest    = parseHex(line.substr(nameEnd), address);
            if(rest.empty() && i + 1 < all.size()) { rest = parseHex(all[i + 1], address); }
            if(!rest.empty() && !parseHex(rest, size).empty() && plausible(size)) {
                return RttBlockInfo{address,
                                    (size - controlBlockHeaderSize) / bufferControlBlockSize};
            }
            continue;
        }
        auto rest = parseHex(line, address);
        if(rest.empty()) { continue; }
        // lld: two more hex columns, the third is the size
        std::uint32_t lma{};
        auto          r2 = parseHex(rest, lma);
        if(!r2.empty() && !parseHex(r2, size).empty() && plausible(size)) {
            return RttBlockInfo{address, (size - controlBlockHeaderSize) / bufferControlBlockSize};
        }
        // GNU ld without LTO: "<addr> <size> <file>" on the line before, same address
        std::uint32_t prevAddress{};
        if(i > 0) {
            auto p1 = parseHex(all[i - 1], prevAddress);
            if(!p1.empty() && prevAddress == address && !parseHex(p1, size).empty()
               && plausible(size))
            {
                return RttBlockInfo{address,
                                    (size - controlBlockHeaderSize) / bufferControlBlockSize};
            }
        }
    }
    auto addressLines = candidates;

    return std::unexpected(fmt::format("failed to parse address from file: {:?} lines: {::?}",
                                       mapFile.string(),
                                       addressLines));
}

// The map's facts, read once per change of the file (a rebuild under a running printer), not once per question.
// From any thread.
class MapFacts {
public:
    struct Facts {
        std::expected<RttBlockInfo, std::string> block{
          std::unexpected{std::string{"no map read yet"}}};
        std::optional<std::uint32_t> announce;
        std::optional<std::uint32_t> epoch;
        std::optional<std::uint32_t>
                                   loggedBoots;   // the count of boots that logged, after the epoch
        std::optional<LedgerPlace> ledger;
        std::size_t ledgers{};   // ::lossLedger symbols found (more than one: none used)
    };

    explicit MapFacts(std::filesystem::path file) : file_{std::move(file)} {}

    std::shared_ptr<Facts const> get() {
        std::error_code                   ec;
        auto const                        time = std::filesystem::last_write_time(file_, ec);
        std::lock_guard<std::mutex> const lock{mutex_};
        if(facts_ && !ec && time == time_) { return facts_; }
        time_  = ec ? std::filesystem::file_time_type{} : time;
        facts_ = std::make_shared<Facts const>(read());
        return facts_;
    }

private:
    Facts read() const {
        Facts         f;
        std::ifstream in{file_, std::ios::binary};
        if(!in) {
            f.block = std::unexpected{fmt::format("failed to open map file: {:?}", file_.string())};
            return f;
        }
        std::string const content{std::istreambuf_iterator<char>{in}, {}};
        f.block            = controlBlockFromMap(content, file_);
        auto const address = [&content](std::string_view symbol) -> std::optional<std::uint32_t> {
            std::istringstream map{content};
            auto const         a = announced_reset::addressFromMap(map, symbol);
            return a ? std::optional<std::uint32_t>{*a} : std::nullopt;
        };
        f.announce = address(announced_reset::Symbol);
        f.epoch    = address(RttEpochSymbol);
        f.loggedBoots
          = f.epoch.transform([](std::uint32_t epoch) { return epoch + LoggedBootsOffset; });
        std::istringstream map{content};
        f.ledger = findLossLedger(map, f.ledgers);
        return f;
    }

    std::filesystem::path           file_;
    std::mutex                      mutex_;
    std::filesystem::file_time_type time_{};
    std::shared_ptr<Facts const>    facts_;
};

}   // namespace uc_log::detail
