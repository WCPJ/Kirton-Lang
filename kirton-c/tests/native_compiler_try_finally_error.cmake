if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "KIRTON and SOURCE are required")
endif()
set(output "${CMAKE_CURRENT_BINARY_DIR}/try_finally_error${CMAKE_EXECUTABLE_SUFFIX}")
execute_process(COMMAND "${KIRTON}" "${SOURCE}"
  RESULT_VARIABLE ast OUTPUT_VARIABLE ast_out ERROR_VARIABLE ast_error TIMEOUT 30 ENCODING UTF-8)
execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
  RESULT_VARIABLE bc OUTPUT_VARIABLE bc_out ERROR_VARIABLE bc_error TIMEOUT 30 ENCODING UTF-8)
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${output}"
  RESULT_VARIABLE compile OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_error
  TIMEOUT 30 ENCODING UTF-8)
if(NOT "${compile}" STREQUAL "0")
  message(FATAL_ERROR "Native compilation failed: ${compile_error}")
endif()
execute_process(COMMAND "${output}" RESULT_VARIABLE native
  OUTPUT_VARIABLE native_out ERROR_VARIABLE native_error TIMEOUT 30 ENCODING UTF-8)
if(NOT "${ast}" STREQUAL "1" OR NOT "${bc}" STREQUAL "1" OR
   NOT "${native}" STREQUAL "1" OR
   NOT "${ast_out}" STREQUAL "FINALLY\n" OR
   NOT "${bc_out}" STREQUAL "FINALLY\n" OR
   NOT "${native_out}" STREQUAL "FINALLY\n" OR
   NOT "${ast_error}" MATCHES "Индекс списка вне диапазона" OR
   NOT "${bc_error}" MATCHES "Индекс списка вне диапазона" OR
   NOT "${native_error}" MATCHES "Индекс списка вне диапазона")
   message(FATAL_ERROR "finally error differs: AST ${ast}: ${ast_out}: ${ast_error}\nBC ${bc}: ${bc_out}: ${bc_error}\nNative ${native}: ${native_out}: ${native_error}")
endif()
