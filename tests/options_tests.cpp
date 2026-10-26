// printer/Options.hpp: a firmware tree's .printer_args line (cmake/RttJlink.cmake's printer_option_table) parses,
// the signatures are read, a malformed one is refused, and the defaults are the documented ones.
#include "uc_log/printer/Options.hpp"

#include "support/Check.hpp"

#include <string>
#include <vector>

namespace {

std::optional<uc_log::printer::PrinterOptions> parse(std::vector<std::string> args) {
    args.insert(args.begin(), "uc_log_printer");
    std::vector<char*> argv;
    argv.reserve(args.size());
    for(auto& a : args) { argv.push_back(a.data()); }
    return uc_log::printer::parseOptions(static_cast<int>(argv.size()), argv.data());
}

// the shape of an RP2350 tree's <target>.printer_args, one argument per line there
std::vector<std::string> printerArgs() {
    return {"--device",
            "RP2350_M33_0",
            "--speed",
            "100000",
            "--map_file",
            "app_debug.map",
            "--hex_file",
            "app_debug_flash.hex",
            "--string_constants_file",
            "app_debug_string_constants.json",
            "--probe",
            "probe a",
            "--pre_reset_command",
            "w4 0x4001A004 0x01000000",
            "--pre_reset_command",
            "w4 0x4001B004 0x01000000",
            "--flash_uncached_offset",
            "0x04000000",
            "--stall_signature",
            "0x400d0000,0x3,0x3",
            "--stall_recovery",
            "w4 0x400d801c 0x00000000",
            "--flash_ready_signature",
            "0x400d0000,0x5,0x0",
            "--build_command",
            "true",
            "--log_dir",
            "log"};
}

void firmwareTreeLine() {
    auto const o = parse(printerArgs());
    CHECK(o.has_value(), "parses");
    if(!o) { return; }
    CHECK(o->device == "RP2350_M33_0" && o->speed == 100000 && o->probe == "probe a"
            && o->mapFile == "app_debug.map" && o->logDir == "log",
          "the plain options");
    CHECK(o->preResetCommands.size() == 2 && o->preResetCommands[0].address == 0x4001'A004U
            && o->preResetCommands[0].value == 0x0100'0000U,
          "the pre-reset writes");
    CHECK(o->flashUncachedOffset == 0x0400'0000U, "the uncached offset, in hex");
    CHECK(o->stallSignature && o->stallSignature->conditions.size() == 1
            && o->stallSignature->conditions[0].address == 0x400d'0000U
            && o->stallSignature->conditions[0].mask == 3
            && o->stallSignature->recovery.size() == 1,
          "the stall signature and its recovery");
    CHECK(o->flashReady.size() == 1 && o->flashReady[0].mask == 5, "the flash-ready signature");
}

void defaults() {
    auto const o = parse(printerArgs());
    if(!o) { return; }
    CHECK(o->unixTransport && o->controlSocket == "auto" && o->controlPort == 34565
            && o->duplexBasePort == 34600 && o->controlHistoryMb == 128,
          "control socket defaults");
    CHECK(!o->disableUi && !o->ramImage && o->host.empty() && !o->parkSignature && !o->flashWindow,
          "the rest off");
}

void malformed() {
    auto args = printerArgs();
    args.insert(args.end(), {"--stall_signature", "0x400d0000,0x3"});
    CHECK(!parse(args), "a stall signature of two words is refused");
    args = printerArgs();
    args.insert(args.end(), {"--park_signature", "1,2,x"});
    CHECK(!parse(args), "a word that is no hex number is refused");
    args = printerArgs();
    args.insert(args.end(), {"--flash_window", "0x10000000"});
    CHECK(!parse(args), "a flash window without its size is refused");
    args = printerArgs();
    args.insert(args.end(), {"--flash_window", "0x10000000,0x400000,0x1"});
    CHECK(!parse(args), "a flash window with a word too many is refused, not cut short");
    args = printerArgs();
    args.insert(args.end(), {"--flash_uncached_offset", "0x3000000,0x1"});
    CHECK(!parse(args), "an offset with a second word is refused");
    // recovery lines without a stall signature would undo nothing: refused, well-formed or not
    args = {"--device",
            "RP2350_M33_0",
            "--speed",
            "100000",
            "--map_file",
            "a.map",
            "--hex_file",
            "a.hex",
            "--string_constants_file",
            "a.json",
            "--build_command",
            "true",
            "--log_dir",
            "log"};
    CHECK(parse(args).has_value(), "the line without stall options parses");
    auto noSignature = args;
    noSignature.insert(noSignature.end(), {"--stall_recovery", "w4 0x400d801c 0x00000000"});
    CHECK(!parse(noSignature), "a stall recovery without a stall signature is refused");
    noSignature = args;
    noSignature.insert(noSignature.end(), {"--stall_recovery", "w4"});
    CHECK(!parse(noSignature), "a malformed stall recovery without a stall signature is refused");
    noSignature = args;
    noSignature.insert(noSignature.end(),
                       {"--stall_signature", "0x400d0000,0x3,0x3", "--stall_recovery", "w4"});
    CHECK(!parse(noSignature), "a malformed stall recovery with its signature is refused");
    args = printerArgs();
    args.insert(args.end(), {"--park_signature", "0x40000000,0xff,0x1,0x2000,0x5"});
    CHECK(!parse(args), "a park signature of five words is refused");
    args = printerArgs();
    args.insert(args.end(), {"--park_signature", "0x40000000,0xff,0x1,0x2000"});
    auto const park = parse(args);
    CHECK(park && park->parkSignature && park->parkSignature->pc == 0x2000U,
          "a park signature with its pc");
}

// --flash_uncached_offset is a hex word like every address of the command line (it went through std::stoul base 0
// before: "4000000" was decimal there); the other refusals say their option and stop the start.
void offsetAndRefusals() {
    auto args = printerArgs();
    args.insert(args.end(), {"--flash_uncached_offset", "4000000"});
    auto const bare = parse(args);
    CHECK(bare && bare->flashUncachedOffset == 0x0400'0000U, "an offset without 0x is hex");
    args = printerArgs();
    args.insert(args.end(), {"--flash_uncached_offset", "0x1g"});
    CHECK(!parse(args), "an offset that is no hex word is refused");
    args = printerArgs();
    args.insert(args.end(), {"--transport", "udp"});
    CHECK(!parse(args), "a transport other than unix or tcp is refused");
    args = printerArgs();
    args.insert(args.end(), {"--transport", "tcp", "--bind_address", "not-an-address"});
    auto const tcp = parse(args);
    CHECK(!tcp, "a bind address that is none is refused");
    args = printerArgs();
    args.insert(args.end(),
                {"--transport", "tcp", "--bind_address", "0.0.0.0", "--ram_image", "--disable_ui"});
    auto const any = parse(args);
    CHECK(any && !any->unixTransport && any->bindAddress.to_string() == "0.0.0.0" && any->ramImage
            && any->disableUi,
          "tcp on every address, the two flags");
    args = printerArgs();
    args.erase(args.begin(), args.begin() + 2);   // no --device
    CHECK(!parse(args), "a required option left out is refused");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               { "firmwareTreeLine",  firmwareTreeLine},
                               {         "defaults",          defaults},
                               {        "malformed",         malformed},
                               {"offsetAndRefusals", offsetAndRefusals},
    });
}
