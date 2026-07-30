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
                enchantum::enchantum
                glaze::glaze)
    target_add_default_build_options(${name} PRIVATE)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

uc_log_add_test(uc_log_test_logentry logentry_tests.cpp)
uc_log_add_test(uc_log_test_rtt_channel rtt_channel_tests.cpp)
uc_log_add_test(uc_log_test_entry_store entry_store_tests.cpp)
uc_log_add_test(uc_log_test_tcp_server tcp_server_tests.cpp)
uc_log_add_test(uc_log_test_rtt_reader rtt_reader_tests.cpp)

uc_log_add_test(uc_log_test_log_macro log_macro_tests.cpp)
# USE_UC_LOG turns the macros from (void)0 into real calls. The catalog is off because its ids come from a post-link
# pass over the target's objects, which a host test cannot run; without it the format strings go on the wire inline.
target_compile_definitions(uc_log_test_log_macro PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false)
