if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "KIRTON and SOURCE are required")
endif()
if(CMAKE_HOST_WIN32)
  set(native_suffix ".exe")
else()
  set(native_suffix "")
endif()
foreach(mode IN ITEMS ast bytecode)
  if(mode STREQUAL "bytecode")
    execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  else()
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
  endif()
  string(STRIP "${output}" output)
  if(NOT "${result}" STREQUAL "0" OR
     NOT output STREQUAL "9223372036854775808" OR
     NOT "${error}" STREQUAL "")
    message(FATAL_ERROR "${mode}: BigInt result mismatch, exit=${result}, stdout=[${output}], stderr=[${error}]")
  endif()
endforeach()
set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_function_overflow${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error TIMEOUT 20)
if(NOT "${compile_result}" STREQUAL "0" OR
   NOT "${compile_output}" STREQUAL "" OR
   NOT "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "native compile failed: exit=${compile_result}, output=[${compile_output}], error=[${compile_error}]")
endif()
execute_process(COMMAND "${native_exe}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
string(STRIP "${output}" output)
if(NOT "${result}" STREQUAL "0" OR
   NOT "${output}" STREQUAL "9223372036854775808" OR
   NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "native function BigInt overflow mismatch, exit=${result}, stdout=[${output}], stderr=[${error}]")
endif()
