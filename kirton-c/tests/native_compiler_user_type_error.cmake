# User type failures may be rejected by the static compiler or diagnosed at
# runtime. Both must reject the invalid program without partial stdout.
if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE OR NOT DEFINED TEST_NAME)
  message(FATAL_ERROR "KIRTON, SOURCE and TEST_NAME required")
endif()
foreach(mode IN ITEMS ast bytecode)
  if(mode STREQUAL "bytecode")
    execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  else()
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  endif()
  if(NOT "${result}" STREQUAL "1" OR NOT "${output}" STREQUAL "" OR "${error}" STREQUAL "")
    message(FATAL_ERROR "${mode}: expected runtime error 1, got ${result}; ${output}; ${error}")
  endif()
endforeach()
if(CMAKE_HOST_WIN32)
  set(suffix ".exe")
else()
  set(suffix "")
endif()
set(exe "${CMAKE_CURRENT_BINARY_DIR}/native_user_type_error_${TEST_NAME}${suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${exe}"
  RESULT_VARIABLE compiled OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_error TIMEOUT 20)
if("${compiled}" STREQUAL "0")
  execute_process(COMMAND "${exe}" RESULT_VARIABLE executed
    OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic TIMEOUT 20)
  if(NOT "${executed}" STREQUAL "1" OR NOT "${output}" STREQUAL "" OR "${diagnostic}" STREQUAL "")
    message(FATAL_ERROR "Native: expected runtime error 1, got ${executed}; ${output}; ${diagnostic}")
  endif()
elseif(NOT "${compiled}" STREQUAL "65" OR "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "Native: unexpected compilation failure ${compiled}; ${compile_error}")
endif()
