cmake_minimum_required(VERSION 3.28)

cmake_policy(SET CMP0174 NEW)

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
# below, boost, rtxui, ...) are compiled as static libraries and must share the same stdlib and debug-container ABI or
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

# target_add_uc_log_rtt_jlink(), its defaults (UC_LOG_SWD_DEFAULT_KHZ, UC_LOG_ID_KEY_STRIP) and the table of the printer
# options it writes into <target>.printer_args (a ctest checks that file against a golden)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/RttJlink.cmake)

# Supplies the remote_fmt formatter for reflectable structs. On the interface library, not the printer: the struct is
# formatted on the microcontroller into an @TYPENAME(...) marker the printer already parses. glaze comes first from
# remote_fmt above (its catalog reader; the same version and a copy of aglio's glaze.patch), and aglio takes that one;
# never link glaze::glaze by name here, it may be an imported target scoped to another directory.
find_or_fetch_package(
    aglio
    GIT_REPOSITORY
    https://github.com/dominicpoeschko/aglio.git
    GIT_TAG
    v0.3.4
    GIT_BRANCH
    master)

add_library(uc_log INTERFACE)
target_include_directories(uc_log INTERFACE src)
target_link_libraries(uc_log INTERFACE rtt::rtt remote_fmt::remote_fmt aglio::aglio)

add_library(uc_log::uc_log ALIAS uc_log)

# Host side: the printer's headers (reader, control server, map, catalog, TUI) under their own include root. A firmware
# target links only uc_log::uc_log (src/), so a firmware file that includes a printer header does not compile.
add_library(uc_log_host INTERFACE)
target_include_directories(uc_log_host INTERFACE host)
target_link_libraries(uc_log_host INTERFACE uc_log::uc_log)
add_library(uc_log::host ALIAS uc_log_host)

