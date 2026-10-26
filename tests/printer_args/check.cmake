# cmake -DFIXTURE=<tests/printer_args> -DRTT_JLINK=<cmake/RttJlink.cmake> -DWORK=<dir> -DGENERATOR=<g> -P check.cmake
# [-DUPDATE=ON writes the goldens]. Configures the fixture with RTT_JLINK and compares every <target>.printer_args with
# golden/<target>.printer_args byte for byte.
file(REMOVE_RECURSE "${WORK}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE}" -B "${WORK}" -G "${GENERATOR}" "-DUC_LOG_RTT_JLINK_CMAKE=${RTT_JLINK}"
    RESULT_VARIABLE configured
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT configured EQUAL 0)
    message(FATAL_ERROR "the fixture does not configure:\n${out}")
endif()
file(
    GLOB goldens
    RELATIVE "${FIXTURE}/golden"
    "${FIXTURE}/golden/*.printer_args")
set(targets every_keyword required_only empty_values)
set(failed "")
foreach(target IN LISTS targets)
    set(made "${WORK}/${target}.printer_args")
    set(golden "${FIXTURE}/golden/${target}.printer_args")
    if(UPDATE)
        file(COPY_FILE "${made}" "${golden}")
        continue()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${made}" "${golden}" RESULT_VARIABLE differs)
    if(NOT differs EQUAL 0)
        file(READ "${made}" text)
        string(APPEND failed "${target}.printer_args differs from its golden; it is:\n${text}\n")
    endif()
endforeach()
list(LENGTH targets wanted)
list(LENGTH goldens have)
if(NOT UPDATE AND NOT have EQUAL wanted)
    string(APPEND failed "golden/ holds ${have} files, the fixture makes ${wanted}\n")
endif()
if(failed)
    message(FATAL_ERROR "${failed}")
endif()
message(STATUS "printer_args: ${wanted} targets equal to their goldens")
