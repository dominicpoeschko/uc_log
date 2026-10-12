#include "jlink/JLink.hpp"

#include "remote_fmt/fmt_wrapper.hpp"
#include "remote_fmt/parser.hpp"
#include "uc_log/FTXUIGui.hpp"
#include "uc_log/JLinkRttReader.hpp"
#include "uc_log/LogLevel.hpp"
#include "uc_log/RttBlockInfo.hpp"
#include "uc_log/TimeDelayedQueue.hpp"
#include "uc_log/detail/AnnouncedReset.hpp"
#include "uc_log/detail/Catalog.hpp"
#include "uc_log/detail/ControlEvents.hpp"
#include "uc_log/detail/ControlServer.hpp"
#include "uc_log/detail/DuplexChannelServer.hpp"
#include "uc_log/detail/HexImage.hpp"
#include "uc_log/detail/Le32.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/detail/LogFormat.hpp"
#include "uc_log/detail/MapFacts.hpp"
#include "uc_log/detail/RamImage.hpp"
#include "uc_log/detail/RowFile.hpp"
#include "uc_log/detail/RttChannelMap.hpp"
#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/detail/SeggerTerms.hpp"
#include "uc_log/detail/TargetLoss.hpp"
#include "uc_log/detail/TcpServerCommon.hpp"
#include "uc_log/metric_utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
// clang-format off
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#  pragma clang diagnostic ignored "-Wexit-time-destructors"
#  pragma clang diagnostic ignored "-Wglobal-constructors"
#  pragma clang diagnostic ignored "-Wextra-semi-stmt"
#  pragma clang diagnostic ignored "-Wdeprecated-copy-with-dtor"
#  pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
#include <cxxopts.hpp>
#include <nlohmann/json.hpp>
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
// clang-format on
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <thread>

namespace {
// the channel of the printer's own log rows (unread boots): no RTT channel has this number
constexpr std::size_t UnreadBootsChannel = 0xFFFF;

struct LogFilePrinter {
    std::function<void(std::string_view)>                errorMessagef;
    std::function<void(LogFileStatus, std::string_view)> statusChangef;
    std::filesystem::path                                logFilePath;
    uc_log::detail::RowFile logFile;   // a failed write is said (detail/RowFile.hpp)
    bool                    logFileEnabled{true};
    std::mutex              mutex;
    // own mutex: add() reports errors through the gui (-> addStatus) while holding `mutex`
    std::filesystem::path                                      statusFilePath;
    uc_log::detail::RowFile                                    statusFile;
    std::mutex                                                 statusMutex;
    std::deque<uc_log::control::StatusMessage>                 statusLines;
    std::size_t                                                statusErrors{};
    std::function<void(uc_log::control::StatusMessage const&)> statusListener;
    static constexpr std::size_t                               StatusLinesKept = 1000;
    static constexpr auto FlushPeriod = std::chrono::milliseconds{100};

    // scripts read the .rttlog while the printer runs: a line is on disk FlushPeriod later at most
    std::jthread flusher{[this](std::stop_token const& stop) {
        while(!stop.stop_requested()) {
            std::this_thread::sleep_for(FlushPeriod);
            std::lock_guard<std::mutex> const lock{mutex};
            if(auto const said = logFile.flush()) { errorMessagef(*said); }
        }
    }};

    LogFilePrinter(uc_log::FTXUIGui::Gui& gui UC_LOG_LIFETIMEBOUND,
                   std::string const&         logDir)
      : errorMessagef{[&gui](auto const& m) { gui.errorMessage(m); }}
      , statusChangef{[&gui](LogFileStatus    s,
                             std::string_view p) { gui.setLogFileStatus(s, p); }} {
        gui.setMessageSink(
          [this](std::string_view level, std::string_view msg) { addStatus(level, msg); });
        openFileUnlocked(logDir);
    }

    void addStatus(std::string_view level,
                   std::string_view msg) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        auto                              message
          = uc_log::detail::toStatusMessage(std::chrono::system_clock::now(), level, msg);
        if(level != "status" && level != "tool") { ++statusErrors; }
        statusFile.row(uc_log::detail::statusFileLine(message));
        auto const said = statusFile.flush();
        keepStatus(std::move(message));
        // the status file's own failure: kept and shown, not written to the file that fails (and not through the gui,
        // which would come back here)
        if(said) {
            ++statusErrors;
            std::fprintf(stderr, "uc_log_printer: %s\n", said->c_str());
            keepStatus(
              uc_log::detail::toStatusMessage(std::chrono::system_clock::now(), "error", *said));
        }
    }

    // under statusMutex: stream order = file order
    void keepStatus(uc_log::control::StatusMessage message) {
        if(statusListener) { statusListener(message); }
        statusLines.push_back(std::move(message));
        if(statusLines.size() > StatusLinesKept) { statusLines.pop_front(); }
    }

    void setStatusListener(std::function<void(uc_log::control::StatusMessage const&)> f) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        statusListener = std::move(f);
    }

    std::vector<uc_log::control::StatusMessage> newestStatus(std::size_t count) {
        std::lock_guard<std::mutex> const lock{statusMutex};
        auto const                        n = std::min(count, statusLines.size());
        return {statusLines.end() - static_cast<std::ptrdiff_t>(n), statusLines.end()};
    }

    std::size_t statusErrorCount() {
        std::lock_guard<std::mutex> const lock{statusMutex};
        return statusErrors;
    }

    void changeDir(std::string const& newDir) {
        std::lock_guard<std::mutex> const lock{mutex};
        openFileUnlocked(newDir);
    }

    void setEnabled(bool enabled) {
        std::lock_guard<std::mutex> const lock{mutex};
        logFileEnabled = enabled;
    }

    void add(std::chrono::system_clock::time_point recv_time,
             uc_log::detail::LogEntry const&       entry) {
        std::lock_guard<std::mutex> const lock{mutex};
        if(!logFileEnabled || !logFile.isOpen()) {
            return;
        }   // a file that did not open was said then
        std::ostringstream row;
        uc_log::detail::logformat::writeEntry(row, recv_time, entry);
        logFile.row(row.str());   // written by `flusher`, not per line: a syscall per line
    }

