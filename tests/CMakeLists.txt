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
uc_log_add_test(uc_log_test_entry_store entry_store_tests.cpp)
uc_log_add_test(uc_log_test_tcp_server tcp_server_tests.cpp)
uc_log_add_test(uc_log_test_rtt_reader rtt_reader_tests.cpp)

uc_log_add_test(uc_log_test_log_macro log_macro_tests.cpp)
# USE_UC_LOG turns the macros into real calls (without it every call site is a constant-false conditional that only
# checks and "uses" its arguments). The catalog is off because its ids come from a post-link pass over the target's
# objects, which a host test cannot run; without it the format strings go on the wire inline.
target_compile_definitions(uc_log_test_log_macro PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false)

# Same macros with a compile-time level floor (warn) and a backend exposing the optional transfer hooks.
uc_log_add_test(uc_log_test_log_min_level log_min_level_tests.cpp)
target_compile_definitions(uc_log_test_log_min_level PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false
                                                             UC_LOG_MIN_LEVEL=warn)

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
