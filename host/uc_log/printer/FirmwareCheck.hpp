#pragma once

#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/HexImage.hpp"
#include "uc_log/printer/LogFormat.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace uc_log::printer {

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
    template<typename ReadF,
             typename EmitF>
    void run(std::string const& hexFile,
             bool               ramImage,
             ReadF&&            read,
             EmitF&&            emit) {
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
        } else {
            auto const      crc   = printer::imageCrc(*image);
            auto const      both  = printer::checkFlash(*image, read, uncachedOffset);
            auto const&     check = both.flash;
            std::error_code ec;
            auto const      written = std::filesystem::last_write_time(hexFile, ec);
            auto const      built
              = ec ? std::string{"?"}
                   : printer::logformat::toIso8601Utc(
                       std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                         std::chrono::file_clock::to_sys(written)));
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

    control::FirmwareCheck get() {
        std::scoped_lock<std::mutex> const lock{mutex};
        return summary;
    }

    // what a check found: the TUI's badge, and the text as a status message (an error when it is one)
    using EmitF = std::function<void(State, std::string const&, bool)>;

    /// At each session start.
    template<typename ReadF>
    void check(std::string const& hexFile,
               bool               ramImage,
               ReadF&&            read,
               EmitF const&       emit) {
        run(hexFile, ramImage, std::forward<ReadF>(read), emit);
        gateHexTime_.reset();
    }

    /// The decode gate: open unless the target runs another image. Checked again whenever the hex file changes (a
    /// rebuild that matches what runs), and every gateRecheck while "different": a flash read can be wrong for a moment
    /// (a chip whose boot stage is still setting up the flash interface), and a gate closed on such a read would hold
    /// the log for good. A cached view that stays wrong is a cache line filled during a reset of one core
    /// (--flash_uncached_offset).
    template<typename ReadF>
    bool gate(std::string const& hexFile,
              bool               ramImage,
              ReadF&&            read,
              EmitF const&       emit) {
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
            run(hexFile, ramImage, std::forward<ReadF>(read), emit);
        }
        return get().state != State::different;
    }

private:
    // the gate's own (the reader thread's): the hex file's time at the last look, and when it last checked
    std::optional<std::filesystem::file_time_type> gateHexTime_;
    Clock::time_point                              gateLastCheck_{};
};

}   // namespace uc_log::printer
