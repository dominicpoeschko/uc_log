# Every ctest entry has a TIMEOUT: a hang ends at the limit instead of ctest's 1500 s default. The slowest run measured
# is about 120 s (tcp_server on clang); the values leave at least 2.5x for the slower CI containers.
set(UC_LOG_TEST_TIMEOUT
    120
    CACHE STRING "default ctest TIMEOUT (s) of a uc_log host test")

# The link groups of a host test or a must-fail case, each the one before plus its own: firmware (uc_log::uc_log, what a
# firmware TU sees), parser (+ remote_fmt's parser, fmt, enchantum: the log macros and frame decoding on the host),
# printer (+ the printer's headers uc_log::host, jlink, Boost), tui (+ FTXUI). A test takes the smallest group its
# includes need: a parser test is compiled and linked without the printer, J-Link and FTXUI.
function(uc_log_link_group target group)
    set(groups firmware parser printer tui)
    if(NOT group IN_LIST groups)
        message(FATAL_ERROR "${target}: LINKS '${group}' is none of ${groups}")
    endif()
    set(libs uc_log::uc_log)
    if(NOT group STREQUAL "firmware")
        list(APPEND libs remote_fmt::parser fmt::fmt enchantum::enchantum Threads::Threads)
    endif()
    if(group STREQUAL "printer" OR group STREQUAL "tui")
        list(APPEND libs uc_log::host jlink::jlink atomic ${Boost_Components})
    endif()
    if(group STREQUAL "tui")
        list(APPEND libs ftxui::screen ftxui::dom ftxui::component)
    endif()
    target_link_libraries(${target} PRIVATE ${libs})
endfunction()

# uc_log_add_test(<name> <sources>... LINKS <group> [TIMEOUT <s>] [CASES <case>...]) LINKS: uc_log_link_group's group
# (required). CASES: one ctest entry <name>.<case> per case of the test's uc_log::test::run() (`--case <case>`), plus
# <name>.cases, which fails when the source's cases and this list differ (a case missing here would never run). Without
# CASES: one entry <name>, the whole executable.
function(uc_log_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TIMEOUT;LINKS" "CASES")
    if(NOT arg_LINKS)
        message(FATAL_ERROR "uc_log_add_test(${name}): LINKS <group> is required (uc_log_link_group)")
    endif()
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT ${UC_LOG_TEST_TIMEOUT})
    endif()
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    uc_log_link_group(${name} ${arg_LINKS})
    target_add_default_build_options(${name} PRIVATE)
    if(arg_CASES)
        foreach(c IN LISTS arg_CASES)
            add_test(NAME ${name}.${c} COMMAND ${name} --case ${c})
            set_tests_properties(${name}.${c} PROPERTIES TIMEOUT ${arg_TIMEOUT})
        endforeach()
        add_test(NAME ${name}.cases COMMAND ${name} --expect-cases ${arg_CASES})
        set_tests_properties(${name}.cases PROPERTIES TIMEOUT ${arg_TIMEOUT})
    else()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT ${arg_TIMEOUT})
    endif()
endfunction()

