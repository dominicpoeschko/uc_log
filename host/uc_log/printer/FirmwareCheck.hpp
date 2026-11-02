#pragma once

#include "uc_log/detail/BuildId.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/LogFormat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace uc_log::printer {

/// Where an image keeps its build id (detail/BuildId.hpp), from the map: the boot words with the copy, the note.
struct BuildIdPlace {
    std::uint32_t bootWords;
    std::uint32_t note;
};

/// The 8 bytes of the build id the hex file's note holds; nullopt: no build-id note there.
inline std::optional<std::array<std::byte,
                                uc_log::Detail::buildIdBytes>>
buildIdOf(std::span<HexSegment const> image,
          std::uint32_t               note) {
    constexpr std::uint32_t need = uc_log::Detail::buildIdHashAt + uc_log::Detail::buildIdBytes;
    for(auto const& seg : image) {
        if(note < seg.address
           || std::uint64_t{note} + need > seg.address + std::uint64_t{seg.data.size()})
        {
            continue;
        }
        auto const at   = std::span{seg.data}.subspan(note - seg.address, need);
        auto const word = [&](std::size_t i) { return le32(at, i * 4); };
        if(!uc_log::Detail::isBuildIdNote(word(0), word(1), word(2), word(3))) {
            return std::nullopt;
        }
        std::array<std::byte, uc_log::Detail::buildIdBytes> id{};
        std::ranges::copy(at.subspan(uc_log::Detail::buildIdHashAt), id.begin());
        return id;
    }
    return std::nullopt;
}

// Does the board run the build whose strings decode the log? Checked at each session start.
struct FirmwareCheck {
    using Clock = printer::Clock;
    using State = control::FirmwareState;
    std::mutex             mutex;
    control::FirmwareCheck summary{};
    /// --flash_uncached_offset: the image is compared through the flash's uncached alias (address + offset), so the
    /// verdict is about the flash, not a cache line; the cached view is compared too, and a line that differs from
    /// flash is said as an error of its own (the target runs code that is not in its flash).
    std::uint32_t uncachedOffset{};

    /// `ramImage`: the image lives in RAM, which changes as it runs - nothing to compare, and
    /// that is no error (emit's third argument says whether it is one).
    /// `buildId`: where the image keeps its build id - the RAM copy equal to the hex file's note is a match without a
    /// look at the flash (detail/BuildId.hpp); anything else compares the flash.
    template<typename ReadF,
             typename EmitF>
    void run(std::string const&                 hexFile,
             bool                               ramImage,
             ReadF&&                            read,
             EmitF&&                            emit,
             std::optional<BuildIdPlace> const& buildId) {
        using printer::ImageCheck;
        std::string            text;
        bool                   isError = true;
        control::FirmwareCheck state{};
        std::ifstream          file{hexFile};
        auto const image = file ? printer::parseIntelHex(file)
                                : std::unexpected{fmt::format("cannot open {:?}", hexFile)};
        if(!image) {
            text = fmt::format("firmware not checked: {}", image.error());
        } else if(ramImage) {
            auto const crc = printer::imageCrc(*image);
            state          = {.state = State::unchecked, .build = crc};
            isError        = false;
            text           = fmt::format(
              "firmware not checked: {} is a RAM image (build {:08x}), and RAM "
              "changes as it runs",
              hexFile,
              crc);
        } else if(auto const id = runningBuildId(*image, read, buildId)) {
            auto const crc = printer::imageCrc(*image);
            state          = {.state = State::match, .build = crc};
            isError        = false;
            text
              = fmt::format("firmware matches {} (build {:08x}, build id {:02x} in RAM, built {})",
                            hexFile,
                            crc,
                            fmt::join(*id, ""),
                            builtAt(hexFile));
        } else {
            auto const  crc   = printer::imageCrc(*image);
            auto const  both  = printer::checkFlash(*image, read, uncachedOffset);
            auto const& check = both.flash;
            auto const  built = builtAt(hexFile);
            if(both.cacheDiffers()) {
                state = {.state = State::match, .build = crc};
                text  = fmt::format(
                  "⚠ the flash holds {} (build {:08x}), but the target's flash cache does not: "
                  "first difference at "
                  "{:#010x} - the target runs code that is not in its flash (a cache line filled "
                  "during a reset of "
                  "one core); a reset of the cache or a flush clears it",
                  hexFile,
                  crc,
                  both.cached.firstDifference);
            } else if(check.result == ImageCheck::Result::match) {
                state   = {.state = State::match, .build = crc};
                isError = false;
                text
                  = fmt::format("firmware matches {} (build {:08x}, {} bytes compared, built {})",
                                hexFile,
                                crc,
                                check.comparedBytes,
                                built);
            } else if(check.result == ImageCheck::Result::different) {
                state = {.state = State::different, .build = crc};
                text  = fmt::format(
                  "⚠ the target does NOT run {} (build {:08x}, built {}): first "
                  "difference at {:#010x} - this log is decoded with the wrong "
                  "strings; flash the target",
                  hexFile,
                  crc,
                  built,
                  check.firstDifference);
            } else {
                state = {.state = State::unchecked, .build = crc};
                text  = fmt::format("firmware not checked: {}", check.error);
            }
        }
        {
            std::scoped_lock<std::mutex> const lock{mutex};
            summary = state;
        }
        emit(state.state, text, isError);
    }

