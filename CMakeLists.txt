cmake_minimum_required(VERSION 3.28)

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

set(UC_LOG_BUILD_PRINTER
    true
    CACHE BOOL "build the uc_log_printer host tool")

add_subdirectory(rtt)
add_subdirectory(remote_fmt)

add_library(uc_log INTERFACE)
target_include_directories(uc_log INTERFACE src)
target_link_libraries(uc_log INTERFACE rtt::rtt remote_fmt::remote_fmt)

add_library(uc_log::uc_log ALIAS uc_log)

if(UC_LOG_BUILD_PRINTER)

    include(FetchContent)
    FetchContent_Declare(
        cmake_helpers
        GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
        GIT_TAG master)
    FetchContent_MakeAvailable(cmake_helpers)

    if(NOT CMAKE_CROSSCOMPILING)

        project(
            uc_log
            VERSION 0.1.0
            DESCRIPTION "Microcontroller logging system with RTT and Remote FMT"
            HOMEPAGE_URL "https://github.com/dominicpoeschko/uc_log"
            LANGUAGES CXX)

        include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)
        include(${cmake_helpers_SOURCE_DIR}/FindOrFetch.cmake)

        find_package(Threads REQUIRED)

        set(Boost_Components filesystem process asio)

        set(BOOST_INCLUDE_LIBRARIES ${Boost_Components})
        find_or_fetch_package(
            Boost
            CONFIG
            COMPONENTS
            ${Boost_Components}
            DOWNLOAD_URL
            https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
            DOWNLOAD_HASH
            SHA256=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
            GIT_REPOSITORY
            https://github.com/boostorg/boost.git
            GIT_TAG
            boost-1.90.0
            GIT_BRANCH
            master)

        set(FMT_INSTALL OFF)
        find_or_fetch_package(
            fmt
            GIT_REPOSITORY
            https://github.com/fmtlib/fmt.git
            GIT_TAG
            12.1.0
            GIT_BRANCH
            master)

        find_or_fetch_package(
            CLI11
            GIT_REPOSITORY
            https://github.com/CLIUtils/CLI11
            GIT_TAG
            v2.6.1
            GIT_BRANCH
            main)

        set(FTXUI_ENABLE_INSTALL OFF)
        set(FTXUI_QUIET ON)
        find_or_fetch_package(
            ftxui
            GIT_REPOSITORY
            https://github.com/ArthurSonzogni/ftxui
            GIT_TAG
            3330de961ccdbcbc40207b43b4437eb1273677b7
            GIT_BRANCH
            main)

        find_or_fetch_package(
            magic_enum
            GIT_REPOSITORY
            https://github.com/Neargye/magic_enum
            GIT_TAG
            v0.9.7
            GIT_BRANCH
            master)

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
        include(${cmake_helpers_SOURCE_DIR}/HostBuild.cmake)
        configure_host_build(uc_log_printer)
    endif()

endif() # UC_LOG_BUILD_PRINTER

function(target_add_uc_log_rtt_jlink targetname)
    set(options VERBOSE)
    set(oneValueArgs
        TARGET_MPU
        SWD_SPEED
        JLINK_IP
        CHANNELS
        MAP_FILE
        PREFIX
        METRICS_PORT
        HEX_FILE)
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

    if(NOT PARSED_ARGS_METRICS_PORT)
        set(PARSED_ARGS_METRICS_PORT 34565)
    elseif(${PARSED_ARGS_METRICS_PORT} LESS 1024 OR PARSED_ARGS_METRICS_PORT GREATER 65535)
        message(WARNING "METRICS_PORT ${PARSED_ARGS_METRICS_PORT} may be out of valid range (1024-65535)")
    endif()

    target_generate_string_constants(${targetname})
    target_generate_ssproj(${targetname})

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
        --metrics_port
        ${PARSED_ARGS_METRICS_PORT}
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
        ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname})

    if(PARSED_ARGS_JLINK_IP AND NOT PARSED_ARGS_JLINK_IP STREQUAL "")
        set(_val "${PARSED_ARGS_JLINK_IP}")
        # Expand $ENV{VAR} if that's what the string is
        if(_val MATCHES "^\\$ENV\\{([^}]+)\\}$")
            set(_env_name "${CMAKE_MATCH_1}")
            set(JLINK_IP_EXPANDED "$ENV{${_env_name}}")
        else()
            set(JLINK_IP_EXPANDED "${_val}")
        endif()

        list(APPEND command --host ${JLINK_IP_EXPANDED})
    endif()

    add_custom_target(
        ${PARSED_ARGS_PREFIX}${targetname}
        COMMENT "start logging ${targetname}"
        USES_TERMINAL
        COMMAND ${command}
        DEPENDS ${targetname} uc_log_printer ${uc_log_printer_bin})

    add_dependencies(${targetname} uc_log_printer)

endfunction()

function(target_generate_ssproj targetname)
    get_target_property(bin_dir ${targetname} BINARY_DIR)

    find_package(
        Python3
        COMPONENTS Interpreter
        REQUIRED)
    set(command ${Python3_EXECUTABLE} ${uc_log_dir}/tools/generate_ssproj.py --input
                ${bin_dir}/${targetname}_string_constants.json --output ${bin_dir}/${targetname}.ssproj)

    add_custom_command(
        TARGET ${targetname}
        POST_BUILD
        COMMAND ${command}
        BYPRODUCTS ${bin_dir}/${targetname}.ssproj)

    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY LINK_DEPENDS ${uc_log_dir}/tools/generate_ssproj.py)
    set_property(
        TARGET ${targetname}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${bin_dir}/${targetname}.ssproj)
endfunction()