# uc_log_add_fail_test(<family> <source> LINKS <group> [DEFINITIONS <def>...] CASES <value> <label> <expect> ...) Code
# that must NOT compile; a rejection is a hard compile error that cannot be observed as a bool, so each case is its own
# EXCLUDE_FROM_ALL target <family>_fail_<label>, and its test builds it. The source picks the case by
# UC_LOG_FAIL_CASE=<value>; a value may carry more definitions after commas (`3,UC_LOG_USE_AGLIO=0`). <expect> ACCEPT: a
# control, test <family>_accepts_<label>, that must build - it proves the stubs and includes are sound. Anything else is
# a regex of the rejection's own text in gcc's and clang's wording, test <family>_rejects_<label>: the build must print
# it (PASS_REGULAR_EXPRESSION), so a case that fails for an unrelated reason fails its test. Keep the cases in step with
# the source. The tests drive the build system and never run concurrently under "ctest -j" (RESOURCE_LOCK).
function(uc_log_add_fail_test family source)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "LINKS" "DEFINITIONS;CASES")
    list(LENGTH arg_CASES count)
    math(EXPR rest "${count} % 3")
    if(NOT arg_LINKS
       OR count EQUAL 0
       OR NOT rest EQUAL 0)
        message(FATAL_ERROR "uc_log_add_fail_test(${family}): LINKS <group> and CASES <value> <label> <expect> ...")
    endif()
    math(EXPR last "${count} - 1")
    foreach(i RANGE 0 ${last} 3)
        math(EXPR i_label "${i} + 1")
        math(EXPR i_expect "${i} + 2")
        list(GET arg_CASES ${i} value)
        list(GET arg_CASES ${i_label} label)
        list(GET arg_CASES ${i_expect} expect)
        string(REPLACE "," ";" definitions "${value}")
        list(POP_FRONT definitions fail_case)
        set(target ${family}_fail_${label})
        add_executable(${target} EXCLUDE_FROM_ALL ${source})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        uc_log_link_group(${target} ${arg_LINKS})
        target_add_default_build_options(${target} PRIVATE)
        target_compile_definitions(${target} PRIVATE ${arg_DEFINITIONS} UC_LOG_FAIL_CASE=${fail_case} ${definitions})
        if(expect STREQUAL "ACCEPT")
            set(test_name ${family}_accepts_${label})
        else()
            set(test_name ${family}_rejects_${label})
        endif()
        add_test(NAME ${test_name} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${target} --config
                                           $<CONFIG>)
        set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK uc_log_fail_build TIMEOUT 300)
        if(NOT expect STREQUAL "ACCEPT")
            set_tests_properties(${test_name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
        endif()
    endforeach()
endfunction()

# The test support itself (support/Check.hpp): the failing cases must fail, with file:line in the text.
uc_log_add_test(
    uc_log_test_support_check
    support_check_tests.cpp
    LINKS
    parser
    CASES
    passes
    failsOnPurpose
    checkEqPrintsBothSides
    throwsOnPurpose)
set_tests_properties(uc_log_test_support_check.failsOnPurpose uc_log_test_support_check.checkEqPrintsBothSides
                     uc_log_test_support_check.throwsOnPurpose PROPERTIES WILL_FAIL TRUE)
add_test(NAME uc_log_test_support_check.fails_text COMMAND uc_log_test_support_check --case failsOnPurpose)
set_tests_properties(uc_log_test_support_check.fails_text
                     PROPERTIES PASS_REGULAR_EXPRESSION "FAIL: on purpose \\(.*support_check_tests.cpp:[0-9]+\\)")
add_test(NAME uc_log_test_support_check.eq_text COMMAND uc_log_test_support_check --case checkEqPrintsBothSides)
set_tests_properties(
    uc_log_test_support_check.eq_text
    PROPERTIES PASS_REGULAR_EXPRESSION
               "FAIL: eq on purpose: got \"41\" expected \"42\" \\(.*support_check_tests.cpp:[0-9]+\\)")
add_test(NAME uc_log_test_support_check.throws_text COMMAND uc_log_test_support_check --case throwsOnPurpose)
set_tests_properties(uc_log_test_support_check.throws_text
                     PROPERTIES PASS_REGULAR_EXPRESSION "FAIL: case throwsOnPurpose threw: thrown on purpose")
add_test(NAME uc_log_test_support_check.unknown COMMAND uc_log_test_support_check --case nope)
set_tests_properties(uc_log_test_support_check.unknown PROPERTIES WILL_FAIL TRUE)
set_tests_properties(uc_log_test_support_check.fails_text uc_log_test_support_check.eq_text
                     uc_log_test_support_check.throws_text uc_log_test_support_check.unknown PROPERTIES TIMEOUT 120)

uc_log_add_test(uc_log_test_logentry logentry_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_catalog_load catalog_load_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_rtt_channel rtt_channel_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_rtt_channel_map rtt_channel_map_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_time_delayed_queue time_delayed_queue_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_entry_store entry_store_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_tcp_server tcp_server_tests.cpp LINKS printer TIMEOUT 600)
uc_log_add_test(uc_log_test_control_server control_server_tests.cpp LINKS printer)
# the reader loop against the fake J-Link (support/FakeJLink.hpp): one binary, rtt_reader/rtt_reader_tests.cpp, whose
# cases live in one header per concern (rtt_reader/<concern>_tests.hpp) - one compile of the headers and the reader on
# the fake, one link. One ctest entry per case (each on a fresh fake): a new case goes into its header's table AND the
# list here, in the order of rtt_reader_tests.cpp (decode, target_ops, resets, recovery, flash_gate, boots).
uc_log_add_test(
    uc_log_test_rtt_reader
    rtt_reader/rtt_reader_tests.cpp
    LINKS
    printer
    CASES
    # decode
    framesDecode
    quietTargetOnlyWarns
    overflowReported
    hostOffsetOutOfRange
    lostConnectionReconnects
    restartedCountersAreNotAWrap
    stopDecodesHeldLines
    directReaderStopsBetweenRecords
    silentAcrossConnections
    stopWithDecoderBacklog
    probeLostMidPass
    # target_ops
    accessTargetReadsAndWrites
    coreOpsThroughProbe
    haltsCaptured
    tuiCoreOps
    tuiHaltWhileWaitingForBlock
    haltPollRateUnderFlood
    faultHaltWhileWaiting
    # resets
    resetsAndFlashes
    outsideResetStartsOver
    announcedReset
    lateAnswerDoesNotWriteNewBoot
    lateArming
    # recovery
    stallSaidAndRecovered
    stallWedgedFlash
    stallWordSetNowAndThen
    stallNeedsEveryCondition
    stallLadderAcrossReconnects
    stallFirstStep
    stallWhileWaitingForBlock
    lockupDetection
    lockupRecoveryKeepsAnswering
    parkedInBootRom
    parkInTheGate
    # flash_gate
    flashReadyWaitsForMapping
    flashNeverMapped
    resetInTheCheck
    resetBeforeTheCheck
    armedAfterFlashReads
    flashWindowGate
    # boots
    unreadBoots
    tornTailAcrossReboot)
# wrapSweep is CPU-bound and slow under TSan: 133 s on a loaded machine, hence its own TIMEOUT
uc_log_add_test(
    uc_log_test_direct_rtt
    direct_rtt_tests.cpp
    LINKS
    printer
    TIMEOUT
    600
    CASES
    wrapSweep
    controlBlockLayout
    reinitInsideAnAccess
    reinitBetweenPasses
    logsInTheWindow
    oddEpoch
    stream
    namesAndDown
    corruptOffsets)
uc_log_add_test(uc_log_test_decode_worker decode_worker_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_segger_terms segger_terms_tests.cpp LINKS printer)
uc_log_add_test(
    uc_log_test_hex_token
    hex_token_tests.cpp
    LINKS
    printer
    CASES
    values
    tokens
    words)
uc_log_add_test(
    uc_log_test_message_log
    message_log_tests.cpp
    LINKS
    printer
    CASES
    wireNames
    counts
    historyAndStream
    statusFile
    observersOutsideTheLock
    haltKind
    formatErrorKind)
uc_log_add_test(
    uc_log_test_options
    options_tests.cpp
    LINKS
    printer
    CASES
    firmwareTreeLine
    defaults
    malformed
    offsetAndRefusals)
target_link_libraries(uc_log_test_options PRIVATE uc_log_printer_core)

# target_add_uc_log_rtt_jlink's <target>.printer_args against goldens (printer_args/: a fixture project that includes
# cmake/RttJlink.cmake alone, no compiler). A deliberate change: the same command with -DUPDATE=ON writes the goldens.
add_test(
    NAME uc_log_test_printer_args
    COMMAND
        ${CMAKE_COMMAND} -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/printer_args
        -DRTT_JLINK=${uc_log_dir}/cmake/RttJlink.cmake -DWORK=${CMAKE_CURRENT_BINARY_DIR}/printer_args
        -DGENERATOR=${CMAKE_GENERATOR} -P ${CMAKE_CURRENT_SOURCE_DIR}/printer_args/check.cmake)
set_tests_properties(uc_log_test_printer_args PROPERTIES TIMEOUT 120)
uc_log_add_test(
    uc_log_test_firmware_check
    firmware_check_tests.cpp
    LINKS
    printer
    CASES
    matchAndDifferent
    ramImage
    gate)
uc_log_add_test(
    uc_log_test_printer_services
    printer_services_tests.cpp
    LINKS
    printer
    CASES
    bootRows
    lossLedgerWatch
    logFiles)
uc_log_add_test(
    uc_log_test_printer_model
    printer_model_tests.cpp
    LINKS
    printer
    CASES
    ports
    loopback
    commands
    reports
    statistics)
target_link_libraries(uc_log_test_printer_model PRIVATE uc_log_printer_core)
uc_log_add_test(
    uc_log_test_printer_view
    printer_view_tests.cpp
    LINKS
    tui
    CASES
    outliers
    rowText
    filterFile
    counts64
    toggleSet
    numbers
    metricValues
    numberField
    scrollKeys)
uc_log_add_test(uc_log_test_map_facts map_facts_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_boot_ledger boot_ledger_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_row_file row_file_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_hex_image hex_image_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_ram_image ram_image_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_announced_reset announced_reset_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_target_inspector target_inspector_tests.cpp LINKS printer)
# the record decoders, and the fixture target (data/records) through both of its maps
uc_log_add_test(
    uc_log_test_target_records
    target_records_tests.cpp
    LINKS
    printer
    CASES
    everyRecordRejectsDamage
    namesAndSizes
    fixtureThroughLld
    fixtureThroughGnu
    readerThroughLld
    readerThroughGnu
    readerKindsAndWhileReading)
target_compile_definitions(uc_log_test_target_records
                           PRIVATE UC_LOG_RECORDS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/data/records")
uc_log_add_test(uc_log_test_target_loss target_loss_tests.cpp LINKS printer)
uc_log_add_test(uc_log_test_scroll_view scroll_view_tests.cpp LINKS tui)

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
# uc_log_add_m32_test(<name> <source> [INCLUDES <dir>...]): a -m32 test with its own warning set (no
# target_add_default_build_options: those flags are the 64-bit host's), UBSan stopping at its first report.
function(uc_log_add_m32_test name source)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "INCLUDES")
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${arg_INCLUDES})
    target_compile_features(${name} PRIVATE cxx_std_26)
    target_compile_options(${name} PRIVATE -m32 -Wall -Wextra -Wconversion -Wsign-conversion -Werror)
    target_link_options(${name} PRIVATE -m32)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" TIMEOUT
                                            ${UC_LOG_TEST_TIMEOUT})
