execute_process(
  COMMAND "${KIRTON}" "${SOURCE}"
  RESULT_VARIABLE ast_rc
  OUTPUT_VARIABLE ast_out
  ERROR_VARIABLE ast_err)
if(NOT ast_rc EQUAL 0)
  message(FATAL_ERROR "AST example failed (rc=${ast_rc}, out=${ast_out}, err=${ast_err})")
endif()
if(NOT ast_out MATCHES "${EXPECT}")
  message(FATAL_ERROR "AST example did not print '${EXPECT}' (out=${ast_out})")
endif()

execute_process(
  COMMAND "${KIRTON}" --bytecode "${SOURCE}"
  RESULT_VARIABLE bytecode_rc
  OUTPUT_VARIABLE bytecode_out
  ERROR_VARIABLE bytecode_err)
if(NOT bytecode_rc EQUAL 0)
  message(FATAL_ERROR "Bytecode example failed (rc=${bytecode_rc}, out=${bytecode_out}, err=${bytecode_err})")
endif()
if(NOT bytecode_out MATCHES "${EXPECT}")
  message(FATAL_ERROR "Bytecode example did not print '${EXPECT}' (out=${bytecode_out})")
endif()
