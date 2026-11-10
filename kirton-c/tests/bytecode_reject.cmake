execute_process(COMMAND "${KIRTON}" --bytecode -e main "${SOURCE}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err ENCODING UTF-8)
if(rc EQUAL 0 OR NOT err MATCHES "${EXPECT}")
  message(FATAL_ERROR "Unsupported bytecode syntax was not rejected clearly (rc=${rc}, stderr=${err})")
endif()
