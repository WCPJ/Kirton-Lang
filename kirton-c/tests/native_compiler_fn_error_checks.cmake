if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE OR NOT DEFINED ERROR_PATTERN)
  message(FATAL_ERROR "KIRTON, SOURCE and ERROR_PATTERN are required")
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
  if("${result}" STREQUAL "0" OR NOT "${output}" STREQUAL "" OR
     NOT "${error}" MATCHES "${ERROR_PATTERN}")
    message(FATAL_ERROR "${mode}: expected error /${ERROR_PATTERN}/, exit=${result}, stdout=[${output}], stderr=[${error}]")
  endif()
endforeach()

set(native_exe "${CMAKE_CURRENT_BINARY_DIR}/native_function_reject${native_suffix}")
file(REMOVE "${native_exe}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_exe}"
  RESULT_VARIABLE native_result OUTPUT_VARIABLE native_output
  ERROR_VARIABLE native_error TIMEOUT 20)
if("${native_result}" STREQUAL "0" OR NOT "${native_output}" STREQUAL "" OR
   NOT "${native_error}" MATCHES "${NATIVE_ERROR_PATTERN}")
  message(FATAL_ERROR "native: expected error /${NATIVE_ERROR_PATTERN}/, exit=${native_result}, stdout=[${native_output}], stderr=[${native_error}]")
endif()
if(EXISTS "${native_exe}")
  message(FATAL_ERROR "native rejection unexpectedly left an executable")
endif()
