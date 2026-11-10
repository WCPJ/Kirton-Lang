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
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
      TIMEOUT 20 ENCODING UTF-8)
  else()
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
      TIMEOUT 20 ENCODING UTF-8)
  endif()
  if("${result}" STREQUAL "0" OR NOT "${output}" STREQUAL "" OR
     NOT "${error}" MATCHES "Побитовая операция|Сдвиг требует")
    message(FATAL_ERROR "${mode}: invalid bitwise operation: exit=${result}, stdout=[${output}], stderr=[${error}]")
  endif()
endforeach()
get_filename_component(case_name "${SOURCE}" NAME_WE)
set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_${case_name}${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_error
  TIMEOUT 20 ENCODING UTF-8)
if(NOT "${compile_result}" STREQUAL "0" OR
   NOT "${compile_out}" STREQUAL "" OR NOT "${compile_error}" STREQUAL "")
  message(FATAL_ERROR "Native bitwise error case failed to compile: ${compile_error}")
endif()
execute_process(COMMAND "${native_exe}"
  RESULT_VARIABLE native_result OUTPUT_VARIABLE native_output ERROR_VARIABLE native_error
  TIMEOUT 20 ENCODING UTF-8)
if(NOT "${native_result}" STREQUAL "70" OR NOT "${native_output}" STREQUAL "" OR
   NOT "${native_error}" MATCHES "Побитовая операция|Сдвиг требует")
  message(FATAL_ERROR "Native bitwise error mismatch: exit=${native_result}, stdout=[${native_output}], stderr=[${native_error}]")
endif()
