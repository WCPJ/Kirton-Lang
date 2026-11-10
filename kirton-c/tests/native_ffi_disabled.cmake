# SPDX-License-Identifier: MPL-2.0
if(NOT DEFINED KIRTON OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "KIRTON and SOURCE required")
endif()
set(native_file "${CMAKE_CURRENT_BINARY_DIR}/kirton_ffi_disabled_probe")
execute_process(COMMAND "${KIRTON}" --native "${SOURCE}" -o "${native_file}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 30)
if("${rc}" STREQUAL "0" OR NOT stderr MATCHES "allow-ffi")
  message(FATAL_ERROR "Native FFI must reject without --allow-ffi: rc=${rc} stderr=${stderr}")
endif()
file(REMOVE "${native_file}" "${native_file}.kirton-native.ll")