private:
    void openFileUnlocked(std::string const& dir) {
        if(auto const said = logFile.flush()) { errorMessagef(*said); }
        logFilePath
          = std::filesystem::path{dir}
          / fmt::format("{}.rttlog",
                        uc_log::detail::logformat::toIso8601Utc(std::chrono::system_clock::now()));
        auto const opened = logFile.open(logFilePath);
        auto const why    = opened ? std::string{} : std::string{std::strerror(errno)};
        {
            std::lock_guard<std::mutex> const lock{statusMutex};
            statusFilePath = logFilePath;
            statusFilePath.replace_extension(".status.log");
            static_cast<void>(statusFile.open(statusFilePath));
        }
        if(!opened) {
            errorMessagef(
              fmt::format("failed to open logfile {:?}: {}", logFilePath.string(), why));
            if(statusChangef) { statusChangef(LogFileStatus::Error, logFilePath.string()); }
        } else {
            std::ostringstream header;
            uc_log::detail::logformat::writeHeader(header);
            logFile.row(header.str());
            if(statusChangef) { statusChangef(LogFileStatus::Active, logFilePath.string()); }
        }
    }
};

// The session's string catalog (detail/Catalog.hpp). A build writes it twice - its strings before the link, its call
// sites after it - and a session that starts while the tree builds would decode every line of the new image as
// "cataloged format string not found": a catalog without "Sites" is waited for up to CatalogWait, then taken as
// it is, with a word.
inline std::expected<uc_log::detail::Catalog,
                     std::string>
loadCompleteCatalog(std::string const&     file,
                    uc_log::FTXUIGui::Gui& gui) {
    static constexpr auto CatalogWait = std::chrono::seconds{60};
    auto                  catalog     = uc_log::detail::loadCatalog(file);
    if(catalog && !catalog->hasSites) {
        gui.statusMessage(
          fmt::format("waiting for the string catalog {:?}: it has no call sites yet (a build in "
                      "progress?)",
                      file));
        auto const end = std::chrono::steady_clock::now() + CatalogWait;
        while(catalog && !catalog->hasSites && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            catalog = uc_log::detail::loadCatalog(file);
        }
        if(catalog && catalog->hasSites) { gui.statusMessage("the string catalog is complete"); }
    }
    if(catalog && !catalog->hasSites) {
        gui.errorMessage(fmt::format(
          "the string catalog {:?} still has no call sites after {} s: decoding with it as "
          "it is (lines of call sites will not decode)",
          file,
          CatalogWait.count()));
    }
    if(catalog && catalog->hasSites && catalog->signatures.empty()) {
        gui.errorMessage("no call sites in " + file
                         + ": every function shows as \"?\" (rebuild the target)");
    }
    return catalog;
}

// Does the board run the build whose strings decode the log? Checked at each session start.
struct FirmwareCheck {
    using State = uc_log::control::FirmwareState;
    std::mutex                     mutex;
    uc_log::control::FirmwareCheck summary{};
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
        using uc_log::detail::ImageCheck;
        std::string                    text;
        bool                           isError = true;
        uc_log::control::FirmwareCheck state{};
        std::ifstream                  file{hexFile};
        auto const image = file ? uc_log::detail::parseIntelHex(file)
                                : std::unexpected{fmt::format("cannot open {:?}", hexFile)};
        if(!image) {
            text = fmt::format("firmware not checked: {}", image.error());
        } else if(ramImage) {
            auto const crc = uc_log::detail::imageCrc(*image);
            state          = {.state = State::unchecked, .build = crc};
            isError        = false;
            text           = fmt::format(
              "firmware not checked: {} is a RAM image (build {:08x}), and RAM "
              "changes as it runs",
              hexFile,
              crc);
        } else {
            auto const      crc   = uc_log::detail::imageCrc(*image);
            auto const      both  = uc_log::detail::checkFlash(*image, read, uncachedOffset);
            auto const&     check = both.flash;
            std::error_code ec;
            auto const      written = std::filesystem::last_write_time(hexFile, ec);
            auto const      built
              = ec ? std::string{"?"}
                   : uc_log::detail::logformat::toIso8601Utc(
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
            std::lock_guard<std::mutex> const lock{mutex};
            summary = state;
        }
        emit(checkState(state), text, isError);
    }

    uc_log::control::FirmwareCheck get() {
        std::lock_guard<std::mutex> const lock{mutex};
        return summary;
    }

    // what a check found: the GUI's badge, and the text as a status message (an error when it is one)
    using EmitF = std::function<void(uc_log::FTXUIGui::FirmwareState, std::string const&, bool)>;

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
    /// rebuild that matches what runs), and every GateRecheck while "different": a flash read can be wrong for a moment
    /// (a chip whose boot stage is still setting up the flash interface), and a gate closed on such a read would hold
    /// the log for good. A cached view that stays wrong is a cache line filled during a reset of one core
    /// (--flash_uncached_offset).
    template<typename ReadF>
    bool gate(std::string const& hexFile,
              bool               ramImage,
              ReadF&&            read,
              EmitF const&       emit) {
        static constexpr auto GateRecheck = std::chrono::seconds{2};
        auto const            now         = std::chrono::steady_clock::now();
        std::error_code       ec;
        auto const            written = std::filesystem::last_write_time(hexFile, ec);
        bool const            first   = !gateHexTime_;
        bool const            changed = !ec && (first || *gateHexTime_ != written);
        bool const due = get().state == State::different && now >= gateLastCheck_ + GateRecheck;
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
    std::chrono::steady_clock::time_point          gateLastCheck_{};

    static uc_log::FTXUIGui::FirmwareState checkState(uc_log::control::FirmwareCheck const& state) {
        using S = uc_log::FTXUIGui::FirmwareState;
        switch(state.state) {
        case State::match:     return S::match;
        case State::different: return S::different;
        case State::unchecked: return S::unchecked;
        }
        return S::unchecked;
    }
};

struct OnScopeExit {
    std::function<void()> f;

