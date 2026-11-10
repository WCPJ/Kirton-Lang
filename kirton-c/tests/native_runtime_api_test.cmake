if(NOT DEFINED TEST_EXECUTABLE)
  message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()
execute_process(COMMAND "${TEST_EXECUTABLE}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 20 ENCODING UTF-8)
string(STRIP "${output}" output)
if(NOT "${result}" STREQUAL "0" OR
   NOT "${output}" STREQUAL "Живая строка Возвращённая строка" OR
   NOT "${error}" STREQUAL "")
  message(FATAL_ERROR "native runtime root/return test failed: exit=${result}, output=[${output}], error=[${error}]")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" abort
  RESULT_VARIABLE abort_result OUTPUT_VARIABLE abort_output ERROR_VARIABLE abort_error
  TIMEOUT 20 ENCODING UTF-8)
if(NOT "${abort_result}" STREQUAL "71" OR
   NOT "${abort_output}" STREQUAL "" OR
   NOT "${abort_error}" STREQUAL "")
  message(FATAL_ERROR "native runtime abort cleanup failed: exit=${abort_result}, output=[${abort_output}], error=[${abort_error}]")
endif()
