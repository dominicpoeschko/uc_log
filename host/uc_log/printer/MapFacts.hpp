#pragma once

// What the printer takes from the image's linker map (--map_file): the RTT control block (its address and how many
// buffers it has), the announced-reset block, the RTT epoch and the loss ledger, all looked up in one MapSymbols
// parse, which `Facts::map` hands on (TargetInspector, the RAM image start). Three map shapes: lld, GNU ld, and GNU
// ld with LTO (the symbol localised, only its input-section line left).

#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/printer/AnnouncedReset.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/RttBlockInfo.hpp"
#include "uc_log/printer/TargetLoss.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer {

/// The RTT control block from the map: the first `...::rttControlBlock` symbol (lld; GNU ld with the size from the
/// input-section line before it) or, GNU ld with LTO, the first input section `.bss._ZN...rttControlBlockE`, whose
/// size is 24 header bytes plus 24 per buffer.
inline std::expected<RttBlockInfo,
                     std::string>
rttBlockOf(MapSymbols const&            map,
           std::filesystem::path const& mapFile) {
    static constexpr std::string_view needle{"::rttControlBlock"};
    static constexpr std::string_view mangledNeedle{"rttControlBlockE"};
    static constexpr std::uint32_t    controlBlockHeaderSize{24};   // 16 ID + 4 numUp + 4 numDown
    static constexpr std::uint32_t    bufferControlBlockSize{24};   // per RTT spec

    auto const plausible = [&](std::uint32_t size) {
        return size >= controlBlockHeaderSize
            && (size - controlBlockHeaderSize) % bufferControlBlockSize == 0;
    };
    std::vector<std::string> candidates;
    for(auto const& e : map.entries()) {
        if(!e.name.contains(e.fromSection ? mangledNeedle : needle)) { continue; }
        candidates.push_back(fmt::format("{} {:#x} {:#x}", e.name, e.address, e.size));
        if(plausible(e.size)) {
            return RttBlockInfo{e.address,
                                (e.size - controlBlockHeaderSize) / bufferControlBlockSize};
        }
    }
    return std::unexpected(fmt::format("failed to parse address from file: {:?} lines: {::?}",
                                       mapFile.string(),
                                       candidates));
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
        // the parse the facts came from, never null: the inspector's symbols, the RAM image's vector table
        std::shared_ptr<MapSnapshot const> map{std::make_shared<MapSnapshot const>()};
        std::uint64_t                      generation{};   // the parses before this one
    };

    explicit MapFacts(std::filesystem::path file) : file_{std::move(file)} {}

    std::shared_ptr<Facts const> get() {
        std::error_code                    ec;
        auto const                         time = std::filesystem::last_write_time(file_, ec);
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(facts_ && !ec && time == time_) { return facts_; }
        time_            = ec ? std::filesystem::file_time_type{} : time;
        auto facts       = read();
        facts.generation = generation_++;
        facts_           = std::make_shared<Facts const>(std::move(facts));
        return facts_;
    }

private:
    [[nodiscard]] Facts read() const {
        Facts f;
        auto  snapshot = std::make_shared<MapSnapshot const>(MapSnapshot::load(file_));
        if(!snapshot->error.empty()) {
            f.block = std::unexpected{fmt::format("failed to open map file: {:?}", file_.string())};
        } else {
            auto const& map = snapshot->symbols;
            f.block         = rttBlockOf(map, file_);
            f.announce      = map.addressOf(announced_reset::symbol);
            f.epoch         = map.addressOf(uc_log::detail::rttEpochSymbol);
            f.loggedBoots   = f.epoch.transform(
              [](std::uint32_t epoch) { return epoch + uc_log::detail::loggedBootsOffset; });
            f.ledger = findLossLedger(map, f.ledgers);
        }
        f.map = std::move(snapshot);
        return f;
    }

    std::filesystem::path           file_;
    std::mutex                      mutex_;
    std::filesystem::file_time_type time_{};
    std::shared_ptr<Facts const>    facts_;
    std::uint64_t                   generation_{};
};

}   // namespace uc_log::printer
