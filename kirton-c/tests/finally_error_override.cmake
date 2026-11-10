execute_process(
  COMMAND "${KIRTON_EXE}" "${SCRIPT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error_output)
set(combined_output "${output}\n${error_output}")
if(result EQUAL 0)
  message(FATAL_ERROR "Expected cleanup failure to propagate")
endif()
if(NOT output MATCHES "FINALLY_OVERRIDE_CAUGHT")
  message(FATAL_ERROR "The body error was not caught. Output: ${output}${error_output}")
endif()
if(output MATCHES "FINALLY_ERROR_FAIL_AFTER")
  message(FATAL_ERROR "Execution continued after cleanup failure")
endif()
if(NOT output MATCHES "OUTER_ALWAYS_RAN")
  message(FATAL_ERROR "The enclosing always block did not run. Output: ${output}")
endif()
