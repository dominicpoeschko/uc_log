#include "uc_log/printer/Options.hpp"

#include <array>
#include <boost/asio/ip/address.hpp>
#include <exception>
#include <fmt/format.h>
#include <memory>
#include <stdexcept>
#include <string>
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
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
// clang-format on

namespace uc_log::printer {

namespace {
    // What an option's value is. `text` with a default "" is an option that may be left out.
    enum class Type { text, port, u32, size, list, flag };

    using Value  = cxxopts::OptionValue const&;
    using Result = cxxopts::ParseResult const&;

    // An option's whole contract in one row: its name, its help, its value (type and default, nullptr = required)
    // and what it does to PrinterOptions. `apply` gets the option's own value and the whole result (for an option
    // that reads another: the stall signature its recovery lines); it returns the error to say, empty when the value
    // is taken. A malformed J-Link command line throws std::runtime_error (JLink::parseCommand), said the same way.
    struct Option {
        char const* name;
        char const* help;
        Type        type;
        char const* defaultValue;
        std::string (*apply)(Value           own,
                             Result          all,
                             PrinterOptions& o);
    };

    template<typename T>
    std::shared_ptr<cxxopts::Value const> valueOf(char const* defaultValue) {
        auto value = cxxopts::value<T>();
        if(defaultValue != nullptr) { value->default_value(defaultValue); }
        return value;
    }

    std::shared_ptr<cxxopts::Value const> valueOf(Option const& option) {
        switch(option.type) {
        case Type::text: return valueOf<std::string>(option.defaultValue);
        case Type::port: return valueOf<std::uint16_t>(option.defaultValue);
        case Type::u32:  return valueOf<std::uint32_t>(option.defaultValue);
        case Type::size: return valueOf<std::size_t>(option.defaultValue);
        case Type::list: return valueOf<std::vector<std::string>>(option.defaultValue);
        case Type::flag: return valueOf<bool>(option.defaultValue);
        }
        return valueOf<bool>(option.defaultValue);
    }

    std::vector<JLink::MemoryWrite> commands(Value value) {
        std::vector<JLink::MemoryWrite> out;
        if(value.count() == 0) { return out; }
        for(auto const& line : value.as<std::vector<std::string>>()) {
            out.push_back(JLink::parseCommand(line));
        }
        return out;
    }

    // the error for a signature-like option `name` whose text is not `shape`
    std::string notA(char const*        name,
                     char const*        shape,
                     std::string const& text) {
        return fmt::format("--{} is {}, not {:?}", name, shape, text);
    }

    constexpr char const* signatureShape = "<address>,<mask>,<value>[+<address>,<mask>,<value>...]";

