if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE OR NOT DEFINED EXPECT_ERROR)
  message(FATAL_ERROR "KIRTON, SOURCE, EXPECT_ERROR are required")
endif()
set(out "${CMAKE_CURRENT_BINARY_DIR}/native_module_rejected")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${out}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30
  ENCODING UTF-8)
if("${result}" STREQUAL "0" OR NOT "${error}" MATCHES "${EXPECT_ERROR}")
  message(FATAL_ERROR "expected diagnostic ${EXPECT_ERROR}: exit=${result}, out=${output}, err=${error}")
endif()
