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
      RESULT_VARIABLE reference_result OUTPUT_VARIABLE reference_output
      ERROR_VARIABLE reference_error TIMEOUT 20)
  else()
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE reference_result OUTPUT_VARIABLE reference_output
      ERROR_VARIABLE reference_error TIMEOUT 20)
  endif()
  string(STRIP "${reference_output}" reference_output)
  if(NOT "${reference_result}" STREQUAL "0" OR
     NOT reference_output STREQUAL "9223372036854775808" OR
     NOT "${reference_error}" STREQUAL "")
    message(FATAL_ERROR "${mode} loop result mismatch: exit=${reference_result}, output=[${reference_output}], error=[${reference_error}]")
  endif()
endforeach()
get_filename_component(case_name "${SOURCE}" NAME_WE)
set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_${case_name}${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error TIMEOUT 20)
if(NOT "${compile_result}" STREQUAL "0" OR
   NOT "${compile_output}" STREQUAL "" OR
   NOT "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "native overflow case failed to compile: ${compile_error}")
endif()
execute_process(COMMAND "${native_exe}"
  RESULT_VARIABLE run_result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
string(STRIP "${output}" output)
if(NOT "${run_result}" STREQUAL "0" OR
   NOT "${output}" STREQUAL "9223372036854775808" OR
   NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "native BigInt overflow mismatch, exit=${run_result}, output=[${output}], error=[${error}]")
endif()