    // The table: one row per option, in the order of --help.
    std::vector<Option> optionTable() {
        std::vector<Option> rows;
        auto                add
          = [&rows](char const* name,
                    char const* help,
                    Type        type,
                    char const* defaultValue,
                    auto apply) { rows.push_back(Option{name, help, type, defaultValue, apply}); };
        add("transport",
            "control server and duplex channels: 'unix' (control.sock, duplex.<n>.sock in the "
            "log directory) or 'tcp' (control_port, duplex_base_port + n on bind_address)",
            Type::text,
            "unix",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                auto const& transport = own.as<std::string>();
                if(transport != "unix" && transport != "tcp") {
                    return fmt::format("--transport is 'unix' or 'tcp', not {:?}", transport);
                }
                o.unixTransport = transport == "unix";
                return {};
            });
        add("duplex_base_port",
            "first tcp port for duplex channels (--transport tcp)",
            Type::port,
            "34600",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.duplexBasePort = own.as<std::uint16_t>();
                return {};
            });
        add("control_port",
            "tcp port of the control server (--transport tcp)",
            Type::port,
            "34565",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.controlPort = own.as<std::uint16_t>();
                return {};
            });
        add("control_history_mb",
            "log history the control server keeps for subscribers asking for the past (MiB)",
            Type::size,
            "128",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.controlHistoryMb = own.as<std::size_t>();
                return {};
            });
        add("speed",
            "swd speed",
            Type::u32,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.speed = own.as<std::uint32_t>();
                return {};
            });
        add("device",
            "mpu device",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.device = own.as<std::string>();
                return {};
            });
        add("build_command",
            "build command",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.buildCommand = own.as<std::string>();
                return {};
            });
        add("map_file",
            "map file",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.mapFile = own.as<std::string>();
                return {};
            });
        add("hex_file",
            "hex file",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.hexFile = own.as<std::string>();
                return {};
            });
        add("string_constants_file",
            "string constants map file",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.stringConstantsFile = own.as<std::string>();
                return {};
            });
        add("log_dir",
            "log file directory",
            Type::text,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.logDir = own.as<std::string>();
                return {};
            });
        add("host",
            "jlink host",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.host = own.as<std::string>();
                return {};
            });
        add("probe",
            "jlink by serial number or nickname, on usb or on the network; required when more "
            "than one is on usb",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.probe = own.as<std::string>();
                return {};
            });
        add("bind_address",
            "address the tcp servers (control + duplex) bind to; the duplex ports give raw "
            "unauthenticated access to the target, so anything but loopback exposes that to "
            "the network",
            Type::text,
            "127.0.0.1",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.bindAddressString = own.as<std::string>();
                try {
                    o.bindAddress = boost::asio::ip::make_address(o.bindAddressString);
                } catch(std::exception const& e) {
                    return fmt::format("invalid bind_address {:?}: {}",
                                       o.bindAddressString,
                                       e.what());
                }
                return {};
            });
        add("log_filter",
            "the uc_log_filter.txt the firmware was compiled with (LogFilter.hpp): shown in "
            "the Filter tab's module tree, and a module it compiled out is named there",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.logFilterFile = own.as<std::string>();
                return {};
            });
        add("park_signature",
            "<address>,<mask>,<value>[,<pc>]: the chip's sign that the J-Link parked it in its "
            "boot ROM during its own reboot (the RP2040: CHIP_RESET.HAD_PSM_RESTART, and the "
            "PC of the boot ROM's safe loop, a wfi: the core asleep); with nothing to read, "
            "the printer resets such a chip itself and says so. From the chip package "
            "(kvasir_chip JLINK_PARK_SIGNATURE)",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                auto const& text = own.as<std::string>();
                if(text.empty()) { return {}; }
                o.parkSignature = parseParkSignature(text);
                return o.parkSignature
                       ? std::string{}
                       : notA("park_signature", "<address>,<mask>,<value>[,<pc>]", text);
            });
        add("stall_signature",
            "<address>,<mask>,<value>[+<address>,<mask>,<value>...]: the chip's sign of a "
            "stall the log cannot show (the RP2350: the QMI stuck in direct mode, BUSY, the "
            "boot ROM waiting on the flash; the RP2040: the boot ROM's SPI transfer waiting "
            "for bytes that never come): every condition holds on every read of a series while "
            "the target says nothing; said as an error. From the chip package (kvasir_chip "
            "JLINK_STALL_SIGNATURE)",
            Type::text,
            "",
            [](Value own, Result all, PrinterOptions& o) -> std::string {
                auto const& text = own.as<std::string>();
                if(text.empty()) { return {}; }
                o.stallSignature = parseStallSignature(text, commands(all["stall_recovery"]));
                return o.stallSignature ? std::string{}
                                        : notA("stall_signature", signatureShape, text);
            });
        add("flash_ready_signature",
            "<address>,<mask>,<value>[+<address>,<mask>,<value>...]: the register words that "
            "hold while the flash is mapped (the RP2040: SSI CTRLR0.TMOD EEPROM_READ and SSIENR, "
            "as boot2 leaves them; the RP2350: QMI direct mode off) - the firmware check reads "
            "the flash only then: an XIP read while a boot ROM drives the flash by hand collides "
            "with its transfer. From the chip package (kvasir_chip JLINK_FLASH_READY_SIGNATURE)",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                auto const& text = own.as<std::string>();
                if(text.empty()) { return {}; }
                auto const ready = parseStallSignature(text, {});
                if(!ready) { return notA("flash_ready_signature", signatureShape, text); }
                o.flashReady = ready->conditions;
                return {};
            });
        add("flash_window",
            "<base>,<size>: the chip's flash window (XIP and its interface's registers) - a "
            "control-socket read there waits for the flash ready signature like the firmware "
            "check. From the chip package (kvasir_chip JLINK_FLASH_WINDOW)",
            Type::text,
            "",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                auto const& text = own.as<std::string>();
                if(text.empty()) { return {}; }
                auto const v = printer::hexWords(text, 2);
                if(v.size() != 2) { return notA("flash_window", "<base>,<size>", text); }
                o.flashWindow = std::pair{v[0], v[1]};
                return {};
            });
        add("flash_uncached_offset",
            "<hex offset>: the firmware check reads the flash at address + offset, an uncached "
            "alias of it, so its verdict is about the flash and not a cache line, and compares "
            "the cached view as well (a line that differs from flash is an error of its own). "
            "From the chip package (kvasir_chip JLINK_FLASH_UNCACHED_OFFSET); 0 = off",
            Type::text,
            "0",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                // a hex word, `0x` optional, like every other address of the command line
                auto const& text = own.as<std::string>();
                auto const  v    = printer::hexWords(text, 1);
                if(v.size() != 1) { return notA("flash_uncached_offset", "a hex number", text); }
                o.flashUncachedOffset = v[0];
                return {};
            });
        add("stall_recovery",
            "J-Link Commander line 'w4 <address> <value>' that undoes such a stall "
            "(repeatable, in order; from the chip package's kvasir_chip JLINK_STALL_RECOVERY)",
            Type::list,
            nullptr,
            [](Value own, Result all, PrinterOptions&) -> std::string {
                // the lines are read by stall_signature; without one they would undo nothing
                if(own.count() != 0 && all["stall_signature"].as<std::string>().empty()) {
                    return "--stall_recovery without --stall_signature (it undoes that stall)";
                }
                return {};
            });
        add("pre_reset_command",
            "J-Link Commander line written to the target before every reset and download, "
            "repeatable, in order; only 'w4 <address> <value>'. What a chip needs there (the "
            "RP chips park core 1) comes from its package's kvasir_chip JLINK_CONNECT_COMMANDS",
            Type::list,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.preResetCommands = commands(own);
                return {};
            });
        add("ram_image",
            "the image lives in RAM (a Kvasir RAM_ONLY target): a reset would boot flash, so "
            "flash and reset load the hex file and start it at its vector table (VTOR, MSP, "
            "xPSR, PC from the map file's _LINKER_vectors_start_ and the hex), as the target's "
            "J-Link script does",
            Type::flag,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.ramImage = own.count() > 0;
                return {};
            });
        add("control_socket",
            "unix domain socket of the control server (--transport unix; JSON lines, "
            "printer/ControlProtocol.hpp): 'auto' = control.sock in the log directory, where "
            "tools/uc_log_client.py looks, 'off' = no control server",
            Type::text,
            "auto",
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.controlSocket = own.as<std::string>();
                return {};
            });
        add("disable_ui",
            "disable ui and just log to file and the control server",
            Type::flag,
            nullptr,
            [](Value own, Result, PrinterOptions& o) -> std::string {
                o.disableUi = own.count() > 0;
                return {};
            });
        return rows;
    }
}   // namespace

