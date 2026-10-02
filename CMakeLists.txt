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

# project() is only called on the standalone/printer path below, so top-level detection has to compare directories here
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(uc_log_is_top_level true)
else()
    set(uc_log_is_top_level false)
endif()
set(UC_LOG_BUILD_TESTS
    ${uc_log_is_top_level}
    CACHE BOOL "build the unit tests (ctest)")

set(UC_LOG_BUILD_PRINTER
    true
    CACHE BOOL "build the uc_log_printer host tool")

# cmake_helpers applies USE_STD_LIB / USE_STDLIB_DEBUG per target, but the fetched dependencies (fmt via remote_fmt
# below, ftxui, boost, ...) are compiled as static libraries and must share the same stdlib and debug-container ABI or
# the final link mixes incompatible symbols. This has to happen before ANY add_subdirectory that can fetch a dependency
# (remote_fmt fetches fmt), and requires the dependencies to actually be built in this tree (USE_FORCE_FETCH), not found
# prebuilt.
if(NOT CMAKE_CROSSCOMPILING)
    if(USE_STDLIB_DEBUG)
        if("${USE_STD_LIB}" STREQUAL "libc++")
            add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>)
        else()
            add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_DEBUG>
                                    $<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_DEBUG_PEDANTIC>)
        endif()
    endif()

    # the compiler id is not known yet this early (before project()), so the Clang condition lives in generator
    # expressions instead of an if()
    if("${USE_STD_LIB}" STREQUAL "libc++")
        add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:-stdlib=libc++>)
        add_link_options($<$<CXX_COMPILER_ID:Clang>:-fuse-ld=lld> $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)
    endif()
endif()

include(FetchContent)
FetchContent_Declare(
    cmake_helpers
    GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
    GIT_TAG master)
FetchContent_MakeAvailable(cmake_helpers)

include(${cmake_helpers_SOURCE_DIR}/FindOrFetch.cmake)

add_subdirectory(rtt)
add_subdirectory(remote_fmt)

# Supplies the remote_fmt formatter for reflectable structs. On the interface library, not the printer: the struct is
# formatted on the microcontroller into an @TYPENAME(...) marker the printer already parses. Also owns glaze (version
# and patch); never link glaze::glaze by name, it may be an imported target scoped to aglio's directory.
find_or_fetch_package(
    aglio
    GIT_REPOSITORY
    https://github.com/dominicpoeschko/aglio.git
    GIT_TAG
    v0.2.6
    GIT_BRANCH
    master)

add_library(uc_log INTERFACE)
target_include_directories(uc_log INTERFACE src)
target_link_libraries(uc_log INTERFACE rtt::rtt remote_fmt::remote_fmt aglio::aglio)

add_library(uc_log::uc_log ALIAS uc_log)

