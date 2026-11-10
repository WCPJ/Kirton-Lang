if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "KIRTON and SOURCE are required")
endif()
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o
  "${CMAKE_CURRENT_BINARY_DIR}/try_unwind_reject${CMAKE_EXECUTABLE_SUFFIX}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30 ENCODING UTF-8)
if("${result}" STREQUAL "0" OR NOT "${error}" MATCHES "requires unwind support")
   message(FATAL_ERROR "expected explicit unsupported unwind diagnostic, got ${result}: ${error}")
endif()
