cmake_minimum_required(VERSION 3.18)

set(uc_log_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

set(uc_log_cmake_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

if(TARGET uc_log_printer)
  return()
endif()

project(uc_log VERSION 0.1.0)

add_subdirectory(rtt)
add_subdirectory(remote_fmt)

add_library(uc_log INTERFACE)
target_include_directories(uc_log INTERFACE src)
target_link_libraries(uc_log INTERFACE rtt::rtt remote_fmt::remote_fmt)

add_library(uc_log::uc_log ALIAS uc_log)

if(NOT CMAKE_CROSSCOMPILING)
  include(cmake_helpers/BuildOptions.cmake)
  include(cmake_helpers/FindOrFetch.cmake)

  find_package(Threads REQUIRED)
  find_or_fetch_package(fmt 8 GIT_REPOSITORY https://github.com/fmtlib/fmt.git
                        GIT_TAG master)
  find_or_fetch_package(CLI11 2 GIT_REPOSITORY
                        https://github.com/CLIUtils/CLI11 GIT_TAG main)
  find_or_fetch_package(nlohmann_json 3.9 GIT_REPOSITORY
                        https://github.com/nlohmann/json GIT_TAG master)

  add_subdirectory(toxic_spokes)
  add_subdirectory(jlink)

  add_executable(uc_log_printer src/uc_log/jlinkPrinter.cpp)
  target_link_libraries(
    uc_log_printer
    remote_fmt::parser
    fmt::fmt
    Threads::Threads
    toxic_spokes::toxic_spokes
    uc_log::uc_log
    jlink::jlink
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    atomic)
  target_add_default_build_options(uc_log_printer PUBLIC)
else()
  include(cmake_helpers/HostBuild.cmake)
  configure_host_build(uc_log_printer)
endif()

function(target_add_uc_log_rtt_jlink targetname)

  cmake_parse_arguments(
    PARSE_ARGV
    1
    PARSED_ARGS
    ""
    "TARGET_MPU;SWD_SPEED;JLINK_IP;CHANNELS;MAP_FILE;PREFIX;TRACE_PORT;HEX_FILE;STRING_CONSTANTS_MAP_FILE"
    "")

  if(PARSED_ARGS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
  endif()

  if(NOT PARSED_ARGS_TARGET_MPU)
    message(FATAL_ERROR "uc_log_rtt needs TARGET_MPU")
  endif()

  if(NOT PARSED_ARGS_SWD_SPEED)
    set(PARSED_ARGS_SWD_SPEED 100000)
  endif()

  if(NOT PARSED_ARGS_CHANNELS)
    set(PARSED_ARGS_CHANNELS 2)
  endif()

  if(NOT PARSED_ARGS_MAP_FILE)
    set(PARSED_ARGS_MAP_FILE "${targetname}.map")
  endif()

  if(NOT PARSED_ARGS_HEX_FILE)
    set(PARSED_ARGS_HEX_FILE "${targetname}_flash.hex")
  endif()

  if(NOT PARSED_ARGS_STRING_CONSTANTS_MAP_FILE)
    set(PARSED_ARGS_STRING_CONSTANTS_MAP_FILE
        "${targetname}_string_constants.json")
  endif()

  if(NOT PARSED_ARGS_PREFIX)
    set(PARSED_ARGS_PREFIX "log_")
  endif()

  if(NOT PARSED_ARGS_TRACE_PORT)
    set(PARSED_ARGS_TRACE_PORT 34565)
  endif()

  get_target_property(build_dir uc_log_printer BINARY_DIR)

  if(NOT CMAKE_CROSSCOMPILING)
    set(uc_log_printer_bin ${build_dir}/uc_log_printer)
  else()
    set(uc_log_printer_bin ${build_dir}/host_build/uc_log_printer)
  endif()
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rtt_log)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname})

  if(NOT PARSED_ARGS_JLINK_IP OR PARSED_ARGS_JLINK_IP STREQUAL "")
    add_custom_target(
      ${PARSED_ARGS_PREFIX}${targetname}
      COMMENT "start logging ${targetname}"
      USES_TERMINAL
      COMMAND
        ${uc_log_printer_bin} --trace_port ${PARSED_ARGS_TRACE_PORT} --device
        ${PARSED_ARGS_TARGET_MPU} --speed ${PARSED_ARGS_SWD_SPEED} --channels
        ${PARSED_ARGS_CHANNELS} --map_file ${PARSED_ARGS_MAP_FILE} --hex_file
        ${PARSED_ARGS_HEX_FILE} --string_constants_file
        ${PARSED_ARGS_STRING_CONSTANTS_MAP_FILE} --build_command
        "cmake --build . --target=${targetname}" --log_dir
        ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname}
      DEPENDS ${targetname} uc_log_printer ${uc_log_printer_bin})
  else()
    add_custom_target(
      ${PARSED_ARGS_PREFIX}${targetname}
      COMMENT "start logging ${targetname}"
      USES_TERMINAL
      COMMAND
        ${uc_log_printer_bin} --trace_port ${PARSED_ARGS_TRACE_PORT} --device
        ${PARSED_ARGS_TARGET_MPU} --speed ${PARSED_ARGS_SWD_SPEED} --channels
        ${PARSED_ARGS_CHANNELS} --map_file ${PARSED_ARGS_MAP_FILE} --host
        ${PARSED_ARGS_JLINK_IP} --hex_file ${PARSED_ARGS_HEX_FILE}
        --string_constants_file ${PARSED_ARGS_STRING_CONSTANTS_MAP_FILE}
        --build_command "cmake --build . --target=${targetname}" --log_dir
        ${CMAKE_CURRENT_BINARY_DIR}/rtt_log/${targetname}
      DEPENDS ${targetname} uc_log_printer ${uc_log_printer_bin})
  endif()

  get_target_property(bin_dir ${targetname} BINARY_DIR)

  target_link_options(${targetname} PUBLIC
                      ${bin_dir}/${targetname}_string_constants.obj)

  get_target_property(options_flags ${targetname} COMPILE_OPTIONS)
  get_target_property(options_flags2 ${targetname} COMPILE_FLAGS)

  string(JOIN " " flags ${CMAKE_CXX_FLAGS} ${options_flags})

  add_custom_command(
    TARGET ${targetname}
    PRE_LINK COMMAND_EXPAND_LISTS
    COMMAND
      python3 ${uc_log_dir}/tools/generate_string_constants.py --target_name
      ${targetname} --out_dir ${bin_dir} --source_dir
      ${CMAKE_CURRENT_SOURCE_DIR} --compiler ${CMAKE_CXX_COMPILER} --objects
      $<TARGET_OBJECTS:${targetname}> --flags ${flags} DEPENDS
      ${uc_log_dir}/tools/generate_string_constants.py BYPRODUCS
      ${bin_dir}/${targetname}_string_constants.cpp
      ${bin_dir}/${targetname}_string_constants.obj
      ${bin_dir}/${targetname}_string_constants.json)

  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY LINK_DEPENDS ${uc_log_dir}/tools/generate_string_constants.py)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.cpp)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.obj)
  set_property(
    TARGET ${targetname}
    APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES
             ${bin_dir}/${targetname}_string_constants.json)
endfunction()
