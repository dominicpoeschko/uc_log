function(uc_log_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(
        ${name}
        PRIVATE remote_fmt::parser
                fmt::fmt
                Threads::Threads
                uc_log::uc_log
                jlink::jlink
                atomic
                ${Boost_Components}
                ftxui::screen
                ftxui::dom
                ftxui::component
                enchantum::enchantum)
    target_add_default_build_options(${name} PRIVATE)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

uc_log_add_test(uc_log_test_logentry logentry_tests.cpp)
uc_log_add_test(uc_log_test_rtt_channel rtt_channel_tests.cpp)
uc_log_add_test(uc_log_test_rtt_channel_map rtt_channel_map_tests.cpp)
uc_log_add_test(uc_log_test_time_delayed_queue time_delayed_queue_tests.cpp)
uc_log_add_test(uc_log_test_entry_store entry_store_tests.cpp)
uc_log_add_test(uc_log_test_tcp_server tcp_server_tests.cpp)
uc_log_add_test(uc_log_test_rtt_reader rtt_reader_tests.cpp)
if(JLINK_USE_FAKE_DLL)
    uc_log_add_test(uc_log_test_jlink jlink_tests.cpp)
endif()
uc_log_add_test(uc_log_test_hex_image hex_image_tests.cpp)
uc_log_add_test(uc_log_test_ram_image ram_image_tests.cpp)
uc_log_add_test(uc_log_test_announced_reset announced_reset_tests.cpp)
uc_log_add_test(uc_log_test_target_inspector target_inspector_tests.cpp)
uc_log_add_test(uc_log_test_scroll_view scroll_view_tests.cpp)

# RTT ring layout wants 4-byte pointers: -m32 where that links, with the directory's stdlib options (libc++ has no
# 32-bit package on Arch, so those builds skip it).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -m32)
set(CMAKE_REQUIRED_LINK_OPTIONS -m32)
if("${USE_STD_LIB}" STREQUAL "libc++" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_REQUIRED_FLAGS "-m32 -stdlib=libc++")
    set(CMAKE_REQUIRED_LINK_OPTIONS -m32 -stdlib=libc++ -fuse-ld=lld)
endif()
check_cxx_source_compiles("#include <deque>\nint main() { std::deque<int> d{1}; return d[0] - 1; }" UC_LOG_HAS_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(UC_LOG_HAS_M32)
    add_executable(uc_log_test_rtt_ring ${CMAKE_CURRENT_SOURCE_DIR}/../rtt/tests/ring_tests.cpp)
    target_include_directories(uc_log_test_rtt_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../rtt/src)
    target_compile_features(uc_log_test_rtt_ring PRIVATE cxx_std_26)
    target_compile_options(uc_log_test_rtt_ring PRIVATE -m32 -Wall -Wextra -Wconversion -Wsign-conversion -Werror)
    target_link_options(uc_log_test_rtt_ring PRIVATE -m32)
    target_link_libraries(uc_log_test_rtt_ring PRIVATE Threads::Threads)
    add_test(NAME uc_log_test_rtt_ring COMMAND uc_log_test_rtt_ring)
    set_tests_properties(uc_log_test_rtt_ring PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()

# Golden files in doc/control_protocol/: `update_control_protocol` rewrites them.
set(uc_log_control_protocol_dir "${CMAKE_CURRENT_SOURCE_DIR}/../doc/control_protocol")
uc_log_add_test(uc_log_test_control_protocol control_protocol_tests.cpp)
target_compile_definitions(uc_log_test_control_protocol
                           PRIVATE UC_LOG_CONTROL_PROTOCOL_DIR="${uc_log_control_protocol_dir}")
add_executable(uc_log_control_protocol_tool control_protocol_tool.cpp)
target_link_libraries(uc_log_control_protocol_tool PRIVATE remote_fmt::parser fmt::fmt Threads::Threads uc_log::uc_log
                                                           ${Boost_Components} enchantum::enchantum)
target_add_default_build_options(uc_log_control_protocol_tool PRIVATE)
add_custom_target(
    update_control_protocol
    COMMAND uc_log_control_protocol_tool dump "${uc_log_control_protocol_dir}"
    DEPENDS uc_log_control_protocol_tool
    COMMENT "Writing the control protocol's golden files")
# libFuzzer fuzzers of the control socket (tests/fuzz/README.md); ctest gives each a short run. On by default where the
# compiler ships libFuzzer and no other sanitizer is set that clashes with their address,undefined.
set(CMAKE_REQUIRED_FLAGS -fsanitize=fuzzer-no-link)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=fuzzer-no-link)
check_cxx_source_compiles("int main() { return 0; }" UC_LOG_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
set(uc_log_fuzz_default OFF)
if(UC_LOG_HAS_LIBFUZZER AND NOT "${USE_SANITIZER}" MATCHES "thread|memory|leak")
    set(uc_log_fuzz_default ON)
endif()
option(UC_LOG_BUILD_FUZZERS "build the control protocol fuzzers (needs libFuzzer)" ${uc_log_fuzz_default})
if(UC_LOG_BUILD_FUZZERS)
    set(uc_log_fuzz_seeds "${CMAKE_CURRENT_BINARY_DIR}/fuzz_seeds")
    add_test(NAME uc_log_fuzz_seeds COMMAND uc_log_control_protocol_tool seeds "${uc_log_fuzz_seeds}")
    set_tests_properties(uc_log_fuzz_seeds PROPERTIES FIXTURES_SETUP uc_log_fuzz_seeds)
    foreach(fuzzer request event session)
        set(fuzz_target uc_log_fuzz_control_${fuzzer})
        add_executable(${fuzz_target} fuzz/fuzz_control_${fuzzer}.cpp)
        target_link_libraries(${fuzz_target} PRIVATE remote_fmt::parser fmt::fmt Threads::Threads uc_log::uc_log
                                                     ${Boost_Components} enchantum::enchantum)
        target_add_default_build_options(${fuzz_target} PRIVATE)
        target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address,undefined
                                                      -fno-sanitize-recover=undefined)
        target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address,undefined)
        # libFuzzer writes into the first corpus directory only: keep that one in the build tree
        set(fuzz_corpus "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus/${fuzzer}")
        file(MAKE_DIRECTORY "${fuzz_corpus}")
        add_test(
            NAME ${fuzz_target}
            COMMAND
                ${fuzz_target} -runs=20000 -max_total_time=30 -timeout=10
                -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/control_protocol.dict "${fuzz_corpus}"
                "${uc_log_fuzz_seeds}/${fuzzer}" "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${fuzzer}")
        set_tests_properties(${fuzz_target} PROPERTIES FIXTURES_REQUIRED uc_log_fuzz_seeds)
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME uc_log_test_client_protocol COMMAND ${Python3_EXECUTABLE}
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_uc_log_client.py")
    set_tests_properties(
        uc_log_test_client_protocol
        PROPERTIES ENVIRONMENT "UC_LOG_CONTROL_PROTOCOL_TOOL=$<TARGET_FILE:uc_log_control_protocol_tool>")
endif()

# the C++ test writes sample rows, the Python one reads them back with the csv module
uc_log_add_test(uc_log_test_log_format log_format_tests.cpp)
set_tests_properties(uc_log_test_log_format PROPERTIES FIXTURES_SETUP uc_log_rttlog_sample)
if(Python3_FOUND)
    add_test(NAME uc_log_test_rttlog_csv
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_rttlog_format.py"
                     "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(uc_log_test_rttlog_csv PROPERTIES FIXTURES_REQUIRED uc_log_rttlog_sample)
endif()

uc_log_add_test(uc_log_test_log_macro log_macro_tests.cpp)
# Catalog mode, as on a target: the test decodes its own lines with the catalog the post-build step writes.
target_compile_definitions(uc_log_test_log_macro PRIVATE USE_UC_LOG)
set(uc_log_test_log_macro_catalog "${CMAKE_CURRENT_BINARY_DIR}/uc_log_test_log_macro_string_constants.json")
target_generate_string_constants(uc_log_test_log_macro)
target_compile_definitions(uc_log_test_log_macro PRIVATE UC_LOG_TEST_CATALOG_JSON="${uc_log_test_log_macro_catalog}")

# mp-units for the quantity metric test (optional). Same pin as remote_fmt/tests.
option(UC_LOG_TESTS_WITH_MP_UNITS "fetch mp-units for the quantity metric test" ON)
if(UC_LOG_TESTS_WITH_MP_UNITS AND NOT TARGET mp-units::mp-units)
    include(FetchContent)
    set(MP_UNITS_API_CONTRACTS
        "NONE"
        CACHE STRING "" FORCE) # otherwise mp-units wants gsl-lite
    set(MP_UNITS_API_NATURAL_UNITS
        OFF
        CACHE BOOL "" FORCE)
    set(MP_UNITS_BUILD_CXX_MODULES
        OFF
        CACHE BOOL "" FORCE)
    set(MP_UNITS_BUILD_INSTALL
        OFF
        CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mp-units
        GIT_REPOSITORY https://github.com/mpusz/mp-units.git
        GIT_TAG v2.5.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR src
        UPDATE_DISCONNECTED ON)
    FetchContent_MakeAvailable(mp-units)
    foreach(_t mp-units-core mp-units-systems)
        if(TARGET ${_t})
            get_target_property(_dirs ${_t} INTERFACE_INCLUDE_DIRECTORIES)
            if(_dirs)
                target_include_directories(${_t} SYSTEM INTERFACE ${_dirs})
            endif()
        endif()
    endforeach()
endif()
if(TARGET mp-units::mp-units)
    target_link_libraries(uc_log_test_log_macro PRIVATE mp-units::mp-units)
endif()

# Same macros with a compile-time level floor (warn) and a backend exposing the optional transfer hooks.
uc_log_add_test(uc_log_test_log_min_level log_min_level_tests.cpp)
target_compile_definitions(uc_log_test_log_min_level PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false
                                                             UC_LOG_MIN_LEVEL=warn)

uc_log_add_test(uc_log_test_log_filter log_filter_tests.cpp)
target_compile_definitions(uc_log_test_log_filter PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false)
target_compile_options(uc_log_test_log_filter PRIVATE "--embed-dir=${CMAKE_CURRENT_SOURCE_DIR}/log_filter")

uc_log_add_test(uc_log_test_log_env log_env_tests.cpp)
target_compile_definitions(uc_log_test_log_env PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false UC_LOG_MIN_LEVEL=info)

# The macros without USE_UC_LOG: no code, no evaluation, no unused-argument warnings (the default build options carry
# the warning flags; the test checks the evaluation).
uc_log_add_test(uc_log_test_log_disabled log_disabled_tests.cpp)

# The ISR ring policies of the RTT backends, host-checkable parts only.
uc_log_add_test(uc_log_test_isr_policy isr_policy_tests.cpp)

# Arguments the UC_LOG macros must NOT accept. A rejection is a hard compile error that cannot be observed as a bool, so
# each case is its own excluded target and the test is "building it fails". Case 0 is the control that must build, so a
# case cannot pass by failing for an unrelated reason. Keep in step with log_macro_fail.cpp.
set(log_macro_fail_cases 0 1 2)

foreach(fail_case IN LISTS log_macro_fail_cases)
    add_executable(log_macro_fail_${fail_case} EXCLUDE_FROM_ALL log_macro_fail.cpp)
    target_link_libraries(
        log_macro_fail_${fail_case}
        PRIVATE remote_fmt::parser
                fmt::fmt
                Threads::Threads
                uc_log::uc_log
                jlink::jlink
                atomic
                ${Boost_Components}
                ftxui::screen
                ftxui::dom
                ftxui::component
                enchantum::enchantum)
    target_add_default_build_options(log_macro_fail_${fail_case} PRIVATE)
    target_compile_definitions(log_macro_fail_${fail_case} PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false
                                                                   UC_LOG_FAIL_CASE=${fail_case})

    if(fail_case EQUAL 0)
        set(test_name log_macro_accepts_control)
    else()
        set(test_name log_macro_rejects_${fail_case})
    endif()
    add_test(NAME ${test_name} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                                       log_macro_fail_${fail_case} --config $<CONFIG>)
    # RESOURCE_LOCK: these drive the build system, so they must not run concurrently under "ctest -j".
    set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK log_macro_fail_build)
    if(NOT fail_case EQUAL 0)
        set_tests_properties(${test_name} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

# Policy configurations IsrPolicy.hpp must NOT accept, the same way, case 0 again the control. Keep in step with
# isr_policy_fail.cpp.
set(isr_policy_fail_cases 0 1 2 3 4)

foreach(fail_case IN LISTS isr_policy_fail_cases)
    add_executable(isr_policy_fail_${fail_case} EXCLUDE_FROM_ALL isr_policy_fail.cpp)
    target_link_libraries(isr_policy_fail_${fail_case} PRIVATE uc_log::uc_log)
    target_add_default_build_options(isr_policy_fail_${fail_case} PRIVATE)
    target_compile_definitions(isr_policy_fail_${fail_case} PRIVATE UC_LOG_FAIL_CASE=${fail_case})

    if(fail_case EQUAL 0)
        set(test_name isr_policy_accepts_control)
    else()
        set(test_name isr_policy_rejects_${fail_case})
    endif()
    add_test(NAME ${test_name} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                                       isr_policy_fail_${fail_case} --config $<CONFIG>)
    set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK log_macro_fail_build)
    if(NOT fail_case EQUAL 0)
        set_tests_properties(${test_name} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
