execute_process(COMMAND "${KIRTON}" "${SOURCE}"
  RESULT_VARIABLE ast_rc OUTPUT_VARIABLE ast_out ERROR_VARIABLE ast_err
  TIMEOUT 20 ENCODING UTF-8)
execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
  RESULT_VARIABLE bytecode_rc OUTPUT_VARIABLE bytecode_out ERROR_VARIABLE bytecode_err
  TIMEOUT 20 ENCODING UTF-8)
if(NOT ast_rc EQUAL 0 OR NOT bytecode_rc EQUAL 0)
  message(FATAL_ERROR "Graphics lifecycle failed: AST rc=${ast_rc}, Bytecode rc=${bytecode_rc}; AST stderr=${ast_err}; Bytecode stderr=${bytecode_err}")
endif()
if(NOT ast_out STREQUAL bytecode_out)
  message(FATAL_ERROR "AST and Bytecode graphics lifecycle output differs: AST=[${ast_out}] Bytecode=[${bytecode_out}]")
endif()
if(NOT ast_out MATCHES "GL_CONTEXT_LIFECYCLE_OK")
  message(FATAL_ERROR "Graphics lifecycle success marker missing: ${ast_out}")
endif()