    // the hex file's time, as the messages say it
    static std::string builtAt(std::string const& hexFile) {
        std::error_code ec;
        auto const      written = std::filesystem::last_write_time(hexFile, ec);
        return ec ? std::string{"?"}
                  : printer::logformat::toIso8601Utc(
                      std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        std::chrono::file_clock::to_sys(written)));
    }

    // The build id the running image put into RAM, when it is the hex file's; nullopt: not known to be (no place, no
    // note in the hex file, a read that failed, an epoch odd - a boot in its init -, another or no id). The copy counts
    // only XOR the epoch it was written for (detail/BuildId.hpp): one an earlier boot or another image left does not.
    template<typename ReadF>
    static std::optional<std::array<std::byte,
                                    uc_log::Detail::buildIdBytes>>
    runningBuildId(std::span<HexSegment const>        image,
                   ReadF&                             read,
                   std::optional<BuildIdPlace> const& buildId) {
        if(!buildId) { return std::nullopt; }
        auto const expected = buildIdOf(image, buildId->note);
        if(!expected) { return std::nullopt; }
        std::array<std::byte, uc_log::Detail::buildIdWordsOffset + uc_log::Detail::buildIdBytes>
          words{};
        if(!read(buildId->bootWords, std::span{words})) { return std::nullopt; }
        auto const epoch = le32(words, 0);
        if(!uc_log::Detail::epochBlockIsReady(epoch)) { return std::nullopt; }
        for(std::uint32_t i = 0; i != uc_log::Detail::buildIdBytes; i += 4) {
            if((le32(words, uc_log::Detail::buildIdWordsOffset + i) ^ epoch) != le32(*expected, i))
            {
                return std::nullopt;
            }
        }
        return expected;
    }

    control::FirmwareCheck get() {
        std::scoped_lock<std::mutex> const lock{mutex};
        return summary;
    }

    // what a check found: the TUI's badge, and the text as a status message (an error when it is one)
    using EmitF = std::function<void(State, std::string const&, bool)>;

    /// At each session start.
    template<typename ReadF>
    void check(std::string const&                 hexFile,
               bool                               ramImage,
               ReadF&&                            read,
               EmitF const&                       emit,
               std::optional<BuildIdPlace> const& buildId = std::nullopt) {
        run(hexFile, ramImage, std::forward<ReadF>(read), emit, buildId);
        gateHexTime_.reset();
    }

    /// The decode gate: open unless the target runs another image. Checked again whenever the hex file changes (a
    /// rebuild that matches what runs), and every gateRecheck while "different": a flash read can be wrong for a moment
    /// (a chip whose boot stage is still setting up the flash interface), and a gate closed on such a read would hold
    /// the log for good. A cached view that stays wrong is a cache line filled during a reset of one core
    /// (--flash_uncached_offset).
    template<typename ReadF>
    bool gate(std::string const&                 hexFile,
              bool                               ramImage,
              ReadF&&                            read,
              EmitF const&                       emit,
              std::optional<BuildIdPlace> const& buildId = std::nullopt) {
        static constexpr auto gateRecheck = std::chrono::seconds{2};
        auto const            now         = Clock::now();
        std::error_code       ec;
        auto const            written = std::filesystem::last_write_time(hexFile, ec);
        bool const            first   = !gateHexTime_;
        bool const            changed = !ec && (first || *gateHexTime_ != written);
        bool const due = get().state == State::different && now >= gateLastCheck_ + gateRecheck;
        if(!ec) { gateHexTime_ = written; }
        if(first) { gateLastCheck_ = now; }
        if(!first && (changed || due)) {
            gateLastCheck_ = now;
            run(hexFile, ramImage, std::forward<ReadF>(read), emit, buildId);
        }
        return get().state != State::different;
    }

private:
    // the gate's own (the reader thread's): the hex file's time at the last look, and when it last checked
    std::optional<std::filesystem::file_time_type> gateHexTime_;
    Clock::time_point                              gateLastCheck_{};
};

}   // namespace uc_log::printer
