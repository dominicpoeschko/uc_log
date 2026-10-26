# The fixture of uc_log_test_printer_args: target_add_uc_log_rtt_jlink (cmake/RttJlink.cmake, included alone) called
# with every keyword, with none but the required one, and with values that leave their options out. check.cmake
# configures it and compares each <target>.printer_args with golden/.
cmake_minimum_required(VERSION 3.28)
project(uc_log_printer_args_fixture NONE)

# remote_fmt's catalog step and the printer host build are not part of what is checked here
function(target_generate_string_constants)

endfunction()
set(UC_LOG_BUILD_PRINTER OFF)

include(${UC_LOG_RTT_JLINK_CMAKE})

add_custom_target(every_keyword)
target_add_uc_log_rtt_jlink(
    every_keyword
    VERBOSE
    TARGET_MPU
    RP2350_M33_0
    SWD_SPEED
    4000
    JLINK_IP
    192.0.2.7
    JLINK_PROBE
    "probe a"
    MAP_FILE
    app.map
    HEX_FILE
    app_flash.hex
    PREFIX
    show_
    CONTROL_PORT
    40000
    DUPLEX_BASE_PORT
    40100
    TRANSPORT
    tcp
    PRE_RESET_COMMANDS
    "w4 0x4001A004 0x01000000"
    "w4 0x4001B004 0x01000000"
    FLASH_UNCACHED_OFFSET
    0x04000000
    PARK_SIGNATURE
    "0x40010008,0x100000,0x100000,0x2000"
    STALL_SIGNATURE
    "0x400d0000,0x3,0x3+0x400d0004,0x1,0x0"
    STALL_RECOVERY
    "w4 0x400d801c 0x00000000"
    "w4 0x400d8020 0x00000001"
    FLASH_READY_SIGNATURE
    "0x400d0000,0x5,0x0"
    FLASH_WINDOW
    "0x10000000,0x04000000"
    RAM_IMAGE
    LOG_FILTER
    /src/uc_log_filter.txt)

add_custom_target(required_only)
target_add_uc_log_rtt_jlink(required_only TARGET_MPU ATSAMD21G18A)

# an empty value and a CMake false value (0) leave their option out
add_custom_target(empty_values)
target_add_uc_log_rtt_jlink(
    empty_values
    TARGET_MPU
    RP2040_M0_0
    JLINK_IP
    ""
    JLINK_PROBE
    ""
    FLASH_UNCACHED_OFFSET
    0
    PARK_SIGNATURE
    ""
    LOG_FILTER
    "")
