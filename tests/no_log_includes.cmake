# cmake -DCXX=<compiler> -DSOURCE=<tu> -DINCLUDES=<a|b> -DDEFINITIONS=<a|b> -DFORBIDDEN=<a|b> -P no_log_includes.cmake:
# fails when a header SOURCE reads (the compiler's -H list) names one of FORBIDDEN (a path fragment). -H, not -M/-MM:
# -MM leaves out the SYSTEM include dirs the formatting stack comes through, and -M also lists every file a
# __has_include found, read or not.
string(REPLACE "|" ";" includes "${INCLUDES}")
string(REPLACE "|" ";" definitions "${DEFINITIONS}")
string(REPLACE "|" ";" forbidden "${FORBIDDEN}")
set(args -std=c++26 -fsyntax-only -H)
foreach(dir IN LISTS includes)
    list(APPEND args "-I${dir}")
endforeach()
foreach(def IN LISTS definitions)
    list(APPEND args "-D${def}")
endforeach()
execute_process(
    COMMAND ${CXX} ${args} ${SOURCE}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE deps
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${CXX} -H failed (${rc}): ${deps}")
endif()
set(found "")
foreach(fragment IN LISTS forbidden)
    string(FIND "${deps}" "${fragment}" at)
    if(NOT at EQUAL -1)
        list(APPEND found "${fragment}")
    endif()
endforeach()
if(found)
    message(FATAL_ERROR "a TU without USE_UC_LOG reads ${found}: uc_log.hpp's front end must not include the "
                        "formatting stack")
endif()
message(STATUS "no formatting header in the no-log TU")
