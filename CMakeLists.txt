cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

set(uc_log_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

set(uc_log_cmake_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

if(TARGET uc_log_printer)
    return()
endif()

set(UC_LOG_BUILD_TEST_GUI
    false
    CACHE BOOL "build the test gui")

add_subdirectory(rtt)
add_subdirectory(remote_fmt)

add_library(uc_log INTERFACE)
target_include_directories(uc_log INTERFACE src)
target_link_libraries(uc_log INTERFACE rtt::rtt remote_fmt::remote_fmt)

add_library(uc_log::uc_log ALIAS uc_log)

if(NOT CMAKE_CROSSCOMPILING)

    project(
        uc_log
        VERSION 0.1.0
        DESCRIPTION "Microcontroller logging system with RTT and Remote FMT"
        HOMEPAGE_URL "https://github.com/dominicpoeschko/uc_log"
        LANGUAGES CXX)

    include(cmake_helpers/BuildOptions.cmake)
    include(cmake_helpers/FindOrFetch.cmake)

    find_package(Threads REQUIRED)

    set(Boost_Components system filesystem process)

    set(BOOST_INCLUDE_LIBRARIES ${Boost_Components})
    find_or_fetch_package(
        Boost
        CONFIG
        COMPONENTS
        ${Boost_Components}
        GIT_REPOSITORY
        https://github.com/boostorg/boost.git
        GIT_TAG
        master)

    set(FMT_INSTALL OFF)
    find_or_fetch_package(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG master)

    find_or_fetch_package(CLI11 GIT_REPOSITORY https://github.com/CLIUtils/CLI11 GIT_TAG main)

    set(FTXUI_ENABLE_INSTALL OFF)
    set(FTXUI_QUIET ON)
    find_or_fetch_package(ftxui GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui GIT_TAG main)

    find_or_fetch_package(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum GIT_TAG master)

    add_subdirectory(jlink)

    add_executable(uc_log_printer src/uc_log/jlinkPrinter.cpp)

    list(TRANSFORM Boost_Components PREPEND "Boost::")
    target_link_libraries(
        uc_log_printer
        PRIVATE remote_fmt::parser
                fmt::fmt
                Threads::Threads
                uc_log::uc_log
                jlink::jlink
                CLI11::CLI11
                atomic
                ${Boost_Components}
                ftxui::screen
                ftxui::dom
                ftxui::component
                magic_enum::magic_enum)

    target_add_default_build_options(uc_log_printer PRIVATE)

    if(${UC_LOG_BUILD_TEST_GUI})
        add_executable(uc_log_gui_test src/uc_log/gui_test.cpp)
        target_link_libraries(
            uc_log_gui_test
            remote_fmt::parser
            fmt::fmt
            Threads::Threads
            uc_log::uc_log
            CLI11::CLI11
            ${Boost_Components}
            ftxui::screen
            ftxui::dom
            ftxui::component)

        target_add_default_build_options(uc_log_gui_test PUBLIC)
    endif()
else()
    include(cmake_helpers/HostBuild.cmake)
    configure_host_build(uc_log_printer)
endif()

function(target_add_uc_log_rtt_jlink targetname)
    set(options VERBOSE)
    set(oneValueArgs
        TARGET_MPU
        SWD_SPEED
        JLINK_IP
        CHANNELS
        MAP_FILE
        PREFIX
        TRACE_PORT
        HEX_FILE
        GUI_TYPE)
    set(multiValueArgs)

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
        set(PARSED_ARGS_SWD_SPEED 100000)
    elseif(${PARSED_ARGS_SWD_SPEED} LESS 1000 OR ${PARSED_ARGS_SWD_SPEED} GREATER 50000000)
        message(WARNING "SWD_SPEED ${PARSED_ARGS_SWD_SPEED} may be out of valid range (1000-50000000)")
    endif()

    if(NOT PARSED_ARGS_CHANNELS)
        set(PARSED_ARGS_CHANNELS 2)
    elseif(${PARSED_ARGS_CHANNELS} LESS 1 OR ${PARSED_ARGS_CHANNELS} GREATER 16)
        message(WARNING "CHANNELS ${PARSED_ARGS_CHANNELS} may be out of valid range (1-16)")
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

    if(NOT PARSED_ARGS_TRACE_PORT)
        set(PARSED_ARGS_TRACE_PORT 34565)
    elseif(${PARSED_ARGS_TRACE_PORT} LESS 1024 OR PARSED_ARGS_TRACE_PORT GREATER 65535)
        message(WARNING "TRACE_PORT ${PARSED_ARGS_TRACE_PORT} may be out of valid range (1024-65535)")
    endif()

    if(NOT PARSED_ARGS_GUI_TYPE)
        set(PARSED_ARGS_GUI_TYPE "ftxui")
    endif()

    get_target_property(build_dir uc_log_printer BINARY_DIR)
    if(NOT build_dir)
        message(FATAL_ERROR "Could not determine uc_log_printer binary directory")
    endif()

    if(NOT CMAKE_CROSSCOMPILING)
        set(uc_log_printer_bin "${build_dir}/uc_log_printer")
    else()
        set(uc_log_printer_bin "${build_dir}/host_build/uc_log_printer")
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
        --trace_port
        ${PARSED_ARGS_TRACE_PORT}
        --device
        ${PARSED_ARGS_TARGET_MPU}
        --speed
        ${PARSED_ARGS_SWD_SPEED}
        --channels
        ${PARSED_ARGS_CHANNELS}
        --map_file
        ${PARSED_ARGS_MAP_FILE}
        --hex_file
        ${PARSED_ARGS_HEX_FILE}
        --string_constants_file
        "${targetname}_string_constants.json"
        --build_command
        "cmake --build . --target=${targetname}"
        --log_dir
        ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname}
        --gui_type
        ${PARSED_ARGS_GUI_TYPE})

    if(PARSED_ARGS_JLINK_IP AND NOT PARSED_ARGS_JLINK_IP STREQUAL "")
        list(APPEND command --host ${PARSED_ARGS_JLINK_IP})
    endif()

    add_custom_target(
        ${PARSED_ARGS_PREFIX}${targetname}
        COMMENT "start logging ${targetname}"
        USES_TERMINAL
        COMMAND ${command}
        DEPENDS ${targetname} uc_log_printer ${uc_log_printer_bin})

    target_generate_string_constants(${targetname})

    add_dependencies(${targetname} uc_log_printer)

endfunction()