if(UC_LOG_BUILD_PRINTER)

    if(NOT CMAKE_CROSSCOMPILING)

        project(
            uc_log
            VERSION 0.1.0
            DESCRIPTION "Microcontroller logging system with RTT and Remote FMT"
            HOMEPAGE_URL "https://github.com/dominicpoeschko/uc_log"
            LANGUAGES CXX)

        include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
           AND USE_EXTRA_SANITIZERS
           AND NOT "${USE_SANITIZER}" STREQUAL "none")
            # of the extra integer checks, not these two: libstdc++ wraps unsigned values and changes their sign on
            # purpose (std::mt19937, string_view::compare, std::align), and the reports, written to the terminal, garble
            # the printer's window in a sanitizer build. Signed overflow (undefined), truncation and nullability stay
            # checked; after cmake_helpers' -fsanitize=, so that it wins
            foreach(flags IN ITEMS sanitizer_cxx_flags sanitizer_c_flags sanitizer_linker_flags)
                list(APPEND ${flags} -fno-sanitize=unsigned-integer-overflow,implicit-integer-sign-change)
            endforeach()
        endif()

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
            https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
            DOWNLOAD_HASH
            SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
            GIT_REPOSITORY
            https://github.com/boostorg/boost.git
            GIT_TAG
            boost-1.92.0
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
            12.2.1
            GIT_BRANCH
            master)
        # FMT_USE_LOCALE / FMT_UNICODE as the compiled fmt library has them, in every host translation unit:
        # string_constant/format.hpp defaults both to 0 (targets have no locale), which makes fmt::locale_ref an empty
        # class here but a pointer in the library - an ODR violation, and a "{:L...}" spec reads a garbage locale
        # pointer.
        if(TARGET fmt)
            target_compile_definitions(fmt PUBLIC FMT_USE_LOCALE=1 FMT_UNICODE=1)
        elseif(TARGET fmt::fmt)
            set_property(
                TARGET fmt::fmt
                APPEND
                PROPERTY INTERFACE_COMPILE_DEFINITIONS FMT_USE_LOCALE=1 FMT_UNICODE=1)
        endif()

        find_or_fetch_package(
            cxxopts
            GIT_REPOSITORY
            https://github.com/jarro2783/cxxopts
            GIT_TAG
            v3.3.1
            GIT_BRANCH
            master)

        # the printer's window (host/uc_log/printer/rtx/)
        find_or_fetch_package(
            rtxui
            GIT_REPOSITORY
            https://github.com/ArthurSonzogni/RTXUI
            GIT_TAG
            v0.1.4
            GIT_BRANCH
            main)
        # RTXUI's own sources, not ours: always optimised, whatever the tree's build type - cmake_helpers clears
        # CMAKE_CXX_FLAGS_<CONFIG> and gives -O only to targets with default build options, so a fetched RTXUI was built
        # at -O0, ten times slower per frame. Its sanitizer: below, with the other fetched libraries.
        target_compile_options(rtxui PRIVATE -O2 -w)
        set_target_properties(rtxui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

        find_or_fetch_package(
            enchantum
            GIT_REPOSITORY
            https://github.com/ZXShady/enchantum
            GIT_TAG
            0.5.0
            GIT_BRANCH
            main)

        add_subdirectory(jlink)

        # ASan in the fetched libraries (rtxui, fmt, boost) and the fake J-Link DLL too: cmake_helpers' sanitizer flags
        # are per target and reached only ours, and a test links all of them into one program. With libc++ that split
        # the container annotations - libc++ marks a vector's spare capacity only in instrumented code, the linker keeps
        # one copy of each inline vector function - and the clang+ASan CI entry reported "container-overflow" in RTXUI's
        # Split() at every window start. Theirs get the sanitizer alone, not the undefined / integer /
        # implicit-conversion checks, which would report in code that is not ours. Not TSan: boost::asio's fences, which
        # TSan does not model (the -Wno-tsan above), would then report in its own code.
        if("${USE_SANITIZER}" STREQUAL "address")
            foreach(
                dep IN
                ITEMS rtxui
                      fmt
                      fmt-c
                      boost_atomic
                      boost_container
                      boost_context
                      boost_date_time
                      boost_filesystem
                      boost_process
                      jlink_fake_dll)
                if(TARGET ${dep})
                    get_target_property(dep_imported ${dep} IMPORTED)
                    if(NOT dep_imported)
                        target_compile_options(${dep} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                    endif()
                endif()
            endforeach()
        endif()

        list(TRANSFORM Boost_Components PREPEND "Boost::")

        # the printer in pieces (host/uc_log/printer/): its core services and the J-Link reader, instantiated once, and
        # the window (rtx/); uc_log_printer is main, uc_log_gui_test the window on a fake target
        add_library(uc_log_printer_core STATIC host/uc_log/printer/BuildRunner.cpp host/uc_log/printer/Options.cpp
                                               host/uc_log/printer/PrinterModel.cpp host/uc_log/printer/ReaderJLink.cpp)
        target_link_libraries(
            uc_log_printer_core
            PUBLIC remote_fmt::parser
                   fmt::fmt
                   Threads::Threads
                   uc_log::host
                   jlink::jlink
                   atomic
                   ${Boost_Components}
                   enchantum::enchantum
            PRIVATE cxxopts::cxxopts)
        target_compile_definitions(uc_log_printer_core PRIVATE CXXOPTS_NO_RTTI)
        target_add_default_build_options(uc_log_printer_core PRIVATE)

        add_library(
            uc_log_rtxui STATIC
            host/uc_log/printer/rtx/App.cpp
            host/uc_log/printer/rtx/AppCommands.cpp
            host/uc_log/printer/rtx/AppDialog.cpp
            host/uc_log/printer/rtx/AppFilter.cpp
            host/uc_log/printer/rtx/AppHeads.cpp
            host/uc_log/printer/rtx/AppKeys.cpp
            host/uc_log/printer/rtx/AppMetrics.cpp
            host/uc_log/printer/rtx/AppMouse.cpp
            host/uc_log/printer/rtx/AppPages.cpp
            host/uc_log/printer/rtx/AppPanels.cpp
            host/uc_log/printer/rtx/AppQuestions.cpp
            host/uc_log/printer/rtx/ContextMenu.cpp
            host/uc_log/printer/rtx/Dialog.cpp
            host/uc_log/printer/rtx/Form.cpp
            host/uc_log/printer/rtx/Gui.cpp
            host/uc_log/printer/rtx/HeadFit.cpp
            host/uc_log/printer/rtx/LogView.cpp
            host/uc_log/printer/rtx/ModeLine.cpp
            host/uc_log/printer/rtx/Panel.cpp
            host/uc_log/printer/rtx/PrinterActs.cpp
            host/uc_log/printer/rtx/Runs.cpp
            host/uc_log/printer/rtx/StatusLine.cpp
            host/uc_log/printer/rtx/TabBar.cpp)
        target_link_libraries(uc_log_rtxui PUBLIC uc_log_printer_core rtxui)
        target_add_default_build_options(uc_log_rtxui PRIVATE)
        # RTXUI dynamic_casts components (a <radio> walks the component tree) and its terminal device: a component
        # compiled without RTTI has no type info for it, and the cast crashes
        target_compile_options(uc_log_rtxui PRIVATE -frtti)

        add_executable(uc_log_printer host/jlinkPrinter.cpp)
        target_link_libraries(uc_log_printer PRIVATE uc_log_printer_core uc_log_rtxui)
        target_add_default_build_options(uc_log_printer PRIVATE)

        # the stress run's checker (host/uc_log/stress/LogStressCheck.hpp): reads what a printer wrote for a firmware
        # that logs uc_log/stress/Records.hpp - a host tool beside the printer, not a test
        add_executable(uc_log_log_stress_check host/log_stress_check.cpp)
        target_link_libraries(uc_log_log_stress_check PRIVATE remote_fmt::parser fmt::fmt uc_log::host
                                                              enchantum::enchantum)
        target_add_default_build_options(uc_log_log_stress_check PRIVATE)

        if(${UC_LOG_BUILD_TEST_GUI})
            add_executable(uc_log_gui_test host/gui_test.cpp)
            target_link_libraries(uc_log_gui_test PRIVATE uc_log_rtxui)
            # what a frame of the window costs, headless (RTXUI's mock terminal is in its src/, not installed)
            add_executable(uc_log_rtxui_bench host/rtxui_bench.cpp)
            target_link_libraries(uc_log_rtxui_bench PRIVATE uc_log_rtxui)
            target_include_directories(uc_log_rtxui_bench SYSTEM PRIVATE ${rtxui_SOURCE_DIR}/src)
            target_add_default_build_options(uc_log_rtxui_bench PRIVATE)
            target_compile_options(uc_log_rtxui_bench PRIVATE -frtti)
            # the host tests' FakeTargetPort (tests/support/) is the demo's target too
            target_include_directories(uc_log_gui_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/tests)
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
