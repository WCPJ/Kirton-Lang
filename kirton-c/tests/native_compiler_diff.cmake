if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE OR NOT DEFINED EXPECT)
  message(FATAL_ERROR "KIRTON, SOURCE and EXPECT are required")
endif()
if(CMAKE_HOST_WIN32)
  set(native_suffix ".exe")
else()
  set(native_suffix "")
endif()

foreach(mode IN ITEMS ast bytecode)
  if(mode STREQUAL "bytecode")
    execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20
      ENCODING UTF-8)
  else()
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20
      ENCODING UTF-8)
  endif()
  string(STRIP "${output}" output)
string(REPLACE "\r\n" "|" output "${output}")
string(REPLACE "\n" "|" output "${output}")
  if(NOT "${result}" STREQUAL "0" OR NOT output STREQUAL "${EXPECT}" OR
     NOT "${error}" STREQUAL "")
    message(FATAL_ERROR "${mode}: exit=${result}, output=[${output}], error=[${error}]")
  endif()
endforeach()

if(NOT NATIVE_ENABLED)
  return()
endif()

set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_${TEST_NAME}${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_error TIMEOUT 20
  ENCODING UTF-8)
if(NOT "${compile_result}" STREQUAL "0" OR
   NOT "${compile_out}" STREQUAL "" OR
   NOT "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "native compile failed: exit=${compile_result}, output=[${compile_out}], error=[${compile_error}]")
endif()
execute_process(COMMAND "${native_exe}"
  RESULT_VARIABLE native_result OUTPUT_VARIABLE native_output ERROR_VARIABLE native_error TIMEOUT 20
  ENCODING UTF-8)
string(STRIP "${native_output}" native_output)
string(REPLACE "\r\n" "|" native_output "${native_output}")
string(REPLACE "\n" "|" native_output "${native_output}")
if(NOT "${native_result}" STREQUAL "0" OR
   NOT native_output STREQUAL "${EXPECT}" OR
   NOT "${native_error}" STREQUAL "")
  message(FATAL_ERROR "native: exit=${native_result}, output=[${native_output}], error=[${native_error}]")
endif()
