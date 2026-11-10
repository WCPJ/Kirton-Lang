execute_process(
  COMMAND "${KIRTON}" "${SCRIPT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if (result EQUAL 0)
  message(FATAL_ERROR "Expected parse failure, but Kirton returned success")
endif()
set(output "${stdout}${stderr}")
string(FIND "${output}" "${EXPECTED_TEXT}" diagnostic_position)
if (diagnostic_position EQUAL -1)
  message(FATAL_ERROR "Expected diagnostic marker '${EXPECTED_TEXT}', got:\n${output}")
endif()