endfunction()
if(UC_LOG_HAS_M32)
    # LossPolicy::Counted's ledger against a real ring (src/uc_log/detail/LossLedger.hpp)
    uc_log_add_m32_test(uc_log_test_loss_ledger loss_ledger_tests.cpp INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../rtt/src
                        ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR})
    # the duplex channels' read/write against the real control block, the test as the probe; FanOutBackend's hand-on
    # (src/uc_log/detail/RttBackendCore.hpp DuplexChannel<I>)
    uc_log_add_m32_test(
        uc_log_test_duplex_channel duplex_channel_tests.cpp INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../rtt/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Golden files in doc/control_protocol/: `update_control_protocol` rewrites them.
set(uc_log_control_protocol_dir "${CMAKE_CURRENT_SOURCE_DIR}/../doc/control_protocol")
uc_log_add_test(uc_log_test_control_protocol control_protocol_tests.cpp LINKS printer)
target_compile_definitions(
    uc_log_test_control_protocol PRIVATE UC_LOG_CONTROL_PROTOCOL_DIR="${uc_log_control_protocol_dir}"
                                         UC_LOG_RECORDS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/data/records")
add_executable(uc_log_control_protocol_tool control_protocol_tool.cpp)
target_link_libraries(uc_log_control_protocol_tool PRIVATE remote_fmt::parser fmt::fmt Threads::Threads uc_log::host
                                                           ${Boost_Components} enchantum::enchantum)
target_add_default_build_options(uc_log_control_protocol_tool PRIVATE)
target_compile_definitions(uc_log_control_protocol_tool
                           PRIVATE UC_LOG_RECORDS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/data/records")
add_custom_target(
    update_control_protocol
    COMMAND uc_log_control_protocol_tool dump "${uc_log_control_protocol_dir}"
    DEPENDS uc_log_control_protocol_tool
    COMMENT "Writing the control protocol's golden files")
# the fuzz dictionary holds every word of the golden requests (a new command or key is a new token), in every tree: the
# dictionary is a source file, the fuzzers that read it need libFuzzer
add_test(NAME uc_log_fuzz_dict COMMAND uc_log_control_protocol_tool dict
                                       "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/control_protocol.dict")
set_tests_properties(uc_log_fuzz_dict PROPERTIES TIMEOUT ${UC_LOG_TEST_TIMEOUT})
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
    set_tests_properties(uc_log_fuzz_seeds PROPERTIES FIXTURES_SETUP uc_log_fuzz_seeds TIMEOUT ${UC_LOG_TEST_TIMEOUT})
    # libFuzzer writes new inputs into the FIRST corpus directory: for ctest that one is emptied before every run, so
    # each starts from the same seeds instead of the last run's findings (fuzz_corpus/ stays for long local runs)
    set(uc_log_fuzz_new "${CMAKE_CURRENT_BINARY_DIR}/fuzz_new")
    add_test(NAME uc_log_fuzz_new_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${uc_log_fuzz_new}")
    add_test(NAME uc_log_fuzz_new_mkdir
             COMMAND ${CMAKE_COMMAND} -E make_directory "${uc_log_fuzz_new}/request" "${uc_log_fuzz_new}/event"
                     "${uc_log_fuzz_new}/session" "${uc_log_fuzz_new}/rtt_stream")
    set_tests_properties(uc_log_fuzz_new_clean uc_log_fuzz_new_mkdir PROPERTIES FIXTURES_SETUP uc_log_fuzz_seeds
                                                                                TIMEOUT ${UC_LOG_TEST_TIMEOUT})
    set_tests_properties(uc_log_fuzz_new_mkdir PROPERTIES DEPENDS uc_log_fuzz_new_clean)
    # Same inputs on every run, not only the same seed: -fno-sanitize-coverage=stack-depth (that feature is measured
    # from the stack start, which ASLR moves - the same input gave different features in every process) and, in the
    # tests, -use_cmp=0 (compare tracing feeds pointer operands into the mutations)
    set(uc_log_fuzz_flags -fsanitize=fuzzer,address,undefined -fno-sanitize-coverage=stack-depth
                          -fno-sanitize-recover=undefined)
    foreach(fuzzer request event session)
        set(fuzz_target uc_log_fuzz_control_${fuzzer})
        add_executable(${fuzz_target} fuzz/fuzz_control_${fuzzer}.cpp)
        target_link_libraries(${fuzz_target} PRIVATE remote_fmt::parser fmt::fmt Threads::Threads uc_log::host
                                                     ${Boost_Components} enchantum::enchantum)
        target_add_default_build_options(${fuzz_target} PRIVATE)
        target_compile_options(${fuzz_target} PRIVATE ${uc_log_fuzz_flags})
        target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address,undefined)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus/${fuzzer}")
        # a fixed seed and a fixed number of runs, never a time budget: the same inputs on every machine; `just fuzz`
        # runs them for a time. The session fuzzer only replays its corpus here (-runs=0: the seeds and regressions,
        # once each): its coverage comes from a server's threads too, whose interleaving differs from run to run - new
        # inputs from it would differ as well
        set(fuzz_runs 20000)
        if(fuzzer STREQUAL "session")
            set(fuzz_runs 0)
        endif()
        add_test(
            NAME ${fuzz_target}
            COMMAND
                ${fuzz_target} -runs=${fuzz_runs} -seed=1 -use_cmp=0 -timeout=10
                -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/control_protocol.dict "${uc_log_fuzz_new}/${fuzzer}"
                "${uc_log_fuzz_seeds}/${fuzzer}" "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${fuzzer}")
        set_tests_properties(${fuzz_target} PROPERTIES FIXTURES_REQUIRED uc_log_fuzz_seeds TIMEOUT 300)
    endforeach()

    # the printer's frame extraction over arbitrary streams in arbitrary pieces (fuzz/fuzz_rtt_stream.cpp)
    add_executable(uc_log_fuzz_rtt_stream fuzz/fuzz_rtt_stream.cpp)
    target_include_directories(uc_log_fuzz_rtt_stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(uc_log_fuzz_rtt_stream PRIVATE remote_fmt::parser fmt::fmt uc_log::host enchantum::enchantum)
    target_add_default_build_options(uc_log_fuzz_rtt_stream PRIVATE)
    target_compile_definitions(uc_log_fuzz_rtt_stream PRIVATE REMOTE_FMT_USE_CATALOG=false)
    target_compile_options(uc_log_fuzz_rtt_stream PRIVATE ${uc_log_fuzz_flags})
    target_link_options(uc_log_fuzz_rtt_stream PRIVATE -fsanitize=fuzzer,address,undefined)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus/rtt_stream")
    # the seeds (the tool's `seeds`: frame starts where the resync has work), and regressions/rtt_stream/ once a run has
    # found something; no dictionary (the input is a byte stream)
    set(uc_log_fuzz_rtt_stream_corpora "${uc_log_fuzz_new}/rtt_stream" "${uc_log_fuzz_seeds}/rtt_stream")
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/rtt_stream")
        list(APPEND uc_log_fuzz_rtt_stream_corpora "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/rtt_stream")
    endif()
    add_test(NAME uc_log_fuzz_rtt_stream COMMAND uc_log_fuzz_rtt_stream -runs=20000 -seed=1 -use_cmp=0 -timeout=10
                                                 ${uc_log_fuzz_rtt_stream_corpora})
    set_tests_properties(uc_log_fuzz_rtt_stream PROPERTIES FIXTURES_REQUIRED uc_log_fuzz_seeds TIMEOUT 300)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME uc_log_test_client_protocol COMMAND ${Python3_EXECUTABLE}
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_uc_log_client.py")
    set_tests_properties(
        uc_log_test_client_protocol
        PROPERTIES ENVIRONMENT "UC_LOG_CONTROL_PROTOCOL_TOOL=$<TARGET_FILE:uc_log_control_protocol_tool>" TIMEOUT 120)
endif()

# the C++ test writes sample rows, the Python one reads them back with the csv module
uc_log_add_test(uc_log_test_log_format log_format_tests.cpp LINKS printer)
set_tests_properties(uc_log_test_log_format PROPERTIES FIXTURES_SETUP uc_log_rttlog_sample)
if(Python3_FOUND)
    add_test(NAME uc_log_test_rttlog_csv
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_rttlog_format.py"
                     "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(uc_log_test_rttlog_csv PROPERTIES FIXTURES_REQUIRED uc_log_rttlog_sample TIMEOUT 120)
endif()

uc_log_add_test(uc_log_test_log_macro log_macro_tests.cpp LINKS printer)
# Catalog mode, as on a target: the test decodes its own lines with the catalog the post-build step writes.
target_compile_definitions(uc_log_test_log_macro PRIVATE USE_UC_LOG)
set(uc_log_test_log_macro_catalog "${CMAKE_CURRENT_BINARY_DIR}/uc_log_test_log_macro_string_constants.json")
# ID_KEY_STRIP: uc_log's rule (UC_LOG_ID_KEY_STRIP, ../CMakeLists.txt), as on a target
target_generate_string_constants(uc_log_test_log_macro ID_KEY_STRIP "${UC_LOG_ID_KEY_STRIP}")
target_compile_definitions(uc_log_test_log_macro PRIVATE UC_LOG_TEST_CATALOG_JSON="${uc_log_test_log_macro_catalog}")

# FanOutBackend: every record in every sink, the primary's lost line in the primary only (catalog mode, like above).
uc_log_add_test(uc_log_test_fan_out fan_out_tests.cpp LINKS printer)
target_compile_definitions(uc_log_test_fan_out PRIVATE USE_UC_LOG)
target_generate_string_constants(uc_log_test_fan_out ID_KEY_STRIP "${UC_LOG_ID_KEY_STRIP}")
target_compile_definitions(
    uc_log_test_fan_out
    PRIVATE UC_LOG_TEST_CATALOG_JSON="${CMAKE_CURRENT_BINARY_DIR}/uc_log_test_fan_out_string_constants.json")

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
uc_log_add_test(uc_log_test_log_min_level log_min_level_tests.cpp LINKS parser)
target_compile_definitions(uc_log_test_log_min_level PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false
                                                             UC_LOG_MIN_LEVEL=warn)

uc_log_add_test(uc_log_test_log_filter log_filter_tests.cpp LINKS parser)
target_compile_definitions(uc_log_test_log_filter PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false)
target_compile_options(uc_log_test_log_filter PRIVATE "--embed-dir=${CMAKE_CURRENT_SOURCE_DIR}/log_filter")

uc_log_add_test(uc_log_test_log_env log_env_tests.cpp LINKS parser)
target_compile_definitions(uc_log_test_log_env PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false UC_LOG_MIN_LEVEL=info)

# The macros without USE_UC_LOG: no code, no evaluation, no unused-argument warnings (the default build options carry
# the warning flags; the test checks the evaluation).
uc_log_add_test(uc_log_test_log_disabled log_disabled_tests.cpp LINKS parser)
# The headers a TU without USE_UC_LOG reads (the compiler's -H list of the same TU, with its include dirs and
# definitions): none of the formatting stack. A firmware's release variant includes uc_log.hpp the same way.
set(UC_LOG_NO_LOG_FORBIDDEN "remote_fmt/remote_fmt.hpp" "aglio/" "glaze/" "enchantum/" "mp-units/" "/fmt/")
add_test(
    NAME uc_log_no_log_includes
    COMMAND
        ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/log_disabled_tests.cpp
        "-DINCLUDES=$<JOIN:$<TARGET_PROPERTY:uc_log_test_log_disabled,INCLUDE_DIRECTORIES>,|>"
        "-DDEFINITIONS=$<JOIN:$<TARGET_PROPERTY:uc_log_test_log_disabled,COMPILE_DEFINITIONS>,|>"
        "-DFORBIDDEN=$<JOIN:${UC_LOG_NO_LOG_FORBIDDEN},|>" -P ${CMAKE_CURRENT_SOURCE_DIR}/no_log_includes.cmake)

# The ISR ring policies of the RTT backends, host-checkable parts only.
uc_log_add_test(uc_log_test_isr_policy isr_policy_tests.cpp LINKS parser)
# which RTT ring a write goes to, for every ring layout, policy, core and level
uc_log_add_test(uc_log_test_rtt_ring_choice rtt_ring_choice_tests.cpp LINKS parser)

# Arguments the UC_LOG macros must NOT accept (log_macro_fail.cpp; its backend stubs match log_macro_tests.cpp). Case 3
# is a reflectable struct with the aglio formatter switched off; the controls without aglio and the same struct with it
# must build, so case 3 fails for the switch and nothing else. Case 4 (shadow) builds with -Wshadow as an error, a scope
# environment and a backend that counts its losses: nothing UC_LOG_AT or RecordFrame::reportLost declares shadows.
uc_log_add_fail_test(
    log_macro
    log_macro_fail.cpp
    LINKS
    parser
    DEFINITIONS
    USE_UC_LOG
    REMOTE_FMT_USE_CATALOG=false
    CASES
    0,UC_LOG_USE_AGLIO=1
    control
    ACCEPT
    0,UC_LOG_USE_AGLIO=0
    control_without_aglio
    ACCEPT
    3,UC_LOG_USE_AGLIO=1
    reflected
    ACCEPT
    4,UC_LOG_USE_AGLIO=1
    shadow
    ACCEPT
    1,UC_LOG_USE_AGLIO=1
    1
    "typed pointers cannot be logged"
    2,UC_LOG_USE_AGLIO=1
    2
    "char pointers cannot be logged"
    3,UC_LOG_USE_AGLIO=0
    3
    "remote_fmt::formatter<Reflected>")

# What a backend's LossPolicy parameter must NOT accept (loss_policy_fail.cpp).
uc_log_add_fail_test(
    loss_policy
    loss_policy_fail.cpp
    LINKS
    firmware
    CASES
    0
    control
    ACCEPT
    1
    1
    "(constraints not satisfied|template constraint failure) for .*RttComBackend"
    2
    2
    "the fastest CPU clock of the image in Hz, not 0"
    3
    3
    "too few template arguments for class template 'Counted'|wrong number of template arguments \\(0, should be at least 1\\)"
)

# Policy configurations IsrPolicy.hpp must NOT accept (isr_policy_fail.cpp).
uc_log_add_fail_test(
    isr_policy
    isr_policy_fail.cpp
    LINKS
    firmware
    CASES
    0
    control
    ACCEPT
    1
    1
    " Levels: each level once, in 0\\.\\.255"
    2
    2
    "a level cannot both have a ring \\(Levels\\) and be declared silent"
    3
    3
    "SilentLevels: each level once, in 0\\.\\.255"
    4
    4
    "PerLevel needs at least one served level")

# What RttLog's options and a fan-out's contexts must NOT accept (rtt_log_fail.cpp).
uc_log_add_fail_test(
    rtt_log
    rtt_log_fail.cpp
    LINKS
    firmware
    CASES
    0
    control
    ACCEPT
    1
    1
    "RttLog: two LossPolicies"
    2
    2
    "RttLog: an option is none of"
    3
    3
    "RttLog: give the ring sizes once"
    4
    4
    "a sink's ISR contexts are per core")

# uc_log::bootsThatLogged() (Runtime.hpp) on a tag whose backend counts no boots: a named static_assert
# (runtime_fail.cpp; panicMode() and drain() compile on every tag, the control).
uc_log_add_fail_test(
    runtime
    runtime_fail.cpp
    LINKS
    firmware
    CASES
    0
    control
    ACCEPT
    1
    backend_without_count
    "bootsThatLogged\\(\\): the tag's backend counts no boots"
    2
    no_backend
    "bootsThatLogged\\(\\): the tag's backend counts no boots")

# The stress stream's host side (src/uc_log/stress/Records.hpp and a firmware that logs it): the checker against
# synthetic streams and remote_fmt's own encoder (the command-line checker for a real run's .rttlog files is a host
# tool: uc_log_log_stress_check, host/log_stress_check.cpp).
uc_log_add_test(uc_log_test_log_stress_check stress/log_stress_check_tests.cpp LINKS printer)
target_compile_definitions(uc_log_test_log_stress_check PRIVATE REMOTE_FMT_USE_CATALOG=false REMOTE_FMT_FRAME_CHECK=0)
# the same with the frame check (REMOTE_FMT_FRAME_CHECK): checked frames through the same decoder
uc_log_add_test(uc_log_test_log_stress_check_framed stress/log_stress_check_tests.cpp LINKS printer)
target_compile_definitions(uc_log_test_log_stress_check_framed PRIVATE REMOTE_FMT_USE_CATALOG=false
                                                                       REMOTE_FMT_FRAME_CHECK=1)
# gcc's LTO loses Printer::lowprint's bound (it copies into the staging array only when the span fits behind `staged`)
# and reports a write past it; the warning comes at link time, so it is turned off for the link too
foreach(target uc_log_test_log_stress_check uc_log_test_log_stress_check_framed)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-overflow>)
    target_link_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-overflow>)
endforeach()

# The config-header hook (UC_LOG_CONFIG_HEADER): uc_log.hpp reads the named header first, so a TU that includes only
# uc_log.hpp logs through the sink and clock specialised there.
uc_log_add_test(uc_log_test_config_header config_header_tests.cpp LINKS parser CASES logsThroughTheConfigHeader)
target_compile_definitions(
    uc_log_test_config_header PRIVATE USE_UC_LOG REMOTE_FMT_USE_CATALOG=false
                                      UC_LOG_CONFIG_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/config_header/test_config.hpp")
# A config header that reaches uc_log.hpp must be the named #error; the control is the same TU with a clean config
# header.
uc_log_add_fail_test(
    config_header
    config_header_tests.cpp
    LINKS
    parser
    DEFINITIONS
    USE_UC_LOG
    REMOTE_FMT_USE_CATALOG=false
    UC_LOG_CONFIG_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/config_header/test_config.hpp"
    CASES
    0
    control
    ACCEPT
    1,UC_LOG_CONFIG_HEADER_FAIL
    reentry
    "includes uc_log/uc_log.hpp, directly or through another header")

# The printer's model headers (host/uc_log/printer/ outside tui/) never reach FTXUI (printer_layering_fail.cpp): a
# poisoned copy of every FTXUI header comes first on the include path, each an #error.
if(ftxui_SOURCE_DIR)
    set(layering_dir ${CMAKE_CURRENT_BINARY_DIR}/printer_layering)
    file(
        GLOB_RECURSE ftxui_headers
        RELATIVE ${ftxui_SOURCE_DIR}/include
        ${ftxui_SOURCE_DIR}/include/ftxui/*.hpp)
    foreach(header IN LISTS ftxui_headers)
        file(CONFIGURE OUTPUT ${layering_dir}/poison/${header} CONTENT
             "#error \"FTXUI reached from a printer model header: ${header}\"\n")
    endforeach()
    file(
        GLOB printer_headers CONFIGURE_DEPENDS
        RELATIVE ${PROJECT_SOURCE_DIR}/host
        ${PROJECT_SOURCE_DIR}/host/uc_log/printer/*.hpp ${PROJECT_SOURCE_DIR}/host/uc_log/printer/reader/*.hpp)
    list(TRANSFORM printer_headers REPLACE "(.+)" "#include \"\\1\"")
    list(JOIN printer_headers "\n" printer_includes)
    file(CONFIGURE OUTPUT ${layering_dir}/printer_headers.hpp CONTENT "${printer_includes}\n")
    uc_log_add_fail_test(
        printer_layering
        printer_layering_fail.cpp
        LINKS
        printer
        DEFINITIONS
        UC_LOG_PRINTER_HEADERS="${layering_dir}/printer_headers.hpp"
        CASES
        0
        model_headers
        ACCEPT
        1
        tui_header
        "FTXUI reached from a printer model header")
    foreach(label IN ITEMS model_headers tui_header)
        target_include_directories(printer_layering_fail_${label} BEFORE PRIVATE ${layering_dir}/poison)
    endforeach()
endif()

# A firmware target cannot include a printer header: they live under host/ (uc_log::host), and a firmware links only
# uc_log::uc_log. The control (a firmware header) must build, the printer header must be the one not found.
uc_log_add_fail_test(
    printer_include
    printer_include_fail.cpp
    LINKS
    firmware
    CASES
    0
    control
    ACCEPT
    1
    printer_header
    "uc_log/printer/ControlServer\\.hpp['\"]? file not found|uc_log/printer/ControlServer\\.hpp: No such file")
