set(oom_command "${ALLOC_TEST_EXE}" "${ALLOC_KIND}")
if(DEFINED ENV{KIRTON_OOM_DISABLE_LSAN})
  set(oom_command "${CMAKE_COMMAND}" -E env "ASAN_OPTIONS=detect_leaks=0" ${oom_command})
endif()
execute_process(
  COMMAND ${oom_command}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error_output)
if(result EQUAL 0)
  message(FATAL_ERROR "Expected ${ALLOC_KIND} allocation to fail")
endif()
if(NOT error_output MATCHES "OOM")
  message(FATAL_ERROR "OOM diagnostic missing: ${output}${error_output}")
endif()
if(error_output MATCHES "AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:")
  message(FATAL_ERROR "Allocator OOM child emitted sanitizer diagnostics: ${output}${error_output}")
endif()
