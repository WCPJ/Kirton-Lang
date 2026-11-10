if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE OR NOT DEFINED CASE)
  message(FATAL_ERROR "KIRTON, SOURCE and CASE are required")
endif()
if(CMAKE_HOST_WIN32)
  set(native_suffix ".exe")
else()
  set(native_suffix "")
endif()
foreach(mode IN ITEMS ast bytecode)
  if(mode STREQUAL "ast")
    execute_process(COMMAND "${KIRTON}" "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
  else()
    execute_process(COMMAND "${KIRTON}" --bytecode "${SOURCE}"
      RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
  endif()
  if("${result}" STREQUAL "0" OR NOT "${out}" STREQUAL "" OR
     "${err}" STREQUAL "")
    message(FATAL_ERROR "${mode}: expected failure and diagnostic, got exit=${result}, stdout=${out}, stderr=${err}")
  endif()
endforeach()
set(exe "${CMAKE_CURRENT_BINARY_DIR}/native_closure_negative_${CASE}${native_suffix}")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${exe}"
  RESULT_VARIABLE compiled OUTPUT_VARIABLE comp_out ERROR_VARIABLE comp_err TIMEOUT 25)
if(NOT "${compiled}" STREQUAL "0" OR NOT "${comp_err}" STREQUAL "")
  message(FATAL_ERROR "native compilation failed unexpectedly: exit=${compiled} ${comp_err}")
endif()
execute_process(COMMAND "${exe}" RESULT_VARIABLE status
  OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
if("${status}" STREQUAL "0" OR NOT "${out}" STREQUAL "" OR
   NOT "${err}" MATCHES "Kirton Native")
  message(FATAL_ERROR "native: expected runtime failure, got exit=${status}, stdout=${out}, stderr=${err}")
endif()