if(UC_LOG_BUILD_PRINTER)

    if(NOT CMAKE_CROSSCOMPILING)

        project(
            uc_log
            VERSION 0.1.0
            DESCRIPTION "Microcontroller logging system with RTT and Remote FMT"
            HOMEPAGE_URL "https://github.com/dominicpoeschko/uc_log"
            LANGUAGES CXX)

        include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # -Weverything enables -Wthread-safety-negative, which requires negative capability annotations on every
            # function that takes a lock; plain std::mutex code cannot satisfy it without annotating the whole codebase
            add_compile_options(-Wno-thread-safety-negative)
        endif()

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND "${USE_SANITIZER}" STREQUAL "thread")
            # boost::asio's fenced blocks use std::atomic_thread_fence, which TSan does not model; gcc warns about every
            # inlined use (also at LTO link time). Harmless noise - TSan still checks all mutex/atomic-object
            # operations.
            add_compile_options(-Wno-tsan)
            add_link_options(-Wno-tsan)
        endif()

        find_package(Threads REQUIRED)

        set(Boost_Components filesystem process asio)

        set(BOOST_INCLUDE_LIBRARIES ${Boost_Components})
        find_or_fetch_package(
            Boost
            CONFIG
            COMPONENTS
            ${Boost_Components}
            DOWNLOAD_URL
            https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
            DOWNLOAD_HASH
            SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
            GIT_REPOSITORY
            https://github.com/boostorg/boost.git
            GIT_TAG
            boost-1.91.0-1
            GIT_BRANCH
            master)

        if(TARGET boost_container AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # libc++ 23 opens namespace std with a '#pragma clang attribute push' that applies to functions only;
            # boost/container/detail/std_fwd.hpp forward-declares just class templates in it, so clang warns three times
            # per boost_container source. Boost's own code, not ours: silence it there.
            target_compile_options(boost_container PRIVATE -Wno-pragma-clang-attribute)
        endif()

        set(FMT_INSTALL OFF)
        find_or_fetch_package(
            fmt
            GIT_REPOSITORY
            https://github.com/fmtlib/fmt.git
            GIT_TAG
            12.2.0
            GIT_BRANCH
            master)

        find_or_fetch_package(
            cxxopts
            GIT_REPOSITORY
            https://github.com/jarro2783/cxxopts
            GIT_TAG
            v3.3.1
            GIT_BRANCH
            master)

        set(FTXUI_ENABLE_INSTALL OFF)
        set(FTXUI_QUIET ON)
        find_or_fetch_package(
            ftxui
            GIT_REPOSITORY
            https://github.com/ArthurSonzogni/ftxui
            GIT_TAG
            v7.0.3
            GIT_BRANCH
            main)

        find_or_fetch_package(
            enchantum
            GIT_REPOSITORY
            https://github.com/ZXShady/enchantum
            GIT_TAG
            0.4.0
            GIT_BRANCH
            main)

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
                    cxxopts::cxxopts
                    atomic
                    ${Boost_Components}
                    ftxui::screen
                    ftxui::dom
                    ftxui::component
                    enchantum::enchantum)

        target_compile_definitions(uc_log_printer PRIVATE CXXOPTS_NO_RTTI)
        target_add_default_build_options(uc_log_printer PRIVATE)

        if(${UC_LOG_BUILD_TEST_GUI})
            add_executable(uc_log_gui_test src/uc_log/gui_test.cpp)
            target_link_libraries(
                uc_log_gui_test
                remote_fmt::parser
                fmt::fmt
                Threads::Threads
                uc_log::uc_log
                cxxopts::cxxopts
                ${Boost_Components}
                ftxui::screen
                ftxui::dom
                ftxui::component
                enchantum::enchantum)

            target_add_default_build_options(uc_log_gui_test PUBLIC)
        endif()

        if(${UC_LOG_BUILD_TESTS})
            enable_testing()
            add_subdirectory(tests)
        endif()
    else()
        include(${cmake_helpers_SOURCE_DIR}/HostBuild.cmake)
        # The printer is built from the aglio the target is built from. Forced into the host tree: a redirection
        # (FETCHCONTENT_SOURCE_DIR_AGLIO) the parent has dropped would otherwise stay in the host tree's cache.
        configure_host_build(uc_log_printer FORWARD_VARS FETCHCONTENT_SOURCE_DIR_AGLIO)
        # configure_host_build() makes a plain custom target, which is excluded from ALL. No firmware target drags the
        # printer in any more, so put it in ALL here to keep a bare `cmake --build .` building the tool.
        set_property(TARGET uc_log_printer PROPERTY EXCLUDE_FROM_ALL FALSE)
    endif()

endif() # UC_LOG_BUILD_PRINTER