std::optional<PrinterOptions> parseOptions(int    argc,
                                           char** argv) {
    cxxopts::Options options(
      "uc_log_printer",
      "Reads a target's uc_log RTT rings through a J-Link and shows the log.\nEnvironment: "
      "UC_LOG_RTT_VIA_DLL=1 reads RTT through the J-Link DLL's own RTT instead of the printer's "
      "reader -\n  the probe's full throughput (~10 % more at saturation), but a stop, reset or "
      "flash drops what the DLL had read\n  and not handed over, without a message, and after a "
      "reset nobody announced old lines can arrive again.\n  Default: the printer's reader (every "
      "byte once, every loss announced). Ring sizes and rates: uc_log README,\n  \"RTT throughput, "
      "ring sizes\".\nUC_LOG_STALL_FIRST_STEP=2|3 (a test switch) starts the stall recovery ladder "
      "at that step.");
    auto const table = optionTable();
    auto       adder = options.add_options();
    for(auto const& option : table) { adder(option.name, option.help, valueOf(option)); }
    PrinterOptions o;
    try {
        auto const result = options.parse(argc, argv);
        for(auto const& option : table) {
            if(auto const error = option.apply(result[option.name], result, o); !error.empty()) {
                fmt::print(stderr, "Error: {}\n", error);
                return std::nullopt;
            }
        }
    } catch(cxxopts::exceptions::exception const& e) {
        fmt::print(stderr, "Error: {}\n{}\n", e.what(), options.help());
        return std::nullopt;
    } catch(std::runtime_error const& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return std::nullopt;
    }
    return o;
}

}   // namespace uc_log::printer
