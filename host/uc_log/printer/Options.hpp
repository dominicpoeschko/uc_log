#pragma once

// The printer's command line: names, defaults and help text are a contract (uc_log cmake/RttJlink.cmake's
// printer_option_table and its golden, kvasir_bench.py's printer start) - change them only together with those.
// Options.cpp holds every option as one row: name, help, value type and default, and what it sets.

#include "jlink/JLink.hpp"
#include "uc_log/printer/HexToken.hpp"
#include "uc_log/printer/TargetHealth.hpp"

#include <boost/asio/ip/address.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fmt/format.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

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
    std::optional<printer::ParkSignature>                  parkSignature;
    std::optional<printer::StallSignature>                 stallSignature;
    std::vector<printer::StallSignature::Condition>        flashReady;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> flashWindow;
    std::uint32_t                                          flashUncachedOffset{};
    std::string                                            logFilterFile;
    std::size_t                                            controlHistoryMb{};
};

// <address>,<mask>,<value>[+<address>,<mask>,<value>...]: all must hold (not ';': CMake lists)
inline std::optional<printer::StallSignature>
parseStallSignature(std::string_view                       text,
                    std::vector<JLink::MemoryWrite> const& recovery) {
    printer::StallSignature signature{{}, recovery};
    while(!text.empty()) {
        auto const plus = text.find('+');
        auto const v    = printer::hexWords(text.substr(0, plus), 4);
        if(v.size() != 3) { return std::nullopt; }
        signature.conditions.push_back({v[0], v[1], v[2]});
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);
    }
    return signature;
}

// <address>,<mask>,<value>[,<pc>]
inline std::optional<printer::ParkSignature> parseParkSignature(std::string_view text) {
    auto const v = printer::hexWords(text, 4);
    if(v.size() != 3 && v.size() != 4) { return std::nullopt; }
    return printer::ParkSignature{v[0],
                                  v[1],
                                  v[2],
                                  v.size() == 4 ? std::optional<std::uint32_t>{v[3]}
                                                : std::nullopt};
}

// nullopt: the command line is wrong (said on stderr). Options.cpp: the one TU with cxxopts.
std::optional<PrinterOptions> parseOptions(int    argc,
                                           char** argv);

}   // namespace uc_log::printer
