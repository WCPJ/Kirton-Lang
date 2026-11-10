execute_process(
  COMMAND "${KIRTON_EXE}" "${SCRIPT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error_output)
if(result EQUAL 0)
  message(FATAL_ERROR "Expected an unhandled runtime error")
endif()
if(NOT output MATCHES "UNHANDLED_ALWAYS_RAN")
  message(FATAL_ERROR "The always block did not run. Output: ${output}${error_output}")
endif()
if(output MATCHES "UNHANDLED_ALWAYS_FAIL_AFTER")
  message(FATAL_ERROR "Execution continued after an unhandled error")
endif()
