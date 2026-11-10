execute_process(COMMAND "${KIRTON}" "${SOURCE}"
  WORKING_DIRECTORY "${WORKING_DIR}"
  RESULT_VARIABLE ast_rc OUTPUT_VARIABLE ast_out ERROR_VARIABLE ast_err
  TIMEOUT 20 ENCODING UTF-8)
execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
  WORKING_DIRECTORY "${WORKING_DIR}"
  RESULT_VARIABLE bytecode_rc OUTPUT_VARIABLE bytecode_out ERROR_VARIABLE bytecode_err
  TIMEOUT 20 ENCODING UTF-8)
if(NOT ast_rc EQUAL 0 OR NOT bytecode_rc EQUAL 0)
  message(FATAL_ERROR "core_smoke failed: AST rc=${ast_rc}, Bytecode rc=${bytecode_rc}; AST stderr=${ast_err}; Bytecode stderr=${bytecode_err}")
endif()
if(NOT ast_out STREQUAL bytecode_out)
  message(FATAL_ERROR "core_smoke AST/Bytecode output differs: AST=[${ast_out}] Bytecode=[${bytecode_out}]")
endif()
if(ast_out MATCHES "FAIL:" OR NOT ast_out MATCHES "ИТОГ: ВСЁ ОК")
  message(FATAL_ERROR "core_smoke did not report all assertions passing: ${ast_out}")
endif()