function(target_add_uc_log_rtt_jlink targetname)
    # RAM_IMAGE: the image lives in RAM (Kvasir's RAM_ONLY): the printer's flash and reset load it and start it at its
    # vector table instead of resetting into the boot ROM (--ram_image)
    set(options VERBOSE RAM_IMAGE)
    set(oneValueArgs
        LOG_FILTER
        TARGET_MPU
        SWD_SPEED
        JLINK_IP
        JLINK_PROBE
        MAP_FILE
        PREFIX
        CONTROL_PORT
        METRICS_PORT
        DUPLEX_BASE_PORT
        TRANSPORT
        HEX_FILE)
    # PRE_RESET_COMMANDS: `w4 <address> <value>` lines written before every reset and download (e.g. RP core 1 parking).
    set(multiValueArgs PRE_RESET_COMMANDS)

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

    if(NOT PARSED_ARGS_MAP_FILE)
        set(PARSED_ARGS_MAP_FILE "${targetname}.map")
    endif()

    if(NOT PARSED_ARGS_HEX_FILE)
        set(PARSED_ARGS_HEX_FILE "${targetname}_flash.hex")
    endif()

    if(NOT PARSED_ARGS_PREFIX)
        set(PARSED_ARGS_PREFIX "log_")
    endif()

    if(PARSED_ARGS_METRICS_PORT)
        message(DEPRECATION "target_add_uc_log_rtt_jlink: METRICS_PORT is deprecated, use CONTROL_PORT")
        if(NOT PARSED_ARGS_CONTROL_PORT)
            set(PARSED_ARGS_CONTROL_PORT ${PARSED_ARGS_METRICS_PORT})
        endif()
    endif()
    if(NOT PARSED_ARGS_CONTROL_PORT)
        set(PARSED_ARGS_CONTROL_PORT 34565)
    elseif(${PARSED_ARGS_CONTROL_PORT} LESS 1024 OR PARSED_ARGS_CONTROL_PORT GREATER 65535)
        message(WARNING "CONTROL_PORT ${PARSED_ARGS_CONTROL_PORT} may be out of valid range (1024-65535)")
    endif()

    if(NOT PARSED_ARGS_DUPLEX_BASE_PORT)
        set(PARSED_ARGS_DUPLEX_BASE_PORT 34600)
    elseif(${PARSED_ARGS_DUPLEX_BASE_PORT} LESS 1024 OR PARSED_ARGS_DUPLEX_BASE_PORT GREATER 65535)
        message(WARNING "DUPLEX_BASE_PORT ${PARSED_ARGS_DUPLEX_BASE_PORT} may be out of valid range (1024-65535)")
    endif()

    # unix: sockets in the log directory, the ports unused; tcp: CONTROL_PORT and DUPLEX_BASE_PORT + n.
    if(NOT PARSED_ARGS_TRANSPORT)
        set(PARSED_ARGS_TRANSPORT unix)
    elseif(NOT PARSED_ARGS_TRANSPORT MATCHES "^(unix|tcp)$")
        message(FATAL_ERROR "target_add_uc_log_rtt_jlink: TRANSPORT is unix or tcp, not ${PARSED_ARGS_TRANSPORT}")
    endif()

    target_generate_string_constants(${targetname})

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
        --device
        ${PARSED_ARGS_TARGET_MPU}
        --speed
        ${PARSED_ARGS_SWD_SPEED}
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

    if(PARSED_ARGS_JLINK_PROBE AND NOT PARSED_ARGS_JLINK_PROBE STREQUAL "")
        list(APPEND command --probe "${PARSED_ARGS_JLINK_PROBE}")
    endif()

    foreach(pre_reset_command IN LISTS PARSED_ARGS_PRE_RESET_COMMANDS)
        list(APPEND command --pre_reset_command "${pre_reset_command}")
    endforeach()

    if(PARSED_ARGS_RAM_IMAGE)
        list(APPEND command --ram_image)
    endif()

    # the compile-time log filter the firmware was built with (LogFilter.hpp), for the printer to show
    if(PARSED_ARGS_LOG_FILTER)
        list(APPEND command --log_filter "${PARSED_ARGS_LOG_FILTER}")
    endif()

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
