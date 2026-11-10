if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "KIRTON and SOURCE are required")
endif()
if(CMAKE_HOST_WIN32)
  set(native_suffix ".exe")
else()
  set(native_suffix "")
endif()

set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_runtime_error${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error TIMEOUT 20 ENCODING UTF-8)
if(NOT "${compile_result}" STREQUAL "0" OR
   NOT "${compile_output}" STREQUAL "" OR
   NOT "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "native runtime-error case failed to compile: exit=${compile_result}, output=[${compile_output}], error=[${compile_error}]")
endif()
execute_process(COMMAND "${native_exe}"
  RESULT_VARIABLE run_result OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 20 ENCODING UTF-8)
if(NOT "${run_result}" STREQUAL "71" OR
   NOT "${output}" MATCHES "Kirton: uninitialized variable" OR
   NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "runtime cleanup/error mismatch: exit=${run_result}, output=[${output}], error=[${error}]")
endif()
