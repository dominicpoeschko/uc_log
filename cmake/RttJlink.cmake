# target_add_uc_log_rtt_jlink(<target> TARGET_MPU <device> ...): a firmware target's printer - its options in
# <target>.printer_args (one argument per line, what kvasir_bench and every other printer start read) and, with
# UC_LOG_BUILD_PRINTER, the log_<target> custom target that runs it. Included by uc_log's CMakeLists; the ctest
# uc_log_test_printer_args includes it alone into a small project (tests/printer_args) and compares the file with a
# golden.

# The SWD speed (kHz) of a target that names none: the J-Link DLL clamps it to the probe's maximum. The printer's
# (target_add_uc_log_rtt_jlink) and the SDK's JLinkExe scripts (Kvasir_SDK cmake/jlink.cmake); a cache value like the
# one below.
set(UC_LOG_SWD_DEFAULT_KHZ
    100000
    CACHE INTERNAL "")

# A log line's string starts ("<file>", <line>, ...: the line number is not part of its id, so a line that moves keeps
# its id (and identical lines in one function sit in neighbouring slots, ordered by line). The ID_KEY_STRIP of every
# uc_log target (target_add_uc_log_rtt_jlink and the catalog tests); a cache value, so the function sees it in its
# caller's directory.
set(UC_LOG_ID_KEY_STRIP
    [[^\("[^"]*", ([0-9]+, )]]
    CACHE INTERNAL "")

function(target_add_uc_log_rtt_jlink targetname)
    # The printer options this function writes, one row each: "<keyword> <option> <kind>", in the order they stand in
    # <target>.printer_args. kind: `value` - one value, left out when empty (or a CMake false value such as 0); `list` -
    # the option once per element, in order; `flag` - the option alone when the keyword is given. A new printer option
    # for a target goes here and nowhere else (and into the golden of tests/printer_args).
    set(printer_option_table
        # the network J-Link's address, or a probe by serial number or nickname
        "JLINK_IP --host value"
        "JLINK_PROBE --probe value"
        # `w4 <address> <value>` lines written before every reset and download (e.g. RP core 1 parking)
        "PRE_RESET_COMMANDS --pre_reset_command list"
        # <offset>: the firmware check reads the flash through its uncached alias (address + offset)
        "FLASH_UNCACHED_OFFSET --flash_uncached_offset value"
        # "<address>,<mask>,<value>": the chip's sign of a boot ROM park after a debug access during its reboot (the
        # printer then resets it)
        "PARK_SIGNATURE --park_signature value"
        # "<address>,<mask>,<value>" and "w4 <a> <v>"...: a stall the chip package can recognise while the target is
        # quiet, and the register writes that undo it
        "STALL_SIGNATURE --stall_signature value"
        "STALL_RECOVERY --stall_recovery list"
        # "<address>,<mask>,<value>[+...]": the register words of a mapped flash - the firmware check reads the flash
        # only then, never while a boot ROM drives it by hand
        "FLASH_READY_SIGNATURE --flash_ready_signature value"
        # "<base>,<size>": control-socket reads there go through the same gate
        "FLASH_WINDOW --flash_window value"
        # the image lives in RAM (Kvasir's RAM_ONLY): the printer's flash and reset load it and start it at its vector
        # table instead of resetting into the boot ROM
        "RAM_IMAGE --ram_image flag"
        # the compile-time log filter the firmware was built with (LogFilter.hpp), for the printer to show
        "LOG_FILTER --log_filter value")

    set(options VERBOSE)
    set(oneValueArgs
        TARGET_MPU
        SWD_SPEED
        MAP_FILE
        PREFIX
        CONTROL_PORT
        DUPLEX_BASE_PORT
        TRANSPORT
        HEX_FILE)
    set(multiValueArgs)
    foreach(row IN LISTS printer_option_table)
        string(REPLACE " " ";" row "${row}")
        list(GET row 0 keyword)
        list(GET row 2 kind)
        if(kind STREQUAL "flag")
            list(APPEND options ${keyword})
        elseif(kind STREQUAL "value")
            list(APPEND oneValueArgs ${keyword})
        elseif(kind STREQUAL "list")
            list(APPEND multiValueArgs ${keyword})
        else()
            message(FATAL_ERROR "target_add_uc_log_rtt_jlink: printer_option_table row ${row}: unknown kind ${kind}")
        endif()
    endforeach()

    cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "${options}" "${oneValueArgs}" "${multiValueArgs}")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "target_add_uc_log_rtt_jlink: Unknown arguments: ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_TARGET_MPU)
        message(FATAL_ERROR "target_add_uc_log_rtt_jlink: TARGET_MPU is required")
    endif()

    if(NOT TARGET ${targetname})
        message(FATAL_ERROR "target_add_uc_log_rtt_jlink: Target '${targetname}' does not exist")
    endif()

    if(NOT PARSED_ARGS_SWD_SPEED)
        set(PARSED_ARGS_SWD_SPEED ${UC_LOG_SWD_DEFAULT_KHZ})
    elseif(${PARSED_ARGS_SWD_SPEED} LESS 1000 OR ${PARSED_ARGS_SWD_SPEED} GREATER 50000000)
        message(WARNING "SWD_SPEED ${PARSED_ARGS_SWD_SPEED} may be out of valid range (1000-50000000)")
    endif()

    if(NOT PARSED_ARGS_MAP_FILE)
        set(PARSED_ARGS_MAP_FILE "${targetname}.map")
    endif()

    if(NOT PARSED_ARGS_HEX_FILE)
        set(PARSED_ARGS_HEX_FILE "${targetname}_flash.hex")
    endif()

    if(NOT PARSED_ARGS_PREFIX)
        set(PARSED_ARGS_PREFIX "log_")
    endif()

    if(NOT PARSED_ARGS_CONTROL_PORT)
        set(PARSED_ARGS_CONTROL_PORT 34565)
    elseif(${PARSED_ARGS_CONTROL_PORT} LESS 1024 OR ${PARSED_ARGS_CONTROL_PORT} GREATER 65535)
        message(WARNING "CONTROL_PORT ${PARSED_ARGS_CONTROL_PORT} may be out of valid range (1024-65535)")
    endif()

    if(NOT PARSED_ARGS_DUPLEX_BASE_PORT)
        set(PARSED_ARGS_DUPLEX_BASE_PORT 34600)
    elseif(${PARSED_ARGS_DUPLEX_BASE_PORT} LESS 1024 OR ${PARSED_ARGS_DUPLEX_BASE_PORT} GREATER 65535)
        message(WARNING "DUPLEX_BASE_PORT ${PARSED_ARGS_DUPLEX_BASE_PORT} may be out of valid range (1024-65535)")
    endif()

    # unix: sockets in the log directory, the ports unused; tcp: CONTROL_PORT and DUPLEX_BASE_PORT + n.
    if(NOT PARSED_ARGS_TRANSPORT)
        set(PARSED_ARGS_TRANSPORT unix)
    elseif(NOT PARSED_ARGS_TRANSPORT MATCHES "^(unix|tcp)$")
        message(FATAL_ERROR "target_add_uc_log_rtt_jlink: TRANSPORT is unix or tcp, not ${PARSED_ARGS_TRANSPORT}")
    endif()

    target_generate_string_constants(${targetname} ID_KEY_STRIP "${UC_LOG_ID_KEY_STRIP}")

    # What a printer needs to know about this target - the chip, the probe, the image's files, the chip package's reset
    # commands and signatures - in one list: the log_ target below passes it, and <target>.printer_args holds it for
    # every other way a printer starts, one argument per line as it stands in argv. The options beyond these five come
    # from printer_option_table above.
    set(printer_args
        --device
        ${PARSED_ARGS_TARGET_MPU}
        --speed
        ${PARSED_ARGS_SWD_SPEED}
        --map_file
        ${PARSED_ARGS_MAP_FILE}
        --hex_file
        ${PARSED_ARGS_HEX_FILE}
        --string_constants_file
        "${targetname}_string_constants.json")

    foreach(row IN LISTS printer_option_table)
        string(REPLACE " " ";" row "${row}")
        list(GET row 0 keyword)
        list(GET row 1 option)
        list(GET row 2 kind)
        if(kind STREQUAL "list")
            foreach(value IN LISTS PARSED_ARGS_${keyword})
                list(APPEND printer_args ${option} "${value}")
            endforeach()
        elseif(PARSED_ARGS_${keyword})
            if(kind STREQUAL "flag")
                list(APPEND printer_args ${option})
            else()
                list(APPEND printer_args ${option} "${PARSED_ARGS_${keyword}}")
            endif()
        endif()
    endforeach()

    list(JOIN printer_args "\n" printer_args_text)
    file(
        GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${targetname}.printer_args"
        CONTENT "${printer_args_text}\n")

    if(NOT UC_LOG_BUILD_PRINTER)
        return()
    endif()

    get_target_property(build_dir uc_log_printer BINARY_DIR)
    if(NOT build_dir)
        message(FATAL_ERROR "Could not determine uc_log_printer binary directory")
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        set(uc_log_printer_bin "${build_dir}/uc_log_printer")
    else()
        get_target_property(uc_log_printer_bin uc_log_printer HOST_EXECUTABLE_PATH)
    endif()

    set(log_base_dir "${CMAKE_CURRENT_BINARY_DIR}/rtt_log")
    set(log_target_dir "${log_base_dir}/${targetname}")

    file(MAKE_DIRECTORY "${log_base_dir}")
    file(MAKE_DIRECTORY "${log_target_dir}")

    if(PARSED_ARGS_VERBOSE)
        message(STATUS "Log directory created: ${log_target_dir}")
    endif()

    set(command
        ${uc_log_printer_bin}
        --control_port
        ${PARSED_ARGS_CONTROL_PORT}
        --duplex_base_port
        ${PARSED_ARGS_DUPLEX_BASE_PORT}
        --transport
        ${PARSED_ARGS_TRANSPORT}
        ${printer_args}
        --build_command
        "cmake --build . --target=${targetname}"
        --log_dir
        ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname})

    add_custom_target(
        ${PARSED_ARGS_PREFIX}${targetname}
        COMMENT "start logging ${targetname}"
        USES_TERMINAL
        COMMAND ${command}
        VERBATIM)

    # The printer is a host tool the firmware never uses, so the dependency belongs on the log_ target, not on the elf -
    # hanging it on the elf made every firmware build walk the whole nested host build (boost, ftxui, fmt, jlink).
    # Target-level, not a DEPENDS on ${uc_log_printer_bin}: the host-build shim is a custom target that always runs, so
    # a file dependency buys nothing and breaks parsing whenever the binary is not on disk yet.
    add_dependencies(${PARSED_ARGS_PREFIX}${targetname} ${targetname} uc_log_printer)

endfunction()