    ~OnScopeExit() {
        if(f) { f(); }
    }
};

// What the command line says.
struct PrinterOptions {
    std::uint32_t                                          speed{};
    std::string                                            device;
    std::string                                            mapFile;
    std::string                                            hexFile;
    std::string                                            stringConstantsFile;
    std::string                                            host;
    std::string                                            probe;
    std::string                                            logDir;
    std::string                                            buildCommand;
    std::string                                            bindAddressString;
    boost::asio::ip::address                               bindAddress;
    std::uint16_t                                          controlPort{};
    std::uint16_t                                          duplexBasePort{};
    std::string                                            controlSocket;
    bool                                                   unixTransport{true};
    bool                                                   disableUi{false};
    bool                                                   ramImage{false};
    std::vector<JLink::MemoryWrite>                        preResetCommands;
    std::optional<JLinkRttReader::ParkSignature>           parkSignature;
    std::optional<JLinkRttReader::StallSignature>          stallSignature;
    std::vector<JLinkRttReader::StallSignature::Condition> flashReady;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> flashWindow;
    std::uint32_t                                          flashUncachedOffset{};
    std::string                                            logFilterFile;
    std::size_t                                            controlHistoryMb{};
};

// "<a>,<b>,..." of hex words (0x optional), at most `count`; empty on a malformed one
std::vector<std::uint32_t> hexWords(std::string_view text,
                                    std::size_t      count) {
    std::vector<std::uint32_t> out;
    while(!text.empty() && out.size() != count) {
        auto const comma = text.find(',');
        try {
            out.push_back(static_cast<std::uint32_t>(
              std::stoul(std::string{text.substr(0, comma)}, nullptr, 16)));
        } catch(std::exception const&) { return std::vector<std::uint32_t>{}; }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return out;
}

// <address>,<mask>,<value>[+<address>,<mask>,<value>...]: all must hold (not ';': CMake lists)
std::optional<JLinkRttReader::StallSignature>
parseStallSignature(std::string_view                       text,
                    std::vector<JLink::MemoryWrite> const& recovery) {
    JLinkRttReader::StallSignature signature{{}, recovery};
    while(!text.empty()) {
        auto const plus = text.find('+');
        auto const v    = hexWords(text.substr(0, plus), 4);
        if(v.size() != 3) { return std::nullopt; }
        signature.conditions.push_back({v[0], v[1], v[2]});
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);
    }
    return signature;
}

// <address>,<mask>,<value>[,<pc>]
std::optional<JLinkRttReader::ParkSignature> parseParkSignature(std::string_view text) {
    auto const v = hexWords(text, 4);
    if(v.size() != 3 && v.size() != 4) { return std::nullopt; }
    return JLinkRttReader::ParkSignature{v[0],
                                         v[1],
                                         v[2],
                                         v.size() == 4 ? std::optional<std::uint32_t>{v[3]}
                                                       : std::nullopt};
}

std::vector<JLink::MemoryWrite> commands(cxxopts::ParseResult const& result,
                                         std::string const&          option) {
    std::vector<JLink::MemoryWrite> out;
    if(result.count(option) == 0) { return out; }
    for(auto const& line : result[option].as<std::vector<std::string>>()) {
        out.push_back(JLink::parseCommand(line));
    }
    return out;
}

// nullopt: the command line is wrong (said on stderr)
std::optional<PrinterOptions> parseOptions(int    argc,
                                           char** argv) {
    cxxopts::Options options(
      "uc_log_printer",
      "Reads a target's uc_log RTT rings through a J-Link and shows the log.\n"
      "Environment: UC_LOG_RTT_VIA_DLL=1 reads RTT through the J-Link DLL's own RTT instead of the "
      "printer's reader -\n"
      "  the probe's full throughput (~10 % more at saturation), but a stop, reset or flash drops "
      "what the DLL had read\n"
      "  and not handed over, without a message, and after a reset nobody announced old lines can "
      "arrive again.\n"
      "  Default: the printer's reader (every byte once, every loss announced). Ring sizes and "
      "rates: uc_log README,\n"
      "  \"RTT throughput, ring sizes\".");
    options.add_options()(
      "transport",
      "control server and duplex channels: 'unix' (control.sock, duplex.<n>.sock in the log "
      "directory) or 'tcp' (control_port, duplex_base_port + n on bind_address)",
      cxxopts::value<std::string>()->default_value("unix"))(
      "duplex_base_port",
      "first tcp port for duplex channels (--transport tcp)",
      cxxopts::value<std::uint16_t>()->default_value("34600"))(
      "control_port",
      "tcp port of the control server (--transport tcp)",
      cxxopts::value<std::uint16_t>()->default_value("34565"))(
      "control_history_mb",
      "log history the control server keeps for subscribers asking for the past (MiB)",
      cxxopts::value<std::size_t>()->default_value("128"))(
      "speed",
      "swd speed",
      cxxopts::value<std::uint32_t>())("device", "mpu device", cxxopts::value<std::string>())(
      "build_command",
      "build command",
      cxxopts::value<std::string>())("map_file", "map file", cxxopts::value<std::string>())(
      "hex_file",
      "hex file",
      cxxopts::value<std::string>())("string_constants_file",
                                     "string constants map file",
                                     cxxopts::value<std::string>())("log_dir",
                                                                    "log file directory",
                                                                    cxxopts::value<std::string>())(
      "host",
      "jlink host",
      cxxopts::value<std::string>()->default_value(""))(
      "probe",
      "jlink by serial number or nickname, on usb or on the network; required when more than one "
      "is on usb",
      cxxopts::value<std::string>()->default_value(""))(
      "bind_address",
      "address the tcp servers (control + duplex) bind to; the duplex ports give raw "
      "unauthenticated access to the "
      "target, so anything but loopback exposes that to the network",
      cxxopts::value<std::string>()->default_value("127.0.0.1"))(
      "log_filter",
      "the uc_log_filter.txt the firmware was compiled with (LogFilter.hpp): shown in the Filter "
      "tab's module tree, "
      "and a module it compiled out is named there",
      cxxopts::value<std::string>()->default_value(""))(
      "park_signature",
      "<address>,<mask>,<value>[,<pc>]: the chip's sign that the J-Link parked it in its boot ROM "
      "during its own "
      "reboot (the RP2040: CHIP_RESET.HAD_PSM_RESTART, and the PC of the boot ROM's safe loop, a "
      "wfi: the core "
      "asleep); with nothing to read, the printer resets such a chip itself and says so. From the "
      "chip package "
      "(TARGET_JLINK_PARK_SIGNATURE)",
      cxxopts::value<std::string>()->default_value(""))(
      "stall_signature",
      "<address>,<mask>,<value>[+<address>,<mask>,<value>...]: the chip's sign of a stall the log "
      "cannot show (the "
      "RP2350: the QMI stuck in direct mode, BUSY, the boot ROM waiting on the flash; the RP2040: "
      "the boot ROM's "
      "SPI transfer waiting for bytes that never come): every condition holds on every read of a "
      "series while the "
      "target says nothing; said as an error. From the chip package (TARGET_JLINK_STALL_SIGNATURE)",
      cxxopts::value<std::string>()->default_value(""))(
      "flash_ready_signature",
      "<address>,<mask>,<value>[+<address>,<mask>,<value>...]: the register words that hold while "
      "the flash is "
      "mapped (the RP2040: SSI CTRLR0.TMOD EEPROM_READ and SSIENR, as boot2 leaves them; the "
      "RP2350: QMI direct mode "
      "off) - the firmware check reads the flash only then: an XIP read while a boot ROM drives "
      "the flash by hand "
      "collides with its transfer. From the chip package (TARGET_JLINK_FLASH_READY_SIGNATURE)",
      cxxopts::value<std::string>()->default_value(""))(
      "flash_window",
      "<base>,<size>: the chip's flash window (XIP and its interface's registers) - a "
      "control-socket read there waits "
      "for the flash ready signature like the firmware check. From the chip package "
      "(TARGET_JLINK_FLASH_WINDOW)",
      cxxopts::value<std::string>()->default_value(""))(
      "flash_uncached_offset",
      "<offset>: the firmware check reads the flash at address + offset, an uncached alias of it, "
      "so its verdict "
      "is about the flash and not a cache line, and compares the cached view as well (a line that "
      "differs from "
      "flash is an error of its own). From the chip package (TARGET_JLINK_FLASH_UNCACHED_OFFSET); "
      "0 = off",
      cxxopts::value<std::string>()->default_value("0"))(
      "stall_recovery",
      "J-Link Commander line 'w4 <address> <value>' that undoes such a stall (repeatable, in "
      "order; from the chip "
      "package's TARGET_JLINK_STALL_RECOVERY)",
      cxxopts::value<std::vector<std::string>>())(
      "pre_reset_command",
      "J-Link Commander line written to the target before every reset and download, repeatable, in "
      "order; only "
      "'w4 <address> <value>'. What a chip needs there (the RP chips park core 1) comes from its "
      "package's "
      "TARGET_JLINK_CONNECT_COMMANDS",
      cxxopts::value<std::vector<std::string>>())(
      "ram_image",
      "the image lives in RAM (a Kvasir RAM_ONLY target): a reset would boot flash, so flash and "
      "reset load the hex "
      "file and start it at its vector table (VTOR, MSP, xPSR, PC from the map file's "
      "_LINKER_vectors_start_ and the "
      "hex), as the target's J-Link script does")(
      "control_socket",
      "unix domain socket of the control server (--transport unix; JSON lines, "
      "detail/ControlProtocol.hpp): 'auto' = "
      "control.sock in the log directory, where tools/uc_log_client.py looks, 'off' = no control "
      "server",
      cxxopts::value<std::string>()->default_value(
        "auto"))("disable_ui", "disable ui and just log to file and the control server");
    PrinterOptions o;
    try {
        auto const result     = options.parse(argc, argv);
        o.controlPort         = result["control_port"].as<std::uint16_t>();
        o.controlHistoryMb    = result["control_history_mb"].as<std::size_t>();
        o.duplexBasePort      = result["duplex_base_port"].as<std::uint16_t>();
        o.speed               = result["speed"].as<std::uint32_t>();
        o.device              = result["device"].as<std::string>();
        o.buildCommand        = result["build_command"].as<std::string>();
        o.mapFile             = result["map_file"].as<std::string>();
        o.hexFile             = result["hex_file"].as<std::string>();
        o.stringConstantsFile = result["string_constants_file"].as<std::string>();
        o.logDir              = result["log_dir"].as<std::string>();
        o.host                = result["host"].as<std::string>();
        o.probe               = result["probe"].as<std::string>();
        o.bindAddressString   = result["bind_address"].as<std::string>();
        o.disableUi           = result.count("disable_ui") > 0;
        o.ramImage            = result.count("ram_image") > 0;
        o.controlSocket       = result["control_socket"].as<std::string>();
        o.logFilterFile       = result["log_filter"].as<std::string>();
        o.preResetCommands    = commands(result, "pre_reset_command");
        auto const transport  = result["transport"].as<std::string>();
        if(transport != "unix" && transport != "tcp") {
            fmt::print(stderr, "Error: --transport is 'unix' or 'tcp', not {:?}\n", transport);
            return std::nullopt;
        }
        o.unixTransport = transport == "unix";
        if(auto const text = result["stall_signature"].as<std::string>(); !text.empty()) {
            o.stallSignature = parseStallSignature(text, commands(result, "stall_recovery"));
            if(!o.stallSignature) {
                fmt::print(stderr,
                           "Error: --stall_signature is "
                           "<address>,<mask>,<value>[+<address>,<mask>,<value>...], not {:?}\n",
                           text);
                return std::nullopt;
            }
        }
        if(auto const text = result["flash_ready_signature"].as<std::string>(); !text.empty()) {
            auto const ready = parseStallSignature(text, {});
            if(!ready) {
                fmt::print(stderr,
                           "Error: --flash_ready_signature is "
                           "<address>,<mask>,<value>[+<address>,<mask>,<value>...], "
                           "not {:?}\n",
                           text);
                return std::nullopt;
            }
            o.flashReady = ready->conditions;
        }
        if(auto const text = result["flash_window"].as<std::string>(); !text.empty()) {
            auto const v = hexWords(text, 2);
            if(v.size() != 2) {
                fmt::print(stderr, "Error: --flash_window is <base>,<size>, not {:?}\n", text);
                return std::nullopt;
            }
            o.flashWindow = std::pair{v[0], v[1]};
        }
        if(auto const text = result["park_signature"].as<std::string>(); !text.empty()) {
            o.parkSignature = parseParkSignature(text);
            if(!o.parkSignature) {
                fmt::print(stderr,
                           "Error: --park_signature is <address>,<mask>,<value>[,<pc>], not {:?}\n",
                           text);
                return std::nullopt;
            }
        }
        auto const offset = result["flash_uncached_offset"].as<std::string>();
        try {
            o.flashUncachedOffset = static_cast<std::uint32_t>(std::stoul(offset, nullptr, 0));
        } catch(std::exception const&) {
            fmt::print(stderr, "Error: --flash_uncached_offset is a number, not {:?}\n", offset);
            return std::nullopt;
        }
    } catch(cxxopts::exceptions::exception const& e) {
        fmt::print(stderr, "Error: {}\n{}\n", e.what(), options.help());
        return std::nullopt;
    } catch(std::runtime_error const& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return std::nullopt;
    }
    try {
        o.bindAddress = boost::asio::ip::make_address(o.bindAddressString);
    } catch(std::exception const& e) {
        fmt::print(stderr, "Error: invalid bind_address {:?}: {}\n", o.bindAddressString, e.what());
        return std::nullopt;
    }
    return o;
}

// The compile-time log filter the firmware was built with (LogFilter.hpp), for the Filter tab.
void loadLogFilter(uc_log::FTXUIGui::Gui& gui,
                   std::string const&     file) {
    if(file.empty()) { return; }
    std::ifstream     stream{file};
    std::string const text{std::istreambuf_iterator<char>{stream}, {}};
    auto const        parsed = uc_log::detail::parseFilter(text);
    if(!stream.is_open()) {
        gui.errorMessage(fmt::format("log filter {}: cannot open it", file));
    } else if(!parsed.error.empty()) {
        gui.errorMessage(fmt::format("log filter {}:{}: {}", file, parsed.line, parsed.error));
    } else {
        gui.setCompiledFilter(parsed.table);
        gui.statusMessage(fmt::format("log filter {}: {} module rule(s), every line >= {}",
                                      file,
                                      parsed.table.count,
                                      uc_log::detail::filterLevelName(parsed.table.global)));
    }
}

// SEGGER's daily EDU terms (detail/SeggerTerms.hpp), accepted for today before every connect so a connect never waits on
// the dialog - just before local midnight for the new day, after waiting for it; an outcome other than "already
// today" is said once.
std::function<void()> termsAcceptor(uc_log::FTXUIGui::Gui& gui UC_LOG_LIFETIMEBOUND) {
    return [&gui, lastSaid = std::optional<uc_log::detail::segger_terms::Result>{}]() mutable {
        using uc_log::detail::segger_terms::Result;
        namespace st = uc_log::detail::segger_terms;
        if(auto const wait = st::waitForNewDay(std::chrono::system_clock::now());
           wait > decltype(wait){})
        {
            gui.toolStatusMessage(
              fmt::format("SEGGER's EDU terms of use: connecting after local midnight, in {} s",
                          std::chrono::ceil<std::chrono::seconds>(wait).count()));
            std::this_thread::sleep_for(wait);
        }
        auto const file   = st::registryFile();
        auto const result = st::acceptToday(file);
        if(result == lastSaid) { return; }
        lastSaid = result;
        switch(result) {
        case Result::accepted:
            gui.toolStatusMessage("SEGGER's EDU terms of use accepted for today");
            break;
        case Result::alreadyToday: break;
        case Result::noFile:
            gui.toolStatusMessage(fmt::format(
              "SEGGER's EDU terms of use: no registry file {:?} (a J-Link EDU may wait on "
              "its daily dialog)",
              file.string()));
            break;
        case Result::noKey:
            gui.toolStatusMessage(fmt::format(
              "SEGGER's EDU terms of use: no {} in {:?} (a J-Link EDU may wait on its daily "
              "dialog)",
              uc_log::detail::segger_terms::Key,
              file.string()));
            break;
        case Result::unwritable:
            gui.toolErrorMessage(
              fmt::format("SEGGER's EDU terms of use: {:?} could not be written", file.string()));
            break;
        }
    };
}

// The row that says the count of boots that logged starts again (the printer's own download wrote over the target's
// RAM): the next boot's number is no continuation of the last one's.
uc_log::detail::LogEntry bootCountRestartRow() {
    uc_log::detail::LogEntry entry{UnreadBootsChannel, std::string_view{}};
    entry.fileName     = "uc_log_printer";
    entry.functionName = "printer";
    entry.logLevel     = uc_log::LogLevel::warn;
    entry.module       = "uc_log";
    entry.logMsg
      = "⚠ the boot count starts again: the printer's download wrote over the target's RAM";
    entry.parsedOk = true;
    return entry;
}

// The rows the printer adds to the log itself: boots that logged and were replaced unread (detail/BootLedger.hpp) - module
// uc_log, so whoever reads the log (the stress checker, a person) finds the loss where it happened. Its own channel
// and target time 0: it goes out ahead of the next boot's lines.
uc_log::detail::LogEntry unreadBootsRow(std::uint32_t unread,
                                        std::uint32_t from,
                                        std::uint32_t to) {
    uc_log::detail::LogEntry entry{UnreadBootsChannel, std::string_view{}};
    entry.fileName     = "uc_log_printer";
    entry.functionName = "printer";
    entry.logLevel     = uc_log::LogLevel::warn;
    entry.module       = "uc_log";
    entry.logMsg       = fmt::format("⚠ {} boot{} passed unread (boots that logged {} -> {})",
                                     unread,
                                     unread == 1 ? "" : "s",
                                     from,
                                     to);
    entry.parsedOk     = true;
    return entry;
}

// The control socket's and the duplex channels' settings from the GUI (port, transport, bind address, on / off).
void wireNetworkSettings(uc_log::FTXUIGui::Gui&                        gui,
                         std::optional<uc_log::detail::ControlServer>& controlServer,
                         uc_log::detail::DuplexChannelHub&             duplexHub,
                         LogFilePrinter&                               logFilePrinter,
                         PrinterOptions const&                         o,
                         std::function<std::filesystem::path(bool)>    controlSocketFor) {
    gui.setOnTcpPortChange([&controlServer](std::uint16_t newPort) {
        if(controlServer) { controlServer->restart(newPort); }
    });
    gui.setTcpClientCountGetter([&controlServer]() -> std::size_t {
        return controlServer ? controlServer->getClientCount() : 0;
    });
    gui.setOnLogDirChange(
      [&logFilePrinter](std::string const& newDir) { logFilePrinter.changeDir(newDir); });
    gui.setOnLogFileEnable([&logFilePrinter](bool enabled) { logFilePrinter.setEnabled(enabled); });
    gui.setOnTcpEnable([&controlServer, controlPort = o.controlPort](bool enabled) {
        if(!controlServer) { return; }
        if(enabled) {
            auto const current = controlServer->getPort();
            controlServer->restart(current != 0 ? current : controlPort);
        } else {
            controlServer->stop();
        }
    });
    gui.setDuplexInfoGetter([&duplexHub]() { return duplexHub.info(); });
    gui.setOnDuplexPortChange([&duplexHub](std::size_t ordinal, std::uint16_t newPort) {
        duplexHub.setPort(ordinal, newPort);
    });
    gui.setOnDuplexEnable(
      [&duplexHub](std::size_t ordinal, bool enabled) { duplexHub.setEnabled(ordinal, enabled); });
    gui.setOnDuplexBasePortChange(
      [&duplexHub](std::uint16_t newBasePort) { duplexHub.setBasePort(newBasePort); });
    gui.setDuplexBasePort(o.duplexBasePort);
    gui.setOnTransportChange(
      [&gui, &controlServer, controlSocketFor, &duplexHub, logDir = o.logDir](bool unix) {
          auto const controlPath = controlSocketFor(unix);
          if(controlServer) { controlServer->setSocketPath(controlPath); }
          duplexHub.setSocketDir(unix ? std::filesystem::path{logDir} : std::filesystem::path{});
          gui.setControlSocketPath(controlServer ? controlPath.native() : std::string{});
      });
    // every socket rebound at runtime; false on an unparsable address, so the GUI can show it without applying it
    gui.setOnNetworkBindAddressChange([&controlServer, &duplexHub](std::string const& s) {
        boost::system::error_code ec;
        auto const                address = boost::asio::ip::make_address(s, ec);
        if(ec) { return false; }
        if(controlServer) { controlServer->setBindAddress(address); }
        duplexHub.setBindAddress(address);
        return true;
    });
}

// The ledger of a LossPolicy::Counted firmware (::lossLedger in the map): read every 2 s, it tells of a ring that
// dropped records and has written nothing since (detail/TargetLoss.hpp). A rebuilt map is a new snapshot.
void watchLossLedger(std::stop_token const&      stop,
                     uc_log::detail::MapFacts&   mapFacts,
                     JLinkRttReader&             rttReader,
                     uc_log::detail::TargetLoss& targetLoss,
                     std::mutex&                 targetLossMutex,
                     uc_log::FTXUIGui::Gui&      gui) {
    std::shared_ptr<uc_log::detail::MapFacts::Facts const> seen;
    std::optional<uc_log::detail::LedgerPlace>             ledger;
    while(!stop.stop_requested()) {
        for(int i = 0; i != 20 && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if(auto const facts = mapFacts.get(); facts != seen) {
            seen   = facts;
            ledger = facts->ledger;
            if(facts->ledgers > 1) {
                gui.errorMessage(fmt::format(
                  "{} ::lossLedger symbols in the map (two counting log backends?): reading none",
                  facts->ledgers));
            }
            gui.targetLossCounted(ledger.has_value());
            std::lock_guard<std::mutex> const lock{targetLossMutex};
            targetLoss.clearWatch();
        }
        if(!ledger) { continue; }
        std::array<uc_log::detail::TargetOp, 1> const read{
          uc_log::detail::target_op::Read{ledger->address, ledger->size}
        };
        auto const result = rttReader.accessTarget(read);
        if(!result || result->empty()) { continue; }
        auto const               rings = uc_log::detail::decodeLossLedger(result->front());
        std::vector<std::string> messages;
        {
            std::lock_guard<std::mutex> const lock{targetLossMutex};
            messages = targetLoss.ledger(rings, std::chrono::steady_clock::now());
        }
        for(auto const& m : messages) { gui.errorMessage(m); }
    }
}

// What the control socket's requests do (detail/ControlServer.hpp): target requests run on the reader thread.
uc_log::detail::ControlTarget controlTarget(JLinkRttReader& rttReader,
                                            std::function<uc_log::control::StatusAnswer()> status,
                                            LogFilePrinter& logFilePrinter) {
    namespace to = uc_log::detail::target_op;
    using uc_log::detail::TargetOp;
    auto const one
      = [&rttReader](TargetOp const& op) -> std::expected<std::vector<std::byte>, std::string> {
        std::array<TargetOp, 1> const ops{op};
        auto                          result = rttReader.accessTarget(ops);
        if(!result) { return std::unexpected{result.error()}; }
        return std::move(result->front());
    };
    return uc_log::detail::ControlTarget{
      .read =
        [&rttReader](std::span<uc_log::control::Piece const> pieces) {
            std::vector<TargetOp> ops;
            ops.reserve(pieces.size());
            for(auto const& p : pieces) { ops.emplace_back(to::Read{p.address, p.size}); }
            return rttReader.accessTarget(ops);
        },
      .write =
        [&rttReader](std::span<uc_log::control::WordWrite const> words) {
            std::vector<TargetOp> ops;
            ops.reserve(words.size());
            for(auto const& w : words) { ops.emplace_back(to::WriteWord{w.address, w.value}); }
            return rttReader.accessTarget(ops);
        },
      .status = std::move(status),
      .reset =
        [&rttReader](std::function<bool()> const& cancelled) {
            return rttReader.resetAndWait(std::chrono::seconds{20}, cancelled);
        },
      .flash =
        [&rttReader](std::function<bool()> const& cancelled) {
            return rttReader.flashAndWait(std::chrono::seconds{90}, cancelled);
        },
      .core = [one](bool halt) -> std::expected<bool, std::string> {
          auto const answer = halt ? one(to::Halt{}) : one(to::Go{});
          if(!answer) { return std::unexpected{answer.error()}; }
          return !answer->empty() && answer->front() != std::byte{0};
      },
      .registers = [one]() -> std::expected<std::vector<std::uint32_t>, std::string> {
          auto const raw = one(to::ReadRegisters{});
          if(!raw) { return std::unexpected{raw.error()}; }
          std::vector<std::uint32_t> values;
          for(std::size_t i = 0; i + 4 <= raw->size(); i += 4) {
              values.push_back(uc_log::detail::le32(*raw, i));
          }
          return values;
      },
      .writeRegisters = [&rttReader](std::span<uc_log::control::RegisterWrite const> writes)
        -> std::expected<void, std::string> {
          std::vector<TargetOp> ops;
          for(auto const& w : writes) { ops.emplace_back(to::WriteRegister{w.index, w.value}); }
          auto const result = rttReader.accessTarget(ops);
          if(!result) { return std::unexpected{result.error()}; }
          return {};
      },
      .messages
      = [&logFilePrinter](std::size_t count) { return logFilePrinter.newestStatus(count); },
      .unixMicros   = {},
      .steadyMicros = {},
      .stop         = {}};
}

// The end is bounded: from the stop on, the reader drains and decodes what it holds and the threads join - if that
// is not done within ExitDeadline (a probe that hangs, a backlog that grinds), the process ends anyway and says so: a
// printer that does not go away holds the probe and blocks the next one.
void armExitDeadline() {
    static constexpr auto ExitDeadline = std::chrono::seconds{5};
    std::thread{[] {
        std::this_thread::sleep_for(ExitDeadline);
        std::fputs(
          "uc_log_printer: did not end within 5 s of the stop - exiting anyway; whatever was read "
          "from the "
          "target and not decoded yet is dropped\n",
          stderr);
        std::fflush(stderr);
        std::_Exit(3);
    }}.detach();
}

}   // namespace

int main(int    argc,
         char** argv) {
    auto const opts = parseOptions(argc, argv);
    if(!opts) { return 1; }
    auto const& o         = *opts;
    auto const  startedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    uc_log::FTXUIGui::Gui gui{};
    gui.setEchoToStderr(o.disableUi);
    loadLogFilter(gui, o.logFilterFile);
    gui.setNetworkBindAddress(o.bindAddressString);
    LogFilePrinter              logFilePrinter{gui, o.logDir};
    uc_log::detail::AsioContext asioContext;
    auto const                  controlSocketFor = [&o](bool unix) {
        if(!unix) { return std::filesystem::path{}; }
        return o.controlSocket == "auto" ? uc_log::detail::controlSocketPath(o.logDir)
                                         : std::filesystem::path{o.controlSocket};
    };

    // a failed bind does not stop logging
    std::optional<uc_log::detail::ControlServer> controlServer;
    if(o.controlSocket != "off" && !o.controlSocket.empty()) {
        auto const path = controlSocketFor(o.unixTransport);
        controlServer.emplace(
          o.bindAddress,
          o.controlPort,
          path,
          [&gui](std::string_view msg) {
              gui.toolErrorMessage(fmt::format("control server: {}", msg));
          },
          [&gui](TcpPortStatus s, std::uint16_t p) { gui.setTcpPortStatus(s, p); });
        controlServer->setHistoryLimit(o.controlHistoryMb * 1024 * 1024);
        gui.setControlSocketPath(path.native());
        gui.toolStatusMessage(
          path.empty() ? fmt::format("control server on {}:{}", o.bindAddressString, o.controlPort)
                       : fmt::format("control socket {}", path.native()));
        logFilePrinter.setStatusListener(
          [&controlServer](uc_log::control::StatusMessage const& m) { controlServer->publish(m); });
    }
    // before the server goes: nothing publishes into it while it is torn down
    OnScopeExit const stopStatusStream{[&logFilePrinter] { logFilePrinter.setStatusListener({}); }};

    uc_log::detail::DuplexChannelHub duplexHub{
      asioContext.ioc,
      o.bindAddress,
      o.duplexBasePort,
      [&gui](std::string_view msg) { gui.errorMessage(msg); },
      [&gui](std::string_view msg) { gui.statusMessage(msg); },
      [&gui]() { gui.triggerRedraw(); },
      o.unixTransport ? std::filesystem::path{o.logDir} : std::filesystem::path{}};
    wireNetworkSettings(gui, controlServer, duplexHub, logFilePrinter, o, controlSocketFor);

    // declared after all users of the context: joined first on destruction so no asio handler runs while the servers
    // above are torn down
    uc_log::detail::AsioContextRunner asioRunner{asioContext};

    TimeDelayedQueue queue{
      [](uc_log::detail::LogEntry const& entry) { return entry.ucTime; },
      [](uc_log::detail::LogEntry const& entry) { return entry.channel.channel; },
      [&logFilePrinter, &controlServer, &gui](std::chrono::system_clock::time_point recv_time,
                                              uc_log::detail::LogEntry const&       entry) {
          logFilePrinter.add(recv_time, entry);
          if(controlServer) {
              using uc_log::control::Stream;
              controlServer->publish(uc_log::detail::toLogLine(recv_time, entry));
              if(controlServer->wants(Stream::metrics)) {
                  for(auto const& sample : uc_log::detail::toMetricSamples(recv_time, entry)) {
                      controlServer->publish(sample);
                  }
              }
          }
          gui.add(recv_time, entry);
      }};

    // the session's catalog: loaded at its start, before its decoder runs (decode thread) - never changed under it
    std::shared_ptr<uc_log::detail::Catalog const> catalog;
    // LossPolicy::Counted firmware: its lost-records counted here, its ledger read by watchLossLedger
    uc_log::detail::TargetLoss targetLoss;
    std::mutex                 targetLossMutex;
    std::atomic<std::uint64_t> targetLostTotal{0};
    uc_log::detail::MapFacts   mapFacts{o.mapFile};

    JLinkRttReader rttReader{
      o.host,
      o.device,
      o.speed,
      o.probe,
      [&mapFacts, &gui]() {
          auto const result = mapFacts.get()->block;
          if(!result.has_value()) { gui.fatalError(result.error()); }
          return result.value_or(RttBlockInfo{});
                           },
      [&o]() { return o.hexFile; },
      [&o, &gui, &catalog]() {
          auto loaded = loadCompleteCatalog(o.stringConstantsFile, gui);
          if(!loaded) { gui.fatalError(loaded.error()); }
          catalog = loaded ? std::make_shared<uc_log::detail::Catalog const>(std::move(*loaded))
                           : std::make_shared<uc_log::detail::Catalog const>();
          return catalog->strings;
                           },
      // the decode thread, while the session's catalog stands
      [&queue, &catalog, &gui, &targetLoss, &targetLossMutex, &targetLostTotal](
        std::size_t                           channel,
        std::string_view                      msg,
        std::optional<remote_fmt::catalog_id> id) {
          uc_log::detail::LogEntry entry{channel, msg, catalog->signatures.find(id)};
          if(auto const n = uc_log::detail::lostRecordCount(entry)) {
              std::string text;
              {
                  std::lock_guard<std::mutex> const lock{targetLossMutex};
                  text = targetLoss.lostRecord(channel, *n, entry.ucTime.time);
              }
              targetLostTotal += *n;
              gui.targetRecordsLost(*n);
              gui.errorMessage(text);
          }
          queue.append(std::move(entry));
                           },
      [&gui](std::string_view msg) { gui.statusMessage(msg); },
      [&gui](std::string_view msg) { gui.errorMessage(msg); },
      [&gui](std::string_view msg) { gui.toolStatusMessage(msg); },
      [&gui](std::string_view msg) { gui.toolErrorMessage(msg); },
      uc_log::detail::DuplexBridge{
                           [&duplexHub](std::vector<uc_log::detail::DuplexChannelDesc> const& descs) {
            duplexHub.configure(descs);
        }, [&duplexHub](std::size_t ordinal, std::span<std::byte const> data) {
            duplexHub.sendToClient(ordinal, data);
        }, [&duplexHub](std::size_t ordinal, std::span<std::byte> out) {
            return duplexHub.peekFromClient(ordinal, out);
        }, [&duplexHub](std::size_t ordinal, std::size_t n) {
            duplexHub.consumeFromClient(ordinal, n);
        }}
    };

    rttReader.setPreResetCommands(o.preResetCommands);
    rttReader.setBeforeConnect(termsAcceptor(gui));
    rttReader.setStallSignature(o.stallSignature);
    rttReader.setFlashReadySignature(o.flashReady);
    rttReader.setFlashWindow(o.flashWindow);
    rttReader.setParkSignature(o.parkSignature);
    // a firmware that announces its own resets (Kvasir_SDK Util/AnnouncedReset.hpp) has the block in its map
    rttReader.setAnnouncedReset([&mapFacts]() { return mapFacts.get()->announce; });
    // the image's RTT epoch (detail/RttEpoch.hpp): fences the printer's ring accesses against a re-init
    rttReader.setRttEpochSource([&mapFacts]() { return mapFacts.get()->epoch; });
    // the image's count of boots that logged (BootLog.hpp): the boots nobody read are counted by it, exactly
    rttReader.setLoggedBootsSource([&mapFacts]() { return mapFacts.get()->loggedBoots; });
    rttReader.setUnreadBootsCallback(
      [&queue](std::uint32_t unread, std::uint32_t from, std::uint32_t to) {
          queue.append(unreadBootsRow(unread, from, to));
      });
    rttReader.setBootCountRestartCallback([&queue]() { queue.append(bootCountRestartRow()); });
    if(o.ramImage) {
        // asked before each flash and reset: a rebuild may have moved the table
        rttReader.setRamImage(
          [&o]() { return uc_log::detail::ramImageStart(o.mapFile, o.hexFile); });
        gui.toolStatusMessage(fmt::format(
          "RAM image: flash and reset load {} and start it at its vector table, no reset "
          "after the download",
          o.hexFile));
    }
    // UC_LOG_RTT_VIA_DLL=1: RTT through the J-Link DLL's own RTT - the max-throughput mode, with its known weaknesses
    // (a session end drops the DLL's unread bytes unannounced; an unannounced reset can replay); see --help
    if(auto const* via = std::getenv("UC_LOG_RTT_VIA_DLL");
       via != nullptr && std::string_view{via} == "1")
    {
        rttReader.setUseDirectRtt(false);
        gui.toolStatusMessage(
          "UC_LOG_RTT_VIA_DLL=1: RTT through the J-Link DLL (max throughput; a stop or reset can "
          "drop "
          "or repeat lines without a message)");
    }
    // the image's own word on its frames (the session's catalog, loaded just before)
    rttReader.setFrameCheckSource([&catalog, &gui] {
        auto const check = catalog ? catalog->frameCheck : remote_fmt::FrameCheck::either;
        if(check != remote_fmt::FrameCheck::either) {
            gui.toolStatusMessage(
              check == remote_fmt::FrameCheck::required
                ? "the image checks its frames (CRC-8): unchecked frames are refused"
                : "the image has no frame check (built before it): checked frames are refused");
        }
        return check;
    });

    FirmwareCheck firmwareCheck;
    firmwareCheck.uncachedOffset = o.flashUncachedOffset;
    FirmwareCheck::EmitF const sayFirmwareCheck
      = [&gui](uc_log::FTXUIGui::FirmwareState state, std::string const& text, bool isError) {
            gui.firmwareChecked(state);
            if(isError) {
                gui.errorMessage(text);
            } else {
                gui.statusMessage(text);
            }
        };
    rttReader.setOnSessionStart([&](JLinkRttReader::DirectMemoryRead const& read) {
        // the last session's lines out before this one's: after a reset the target's times start again at 0 and
        // would sort in front of the old boot's last lines in the merge
        queue.flush();
        firmwareCheck.check(o.hexFile, o.ramImage, read, sayFirmwareCheck);
    });
    // decode only with the strings of the image the target runs (FirmwareCheck::gate)
    rttReader.setDecodeGate([&](JLinkRttReader::DirectMemoryRead const& read) {
        return firmwareCheck.gate(o.hexFile, o.ramImage, read, sayFirmwareCheck);
    });

    std::jthread ledgerWatch{[&](std::stop_token const& stop) {
        watchLossLedger(stop, mapFacts, rttReader, targetLoss, targetLossMutex, gui);
    }};

    // cleared again before the reader goes: its requests run on the reader
    if(controlServer) {
        controlServer->setTarget(controlTarget(
          rttReader,
          [&rttReader,
           &firmwareCheck,
           &logFilePrinter,
           &controlServer,
           &targetLostTotal,
           startedUs] {
              return uc_log::control::StatusAnswer{
                .running      = rttReader.getStatus().isRunning != 0,
                .halted       = rttReader.isHalted(),
                .flashing     = rttReader.isFlashing(),
                .sessions     = static_cast<std::uint32_t>(rttReader.sessionCount()),
                .firmware     = firmwareCheck.get(),
                .errors       = logFilePrinter.statusErrorCount(),
                .log_seq      = controlServer->logSeq(),
                .started_us   = startedUs,
                .ram_image    = rttReader.isRamImage(),
                .target_lost  = targetLostTotal.load(),
                .rtt_bytes    = rttReader.rttBytes(),
                .probe_serial = rttReader.probeSerial()};
          },
          logFilePrinter));
    }
    OnScopeExit const releaseTarget{[&controlServer] {
        if(controlServer) { controlServer->clearTarget(); }
    }};

    if(!o.disableUi) {
        gui.setMapFile(o.mapFile);
        auto const result = gui.run(rttReader, o.buildCommand, o.host, o.probe);
        armExitDeadline();
        return result;
    }
    static std::atomic<bool> shutdownRequested{false};
    std::signal(SIGINT, [](int signal) {
        if(signal == SIGINT) { shutdownRequested = true; }
    });
    while(!shutdownRequested) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    armExitDeadline();
    return 0;
}
