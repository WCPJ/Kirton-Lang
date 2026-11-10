# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0/.
cmake_minimum_required(VERSION 3.15)
project(kirton VERSION 1.3.0 LANGUAGES C)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" KIRTON_VERSION_FILE)
string(STRIP "${KIRTON_VERSION_FILE}" KIRTON_VERSION_FROM_FILE)
set(KIRTON_PRODUCT_VERSION "${KIRTON_VERSION_FROM_FILE}" CACHE STRING "Version shown by the Kirton executable")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(CTest)
include(GNUInstallDirs)

# Для IDE/clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Флаг: собирать ли старый модуль буфера/BMP
option(KIRTON_LEGACY_DRAW2D "Build legacy CPU framebuffer module (mod_draw2d.c)" OFF)
option(KIRTON_BUILD_FUZZERS "Build Clang/libFuzzer native-input fuzz targets" OFF)
set(_kirton_clang_names clang clang-23 clang-22 clang-21 clang-20 clang-19)
if (WIN32 AND DEFINED ENV{LOCALAPPDATA})
  find_program(KIRTON_CLANG_EXECUTABLE NAMES ${_kirton_clang_names}
    HINTS "$ENV{LOCALAPPDATA}/Kirton/LLVM-23.1.5/bin")
else()
  find_program(KIRTON_CLANG_EXECUTABLE NAMES ${_kirton_clang_names})
endif()
set(_kirton_clang_runtime_default "")
if (KIRTON_CLANG_EXECUTABLE)
  file(TO_CMAKE_PATH "${KIRTON_CLANG_EXECUTABLE}" _kirton_clang_runtime_default)
endif()
set(KIRTON_CLANG_RUNTIME_PATH "${_kirton_clang_runtime_default}" CACHE STRING
  "Build-tree fallback for Clang; installed SDKs resolve Clang from LOCALAPPDATA or PATH")
if (NOT KIRTON_CLANG_EXECUTABLE)
  message(STATUS "Clang was not found; Native executable generation tests will not be registered")
endif()

# ---------------- Библиотека ядра ----------------
set(KIRTON_CORE_SOURCES
    src/alloc_test.c
    src/utf8.c
    src/number.c
    src/lexer.c
    src/ast.c
    src/parser.c
    src/value.c
    src/eval.c
    src/bytecode.c
    src/native_compiler.c
    src/native_runtime.c
    src/ffi.c
    src/native.c
    src/source_cache.c
    src/callstack.c
    src/diag.c

    # нативные модули рантайма:
    src/mod_fs.c
    src/mod_time.c
    src/mod_math.c
    src/mod_math3d.c
    src/mod_http.c
    src/mod_process.c
    src/mod_os.c
    src/mod_path.c
    src/mod_str.c
    src/mod_runtime.c
    src/mod_memory.c
    src/mod_native_resource.c

    # новые: коллекции/утилиты/JSON/regex/таймеры
    src/mod_list.c
    src/mod_dict.c
    src/mod_json.c
    src/mod_regex.c
    src/mod_timers.c

    # новые ресурсы/звук
    src/mod_bitmap.c
    src/mod_sound.c
    src/stb_image.c

    # новые: системная инфа и pacman-обёртка для KirtonOS
    src/mod_sys.c
    src/mod_pkg.c
)

# Оконный модуль и буфер обмена — только на Windows
if (WIN32)
  list(APPEND KIRTON_CORE_SOURCES
       src/mod_window.c
       src/mod_clipboard.c
       src/mod_gl.c
       src/mod_d3d11.c)
endif()

# Старый модуль (по желанию)
if (KIRTON_LEGACY_DRAW2D)
  list(APPEND KIRTON_CORE_SOURCES src/mod_draw2d.c)
endif()

add_library(kirton_core STATIC ${KIRTON_CORE_SOURCES})
target_include_directories(kirton_core PUBLIC include third_party)
set_target_properties(kirton_core PROPERTIES OUTPUT_NAME kirton)
set(KIRTON_RUNTIME_LIBRARY_PATH "$<TARGET_FILE:kirton_core>" CACHE STRING
  "Build-tree fallback path for the static Runtime used by Native compilation")
if (WIN32)
  if (MSVC)
    set_property(TARGET kirton_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # Clang 20's Windows driver can add libcmt even when /MD is selected.
    set(KIRTON_NATIVE_RUNTIME_LINK_FLAGS "-fms-runtime-lib=$<IF:$<CONFIG:Debug>,dll_dbg,dll> -Wl,/NODEFAULTLIB:libcmt -Wl,/NODEFAULTLIB:libcmtd -Wl,/NODEFAULTLIB:libucrt -Wl,/NODEFAULTLIB:libucrtd -Wl,/STACK:8388608 -lwinhttp -luser32 -lgdi32 -lwinmm -lopengl32 -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -ldxguid")
  else()
    set(KIRTON_NATIVE_RUNTIME_LINK_FLAGS "-lwinhttp -luser32 -lgdi32 -lwinmm -lopengl32 -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -ldxguid")
  endif()
elseif (UNIX)
  set(KIRTON_NATIVE_RUNTIME_LINK_FLAGS "-lm -ldl")
  if (CMAKE_C_FLAGS MATCHES "sanitize=address")
    string(APPEND KIRTON_NATIVE_RUNTIME_LINK_FLAGS " -fsanitize=address")
  endif()
endif()
# Pass the sanitizers used to build kirton_core to Clang when linking a
# generated POSIX executable. A sanitized static archive otherwise has
# unresolved __asan_*/__ubsan_* symbols in the Native link step.
set(KIRTON_NATIVE_POSIX_SANITIZER_FLAGS "")
if (UNIX AND NOT WIN32)
  set(_kirton_sanitizer_cflags "${CMAKE_C_FLAGS}")
  if (CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _kirton_build_type)
    string(APPEND _kirton_sanitizer_cflags " ${CMAKE_C_FLAGS_${_kirton_build_type}}")
  endif()
  string(REGEX MATCHALL "-fsanitize=[A-Za-z0-9_,]+" _kirton_sanitize_options "${_kirton_sanitizer_cflags}")
  set(_kirton_sanitizers "")
  foreach(_kirton_option IN LISTS _kirton_sanitize_options)
    string(REPLACE "-fsanitize=" "" _kirton_option "${_kirton_option}")
    string(REPLACE "," ";" _kirton_option "${_kirton_option}")
    list(APPEND _kirton_sanitizers ${_kirton_option})
  endforeach()
  if (_kirton_sanitizers)
    list(REMOVE_DUPLICATES _kirton_sanitizers)
    list(JOIN _kirton_sanitizers "," _kirton_sanitizer_list)
    set(KIRTON_NATIVE_POSIX_SANITIZER_FLAGS "-fsanitize=${_kirton_sanitizer_list}")
  endif()
endif()
set(KIRTON_NATIVE_ASAN_LINK 0)
if (MSVC AND CMAKE_C_FLAGS MATCHES "sanitize=address")
  set(KIRTON_NATIVE_ASAN_LINK 1)
  get_filename_component(_kirton_cl_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_kirton_cl_host_dir "${_kirton_cl_dir}" DIRECTORY)
  get_filename_component(_kirton_cl_bin_dir "${_kirton_cl_host_dir}" DIRECTORY)
  get_filename_component(_kirton_msvc_dir "${_kirton_cl_bin_dir}" DIRECTORY)
  file(TO_CMAKE_PATH "${_kirton_msvc_dir}/lib/x64" KIRTON_NATIVE_ASAN_MSVCLIB_DIR)
  file(TO_CMAKE_PATH "${_kirton_cl_dir}" KIRTON_NATIVE_ASAN_DLL_DIR)
  set(_kirton_windows_kits "$ENV{CMAKE_WINDOWS_KITS_10_DIR}")
  if (NOT _kirton_windows_kits)
    set(_kirton_windows_kits "C:/Program Files (x86)/Windows Kits/10")
  endif()
  set(_kirton_sdk_root "${_kirton_windows_kits}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  file(TO_CMAKE_PATH "${_kirton_sdk_root}/ucrt/x64" KIRTON_NATIVE_ASAN_UCRT_DIR)
  file(TO_CMAKE_PATH "${_kirton_sdk_root}/um/x64" KIRTON_NATIVE_ASAN_UM_DIR)
  set(KIRTON_NATIVE_ASAN_LINK_LIBS "winhttp.lib user32.lib gdi32.lib winmm.lib opengl32.lib d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib dxguid.lib")
endif()
if (UNIX)
  target_link_libraries(kirton_core PUBLIC m ${CMAKE_DL_LIBS})
endif()

# ---------------- Исполняемый файл ----------------
add_executable(kirton src/main.c)
target_link_libraries(kirton PRIVATE kirton_core)
target_compile_definitions(kirton PRIVATE KIRTON_VERSION="${KIRTON_PRODUCT_VERSION}")
target_compile_definitions(kirton_core PRIVATE
  KIRTON_CLANG_PATH="${KIRTON_CLANG_RUNTIME_PATH}"
  KIRTON_RUNTIME_LIBRARY_PATH="${KIRTON_RUNTIME_LIBRARY_PATH}"
  KIRTON_RUNTIME_LINK_FLAGS="${KIRTON_NATIVE_RUNTIME_LINK_FLAGS}"
  KIRTON_NATIVE_POSIX_SANITIZER_FLAGS="${KIRTON_NATIVE_POSIX_SANITIZER_FLAGS}"
  KIRTON_NATIVE_ASAN_LINK=${KIRTON_NATIVE_ASAN_LINK}
  KIRTON_NATIVE_ASAN_LINKER_PATH="${CMAKE_C_COMPILER}"
  KIRTON_NATIVE_ASAN_MSVCLIB_DIR="${KIRTON_NATIVE_ASAN_MSVCLIB_DIR}"
  KIRTON_NATIVE_ASAN_UCRT_DIR="${KIRTON_NATIVE_ASAN_UCRT_DIR}"
  KIRTON_NATIVE_ASAN_UM_DIR="${KIRTON_NATIVE_ASAN_UM_DIR}"
  KIRTON_NATIVE_ASAN_DLL_DIR="${KIRTON_NATIVE_ASAN_DLL_DIR}"
  KIRTON_NATIVE_ASAN_LINK_LIBS="${KIRTON_NATIVE_ASAN_LINK_LIBS}")

install(TARGETS kirton RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS kirton_core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/kirton DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
  docs/GETTING_STARTED.md
  docs/KIRTON_1_3_GUIDE.md
  docs/KIRTON_1_2_GUIDE.md
  docs/KIRTON_OOM_CONTRACT.md
  docs/KIRTON_1_1_GUIDE.md
  docs/SYNTAX_V1_1.md
  docs/TECHNICAL_REFERENCE_V1_1.md
  docs/LANGUAGE_REFERENCE.md
  docs/CLI_REFERENCE.md
  docs/EMBEDDING.md
  docs/LICENSING.md
  docs/KNOWN_LIMITATIONS.md
  docs/GENTOO.md
  docs/SYNTAX_V1.md
  docs/CONFORMANCE_V1.md
  docs/RELEASE_GATE_V1.md
  docs/GETTING_STARTED_V1.md
  docs/PLATFORMS_V1.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kirton/docs)
install(FILES README.md VERSION LICENSE LICENSE_STATUS.md THIRD_PARTY_NOTICES.md CHANGELOG.md
  "RELEASE_NOTES_${KIRTON_VERSION_FROM_FILE}.md"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kirton)
install(DIRECTORY examples/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kirton/examples
  FILES_MATCHING PATTERN "*.kir" PATTERN "README.md")

if (KIRTON_BUILD_FUZZERS)
  if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KIRTON_BUILD_FUZZERS requires Clang/libFuzzer")
  endif()
  target_compile_options(kirton_core PRIVATE -fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer)
  target_link_options(kirton_core INTERFACE -fsanitize=address)
  foreach(fuzzer json bmp)
    add_executable(kirton_fuzz_${fuzzer} tests/fuzz_${fuzzer}_libfuzzer.c)
    target_compile_options(kirton_fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer,address -fno-omit-frame-pointer)
    target_link_options(kirton_fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer,address)
    target_link_libraries(kirton_fuzz_${fuzzer} PRIVATE kirton_core)
  endforeach()
endif()

# Если включили старый модуль — сообщим об этом C/CPP
if (KIRTON_LEGACY_DRAW2D)
  target_compile_definitions(kirton_core PUBLIC KIRTON_LEGACY_DRAW2D)
  target_compile_definitions(kirton      PUBLIC KIRTON_LEGACY_DRAW2D)
endif()

# ---------------- Платформенные настройки ----------------
if (MSVC)
  target_compile_options(kirton_core PRIVATE /utf-8 /W4 /permissive-)
  target_compile_options(kirton      PRIVATE /utf-8 /W4 /permissive-)
  # Match the usual 8 MiB main-thread stack reserve on Linux. MSVC Debug's
  # large evaluator frames otherwise overflow on ordinary recursive programs.
  target_link_options(kirton PRIVATE /STACK:8388608)
  target_compile_definitions(kirton_core PUBLIC UNICODE _UNICODE)
  target_compile_definitions(kirton      PUBLIC UNICODE _UNICODE)
  # HTTP (WinHTTP), окно/клипборд/рендер и звук (winmm)
  target_link_libraries(kirton_core PUBLIC winhttp user32 gdi32 winmm opengl32 d3d11 dxgi d3dcompiler windowscodecs ole32 dxguid)
else()
  target_compile_options(kirton_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_options(kirton      PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if (WIN32) # MinGW-путь
    target_link_libraries(kirton_core PUBLIC winhttp user32 gdi32 winmm opengl32 d3d11 dxgi d3dcompiler windowscodecs ole32 dxguid)
  endif()
endif()

if (BUILD_TESTING)
  add_executable(kirton_native_runtime_api_test tests/native_runtime_api.c)
  target_include_directories(kirton_native_runtime_api_test PRIVATE src)
  target_link_libraries(kirton_native_runtime_api_test PRIVATE kirton_core)
  add_test(NAME kirton_native_runtime_api
    COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:kirton_native_runtime_api_test>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_runtime_api_test.cmake)
  add_test(NAME kirton_native_compiler_llvm_emit
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_foundation.kir
      -DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/native-foundation.ll
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_emit.cmake)
  add_test(NAME kirton_native_compiler_reference_arithmetic
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_arithmetic.kir
      "-DEXPECT=102|11"
      -DNATIVE_ENABLED=0
      -DTEST_NAME=reference_arithmetic
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
  add_test(NAME kirton_native_compiler_reference_negative
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_negative.kir
      "-DEXPECT=-9|-3|-36"
      -DNATIVE_ENABLED=0
      -DTEST_NAME=reference_negative
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
  add_test(NAME kirton_native_compiler_reference_foundation
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_foundation.kir
      -DEXPECT=42
      -DNATIVE_ENABLED=0
      -DTEST_NAME=reference_foundation
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
  add_test(NAME kirton_native_compiler_reference_boundary
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_boundary.kir
      "-DEXPECT=9223372036854775807|-9223372036854775808"
      -DNATIVE_ENABLED=0
      -DTEST_NAME=reference_boundary
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
  add_test(NAME kirton_native_compiler_reject_unknown
    COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_unknown_variable.kir
      "-DEXPECT=unknown variable"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_reject.cmake)
  add_test(NAME kirton_native_compiler_llvm_arithmetic_ops
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_arithmetic.kir
      -DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/native-arithmetic.ll
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_emit_arithmetic.cmake)
  if (KIRTON_CLANG_EXECUTABLE)
    function(add_native_diff native_name native_source native_expect)
      add_test(NAME kirton_native_${native_name}_diff
        COMMAND ${CMAKE_COMMAND}
          -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${native_source}
          "-DEXPECT=${native_expect}"
          -DNATIVE_ENABLED=1
          -DTEST_NAME=${native_name}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
    endfunction()
    add_native_diff(foundation native_compiler_foundation.kir "42")
    add_native_diff(arithmetic native_compiler_arithmetic.kir "102|11")
    add_native_diff(negative native_compiler_negative.kir "-9|-3|-36")
    add_native_diff(boundary native_compiler_boundary.kir "9223372036854775807|-9223372036854775808")
    add_test(NAME kirton_native_bigint_overflow
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_overflow.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_overflow.cmake)
  endif()

  function(add_native_control_reference case_name source_file expected_output)
    add_test(NAME kirton_native_reference_${case_name}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${source_file}
        "-DEXPECT=${expected_output}"
        -DNATIVE_ENABLED=0
        -DTEST_NAME=reference_${case_name}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
    if (KIRTON_CLANG_EXECUTABLE)
      add_native_diff(${case_name} ${source_file} "${expected_output}")
    endif()
  endfunction()
  function(add_native_dynamic_control_reference case_name source_file expected_output)
    add_test(NAME kirton_native_reference_${case_name}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${source_file}
        "-DEXPECT=${expected_output}"
        -DNATIVE_ENABLED=0
        -DTEST_NAME=reference_${case_name}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_diff.cmake)
    if (KIRTON_CLANG_EXECUTABLE)
      add_native_diff(${case_name} ${source_file} "${expected_output}")
    endif()
  endfunction()
  add_native_control_reference(control_flow native_compiler_control_flow.kir "5050")
  add_native_control_reference(if_true native_compiler_if_true.kir "1|1")
  add_native_control_reference(if_false native_compiler_if_false.kir "1")
  add_native_control_reference(nested_if native_compiler_nested_if.kir "2")
  add_native_control_reference(zero_loop native_compiler_zero_loop.kir "0")
  add_native_control_reference(one_loop native_compiler_one_loop.kir "1")
  add_native_control_reference(loop_10000 native_compiler_10000_loop.kir "10000")
  add_native_control_reference(nested_loops native_compiler_nested_loops.kir "35")
  add_native_control_reference(block_scope native_compiler_block_scope.kir "9")
  add_native_control_reference(comparisons native_compiler_comparisons.kir "1|1|1|1|1|1|1|1")
  add_native_control_reference(bool native_compiler_bool.kir "0|1")
  if (KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_bigint_loop_growth
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_loop_overflow.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_overflow.cmake)
  endif()
  add_native_control_reference(factorial native_compiler_factorial.kir "120")
  add_native_control_reference(fn_forward native_compiler_fn_forward.kir "3")
  add_native_control_reference(fn_zero native_compiler_fn_zero.kir "42")
  add_native_control_reference(fn_multi native_compiler_fn_multi.kir "42|25|6")
  add_native_control_reference(fn_void native_compiler_fn_void.kir "7|7")
  add_native_control_reference(fn_fallthrough native_compiler_fn_fallthrough.kir "9")
  add_native_control_reference(fn_frames native_compiler_fn_frames.kir "2|11|50")
  add_native_control_reference(fn_same_locals native_compiler_fn_same_locals.kir "3|4|11")
  add_native_control_reference(fn_globals native_compiler_fn_globals.kir "1|2|2")
  add_native_control_reference(fn_control native_compiler_fn_control.kir "15|6")
  add_native_control_reference(fn_mutual native_compiler_fn_mutual.kir "1|0")
  add_native_control_reference(fn_bool native_compiler_fn_bool.kir "1|0")
  add_native_control_reference(fn_duplicate native_compiler_fn_duplicate.kir "2")
  add_native_dynamic_control_reference(string_literal native_compiler_string_literal.kir "Киртон")
  add_native_dynamic_control_reference(string_condition native_compiler_unsupported_condition.kir "1")
  add_native_dynamic_control_reference(string_return native_compiler_string_return.kir "Киртон")
  add_native_dynamic_control_reference(string_branch_return native_compiler_string_branch_return.kir "Успешно")
  add_native_dynamic_control_reference(string_gc_recursion native_compiler_string_gc_recursion.kir "Привет, Киртон 🧭|Повторный вызов|Ещё один вызов")
  add_native_dynamic_control_reference(string_gc_temporary native_compiler_string_gc_temporary.kir "Живая строка 🐢")
  add_native_control_reference(bigint_arithmetic native_compiler_bigint_arithmetic.kir "9223372036854775808|9223372036854775805|27670116110564327424|3074457345618258602|2|истина|истина|истина")
  add_native_control_reference(bigint_negative native_compiler_bigint_negative.kir "-1|27670116110564327427|-1844674407370955162|1|-3|истина")
  add_native_control_reference(bigint_256 native_compiler_bigint_256.kir "115792089237316195423570985008687907853269984665640564039457584007913129639936|115792089237316195423570985008687907853269984665640564039457584007913129639937|347376267711948586270712955026063723559809953996921692118372752023739388919808")
  add_native_control_reference(bigint_mixed_float native_compiler_bigint_mixed_float.kir "4.6116860184273879e+18|9.2233720368547758e+18|истина|истина")
  add_native_control_reference(bigint_functions native_compiler_bigint_functions.kir "9223372036854775809|9223372036854775808|42|9223372036854775808|9223372036854775809|265252859812191058636308480000000")
  add_native_control_reference(bigint_gc_temporary native_compiler_bigint_gc_temporary.kir "9223372036854775808|115792089237316195423570985008687907853269984665640564039457584007913129639936")
  add_native_control_reference(gc_global_alias native_compiler_gc_global_alias.kir "9223372036854775808|9223372036854775872|18446744073709551680|9223372036854775872|9223372036854775808|Начало|Конец|Конец|Начало")
  add_native_control_reference(string_ops native_compiler_string_ops.kir "Киртон|истина|ложь|истина|истина|истина|🧭 Киртон|начало-конец|истина")
  add_native_control_reference(global_anchor native_compiler_global_anchor.kir "9223372036854775808|9223372036854780808|9223372036854780808|до|до-после|до-после")
  add_native_control_reference(bitwise native_compiler_bitwise.kir "400000000000000000000|25000000000000000000|-1|-16|-18446744073709551616|-1|-1|0|8|14|6|3|-5|-8|1|8|73786976294838206719")
  add_native_control_reference(logic native_compiler_logic.kir "ложь|истина|0|истина|истина|2|истина|ложь|истина|истина|истина|ложь|истина|истина|истина|ложь|истина")
  add_native_control_reference(mixed_stress native_compiler_mixed_stress.kir "1554806|истина|ложь|истина|0")
  add_native_control_reference(print_boolean_only native_compiler_print_boolean_only.kir "истина|ложь|истина|ложь|ложь")
  add_native_control_reference(print_multiple_i64 native_compiler_print_multiple_i64.kir "1 2 3|0")
  add_native_control_reference(equality_mixed_types native_compiler_equality_mixed_types.kir "ложь|2|истина|4|ложь|истина|истина|ложь")
  add_native_control_reference(nil_values native_compiler_nil_values.kir "пусто|истина|ложь|истина|истина|истина|ложь|пусто|пусто|верно")
  add_native_control_reference(nil_gc native_compiler_nil_gc.kir "2000|пусто")
  add_native_control_reference(nil_return native_compiler_nil_return.kir "пусто|пусто|пусто|пусто|пусто|истина")
  add_native_control_reference(dynamic_vars native_compiler_dynamic_vars.kir
    "7|Киртон|истина|пусто|9223372036854775808|9223372036854775810|3|привет|ложь|пусто")
  add_native_control_reference(dynamic_functions native_compiler_dynamic_functions.kir
    "1|1|тест|тест|истина|истина|пусто|пусто|строка|42")
  add_native_control_reference(dynamic_ops native_compiler_dynamic_ops.kir
    "8|Киртон|9223372036854775810|истина|ложь|истина")
  add_native_control_reference(dynamic_invalid native_compiler_dynamic_invalid.kir
    "да!|пусто|7|пусто|истина|пусто|-3|пусто")
  add_native_control_reference(dynamic_fallthrough native_compiler_dynamic_fallthrough.kir
    "что-то|пусто|55|пусто")
  add_native_control_reference(dynamic_recursive native_compiler_dynamic_recursive.kir
    "Привет 🧭|9223372036854775808|истина|пусто")
  add_native_control_reference(dynamic_gc_stress native_compiler_dynamic_gc_stress.kir
    "Начало 🐢|9223372036854777807|Другой 🧭|9223372036854777809|Начало 🐢")
  add_native_control_reference(dynamic_shortcircuit native_compiler_dynamic_shortcircuit.kir
    "ложь|истина|истина|истина|2")
  add_native_control_reference(dynamic_static_mismatch native_compiler_dynamic_static_mismatch.kir
    "пусто|пусто|пусто|пусто|пусто|10")

  # Native collections: positive AST / Bytecode / compiled ELF differential cases.
  add_native_control_reference(collection_simple_list native_compiler_unsupported.kir "[1, 2]")
  add_native_control_reference(collection_simple_dict native_compiler_out_of_range.kir "{ключ: 1}")
  add_native_control_reference(collections_alias native_compiler_collections_alias.kir "изменено 9223372036854775808 3 конец изменено|истина истина|2")
  add_native_control_reference(collections_attrs native_compiler_collections_attrs.kir "начало 2 пусто|новое 5|финал финал финал")
  add_native_control_reference(collections_dyn_reassign native_compiler_collections_dyn_reassign.kir "1|3|Р й|истина пусто")
  add_native_control_reference(collections_functions native_compiler_collections_functions.kir "11|first 9223372036854775808 5|9 11")
  add_native_control_reference(collections_gc_stress native_compiler_collections_gc_stress.kir "2000 1999 2000|начало 100000000000000000000000000000000")
  add_native_control_reference(collections_literals native_compiler_collections_literals.kir "[1, Киртон 🧭, пусто, [9223372036854775808, ложь]]|1 Киртон 🧭 пусто 9223372036854775808 ложь|{alpha: 8, β: [2, 3], empty: {}} 8 3 пусто|истина истина")
  add_native_control_reference(collections_order native_compiler_collections_order.kir "12345|1234567 7 5")
  add_native_control_reference(collections_utf8_index native_compiler_collections_utf8_index.kir "Ж 😀 Щ - к|🧭 ёж 123 пусто")
  # Native 0.6C: loop-control, UTF-8 enumeration and VM-owned collection builtins.
  add_native_control_reference(foreach_control native_compiler_foreach_control.kir "LOOP_CONTROL_OK")
  add_native_control_reference(foreach_builtins native_compiler_foreach_builtins.kir
    "0|истина|истина|2 1 2|6 3|0|истина|истина 1 8|истина|ложь 0|ложь 0")
  add_native_control_reference(foreach_nested native_compiler_foreach_nested.kir
    "8 Я🧭ё цель пусто|8")
  add_native_control_reference(foreach_mutation native_compiler_foreach_mutation.kir
    "3 xx|31125 250")
  add_native_control_reference(foreach_continue_stress native_compiler_foreach_continue_stress.kir
    "3600")
  add_native_control_reference(foreach_empty native_compiler_foreach_empty.kir "0")
  add_native_control_reference(parameter_widen native_compiler_parameter_widen.kir "10 45 0")
  # Native 0.7: shared VM user types, enum identity, widened core stdlib,
  # and consistent precedence between registered natives and user functions.
  add_native_control_reference(user_type_identity user_types.kir "USER_TYPES_OK")
  add_native_control_reference(user_type_cycles bytecode_struct_cycles.kir "STRUCT_CYCLE_ENUM_OK")
  add_native_control_reference(user_type_mixed native_compiler_user_struct_mixed.kir
    "Киртон 🧭 истина 100 100|истина 9223372036854775808")
  add_native_control_reference(user_type_gc native_compiler_user_types_gc.kir
    "истина изменено истина истина")
  add_native_control_reference(user_type_stdlib native_compiler_user_builtins.kir
    "2 нет|истина [2, 3, 4, 6]|4 [2, 3, 6]|10 замена|10 ложь 1|9223372036854775814|24 1")
  add_native_control_reference(user_type_shadow native_compiler_user_builtin_shadow.kir
    "6|2|11")
  # Native 0.8: reuse the VM Value ABI across deterministic string,
  # list, dictionary and math stdlib calls (no AST/Bytecode fallback).
  add_native_control_reference(stdlib_strings native_compiler_stdlib_strings.kir
    "Привет, мир!|12|истина истина|Привет, Киртон!|1|[а, б, в]|bcd OK")
  add_native_control_reference(stdlib_collections native_compiler_stdlib_collections.kir
    "истина [1, 99, 3] [1, 2, 3]|1|[1, 2, 3]|[1, 2, 3, 4]|истина []|[а, б] [1, 2]|[[а, 1], [б, 2]]|истина {а: 1, б: 2}")
  add_native_control_reference(stdlib_math native_compiler_stdlib_math.kir
    "3 4 5 -3|3 1 10|-1.0 истина|4.0 2 8|9.0 32.0 2.0")
  add_native_control_reference(stdlib_gc native_compiler_stdlib_gc.kir
    "2000|[а, б, в]|сохранено|[1, 2]")
  add_executable(kirton_native_stdlib_registration_test tests/native_stdlib_registration.c)
  target_include_directories(kirton_native_stdlib_registration_test PRIVATE src)
  target_link_libraries(kirton_native_stdlib_registration_test PRIVATE kirton_core)
  add_test(NAME kirton_native_stdlib_registration
    COMMAND kirton_native_stdlib_registration_test)
  if (KIRTON_CLANG_EXECUTABLE)
    foreach(error_case IN ITEMS user_type_invalid_field user_type_missing_field
                                user_type_invalid_assignment enum_invalid_variant)
      add_test(NAME kirton_native_user_type_error_${error_case}
        COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${error_case}.kir
          -DTEST_NAME=${error_case}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_user_type_error.cmake)
    endforeach()
  endif()

  if (KIRTON_CLANG_EXECUTABLE)
    foreach(error_case IN ITEMS list_bounds list_negative list_fraction string_bounds setindex_type)
      add_test(NAME kirton_native_collection_error_${error_case}
        COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_collection_error_${error_case}.kir
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_collection_error.cmake)
    endforeach()
  endif()
  if (KIRTON_CLANG_EXECUTABLE)
    foreach(error_case IN ITEMS bitwise_negative_shift bitwise_float_error)
      add_test(NAME kirton_native_${error_case}
        COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_${error_case}.kir
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_bitwise_error.cmake)
    endforeach()
    add_test(NAME kirton_native_bigint_division_by_zero_cleanup
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_bigint_division_by_zero.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_bigint_error.cmake)
  endif()
  if (KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_string_runtime_error_cleanup
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_string_runtime_error.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_runtime_error.cmake)
  endif()
  add_test(NAME kirton_native_fn_reject_wrong_arity
    COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_wrong_arity.kir
      "-DERROR_PATTERN=число аргументов|ожидает"
      "-DNATIVE_ERROR_PATTERN=expects 1 arguments"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_error_checks.cmake)
  add_test(NAME kirton_native_fn_reject_unknown
    COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_unknown.kir
      "-DERROR_PATTERN=Нет функции|Неизвестная функция"
      "-DNATIVE_ERROR_PATTERN=unknown function"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_error_checks.cmake)
  # Native 0.6A: parameters can now be polymorphic; previously rejected.
  add_native_control_reference(fn_dynamic_parameter_type
    native_compiler_fn_dynamic_type.kir "1|истина")
  # Native module static linker: imports are compiled to LLVM closures,
  # including transitive imports and a single module initialization per path.
  add_native_control_reference(module_static_link native_compiler_module_entry.kir
    "1|2|4|5|17|19")
  set_tests_properties(kirton_native_reference_module_static_link
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if (KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_static_link_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_diamond_cache native_compiler_module_diamond.kir
    "INIT_ONCE|1|2")
  set_tests_properties(kirton_native_reference_module_diamond_cache
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_diamond_cache_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_nested_import native_compiler_module_nested.kir
    "INIT_ONCE|1|2|3")
  set_tests_properties(kirton_native_reference_module_nested_import
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_nested_import_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  # End-to-end integration across transitive modules, returned closures,
  # mutable collections, BigInt, and exception/finally unwinding.
  add_native_control_reference(release_integration
    native_compiler_release_integration.kir
    "CAUGHT|401 402 403|1453200 истина 9223372036854775808 Киртон")
  set_tests_properties(kirton_native_reference_release_integration
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_release_integration_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_default_name native_compiler_module_default.kir
    "1")
  add_native_control_reference(module_gc_stress native_compiler_module_gc.kir
    "2001")
  foreach(module_case IN ITEMS default_name gc_stress)
    set_tests_properties(kirton_native_reference_module_${module_case}
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(KIRTON_CLANG_EXECUTABLE)
      set_tests_properties(kirton_native_module_${module_case}_diff
        PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
    endif()
  endforeach()
  if(KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_module_cycle_reject
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_cycle_entry.kir
        "-DEXPECT_ERROR=cyclic native module import"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_error.cmake)
    add_test(NAME kirton_native_module_missing_reject
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_missing.kir
        "-DEXPECT_ERROR=requires a resolvable user module"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_error.cmake)
    add_test(NAME kirton_native_module_type_visibility_reject
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_type_visibility.kir
        "-DEXPECT_ERROR=unknown function"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_error.cmake)
    set_tests_properties(kirton_native_module_cycle_reject
      kirton_native_module_missing_reject kirton_native_module_type_visibility_reject
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_user_types native_compiler_module_types_positive.kir
    "10|истина|32|32")
  set_tests_properties(kirton_native_reference_module_user_types
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_user_types_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_user_types_nested native_compiler_module_user_type_nested_entry.kir
    "обёртка|узел|9223372036854775808|истина|истина")
  set_tests_properties(kirton_native_reference_module_user_types_nested
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_user_types_nested_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  add_native_control_reference(module_type_namespace native_compiler_module_same_type_entry.kir
    "5|7|9|истина|истина|ложь|ложь|ложь")
  set_tests_properties(kirton_native_reference_module_type_namespace
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_type_namespace_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  if(KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_module_type_identity_reject
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_same_type_negative.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_module_type_runtime_error.cmake)
    set_tests_properties(kirton_native_module_type_identity_reject
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  # Native try/catch/finally: runtime-error propagation remains in generated
  # LLVM code, with pin-safe catch values and finally on the error path.
  add_native_control_reference(try_success native_compiler_try_success.kir
    "body|finally|7")
  add_native_control_reference(try_error native_compiler_try_error.kir
    "runtime|finally|after")
  add_native_control_reference(try_nested native_compiler_try_nested.kir
    "runtime!|тело|успех!|inner finally|runtime|outer finally")
  add_native_control_reference(try_call_failure native_compiler_try_call_failure.kir
    "runtime|done|continue")
  add_native_control_reference(try_print_failure native_compiler_try_print_failure.kir
    "runtime|done|continue")
  add_native_control_reference(try_catch_shadow native_compiler_try_catch_shadow.kir
    "runtime|fn outer|fn outer|runtime|outer|outer")
  add_native_control_reference(try_catch_reuse native_compiler_try_catch_reuse.kir
    "runtime|runtime|42")
  # Structured control-flow unwinding now runs finally on return and loop
  # transfers, retaining pending Value results across GC safepoints.
  add_native_control_reference(try_return_unwind native_compiler_try_unwind_reject.kir
    "cleanup|5")
  add_native_control_reference(try_return_nested native_compiler_try_return_nested.kir
    "inner|outer|ok")
  add_native_control_reference(try_return_override native_compiler_try_return_override.kir
    "7")
  add_native_control_reference(try_return_catch native_compiler_try_return_catch.kir
    "cleanup|9")
  add_native_control_reference(try_return_early_error native_compiler_try_return_early_error.kir
    "caught|cleanup|8")
  add_native_control_reference(try_break_outer native_compiler_try_break_outer.kir
    "1|2|3|done")
  add_native_control_reference(try_continue_outer native_compiler_try_continue_outer.kir
    "1|step|2|3|step|4|step")
  add_native_control_reference(try_break_inner_loop native_compiler_try_break_inner_loop.kir
    "2|outer cleanup")
  add_native_control_reference(try_foreach_break native_compiler_try_foreach_break.kir
    "1|cleanup|2|cleanup|done")
  add_native_control_reference(try_finally_continue native_compiler_try_finally_continue.kir
    "in|1|after|in|in|3|after")
  add_native_control_reference(try_finally_return_override_nested native_compiler_try_finally_return_override_nested.kir
    "outer|2")
  add_native_control_reference(try_return_gc_stress native_compiler_try_return_gc_stress.kir
    "719400")
  add_native_control_reference(try_finally_error_outer_catch native_compiler_try_finally_error_outer_catch.kir
    "runtime|end")
  add_native_control_reference(try_finally_override_break native_compiler_try_finally_override_break.kir
    "1|3|4|5|end")
  add_native_control_reference(try_return_from_catch_nested native_compiler_try_return_from_catch_nested.kir
    "inner|outer|caught")
  if(KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_try_finally_error_differential
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_try_finally_error.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_try_finally_error.cmake)
  endif()
  add_native_control_reference(module_unicode_bigint native_compiler_module_unicode_bigint.kir
    "9223372036854775809|[9223372036854775809, Киртон, истина]|9223372036854775810")
  set_tests_properties(kirton_native_reference_module_unicode_bigint
    PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  if(KIRTON_CLANG_EXECUTABLE)
    set_tests_properties(kirton_native_module_unicode_bigint_diff
      PROPERTIES ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endif()
  # Native 0.9: nested functions, first-class function values and GC-traced
  # lexical closures must match AST and bytecode execution.
  add_native_control_reference(fn_nested_scope native_compiler_fn_scope_nested.kir "3")
  add_native_control_reference(fn_value native_compiler_fn_value.kir "42")
  add_native_control_reference(fn_higher_order native_compiler_fn_higher_order.kir "42|21")
  add_native_control_reference(fn_hoist native_compiler_fn_hoist.kir "42")
  add_native_control_reference(fn_global native_compiler_fn_global.kir "42")
  add_native_control_reference(fn_capture_escape native_compiler_fn_capture_escape.kir "42")
  add_native_control_reference(fn_capture_mutate native_compiler_fn_capture_mutate.kir "1|2|3")
  add_native_control_reference(fn_capture_higher native_compiler_fn_capture_higher.kir "103|101")
  add_native_control_reference(fn_capture_recursion native_compiler_fn_capture_recursion.kir "720")
  add_native_control_reference(fn_capture_shadow native_compiler_fn_capture_shadow.kir "9")
  add_native_control_reference(fn_capture_shared native_compiler_fn_capture_shared.kir "1|2|2")
  add_native_control_reference(fn_capture_deep native_compiler_fn_capture_deep.kir "125")
  add_native_control_reference(fn_capture_gc native_compiler_fn_capture_gc.kir "обновлено")
  add_native_control_reference(fn_value_gc native_compiler_fn_value_gc.kir "998")
  add_native_control_reference(fn_capture_foreach native_compiler_fn_capture_foreach.kir "3|3|3")
  add_native_control_reference(fn_capture_cycle native_compiler_fn_capture_cycle.kir "41|42")
  add_native_control_reference(fn_capture_alias native_compiler_fn_capture_alias.kir "строка|14|14")
  if(KIRTON_CLANG_EXECUTABLE)
    foreach(negative IN ITEMS negative_arity negative_noncallable)
      add_test(NAME kirton_native_closure_${negative}
        COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_${negative}.kir
          -DCASE=${negative}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_closure_negative.cmake)
    endforeach()
  endif()
  if (KIRTON_CLANG_EXECUTABLE)
    add_test(NAME kirton_native_fn_bigint_overflow
      COMMAND ${CMAKE_COMMAND} -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_overflow.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_compiler_fn_overflow.cmake)
  endif()

  add_test(NAME kirton_bytecode_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
  add_test(NAME kirton_bytecode_core_smoke_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/core_smoke.kir
      -DWORKING_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples/apps
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_smoke_diff.cmake)
  set_tests_properties(kirton_bytecode_core_smoke_diff PROPERTIES TIMEOUT 45)
  add_test(NAME kirton_bytecode_memory_v2_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_memory_v2_diff.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
  add_test(NAME kirton_bytecode_fs_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_fs_diff.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef bytecode_fs_test_suffix)
  set_tests_properties(kirton_bytecode_fs_diff PROPERTIES
    ENVIRONMENT "KIRTON_BYTECODE_FS_DIR=${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/kirton_bytecode_fs_${bytecode_fs_test_suffix}")
  foreach(bytecode_native_case bytecode_os_diff bytecode_time_diff bytecode_timers_diff bytecode_runtime_diff bytecode_sys_diff bytecode_top_level_return)
    add_test(NAME kirton_${bytecode_native_case}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${bytecode_native_case}.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
  endforeach()
  foreach(bytecode_module_case bytecode_module_diff bytecode_module_capture bytecode_module_native bytecode_module_error bytecode_module_cycle bytecode_module_cross bytecode_module_missing bytecode_native_error bytecode_native_builtin bytecode_include_diff)
    add_test(NAME kirton_${bytecode_module_case}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${bytecode_module_case}.kir
        $<$<STREQUAL:${bytecode_module_case},bytecode_module_error>:-DEXPECT=MODULE_PARSE_ERROR_OK>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff_program.cmake)
    set_tests_properties(kirton_${bytecode_module_case} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  endforeach()
  add_test(NAME kirton_bytecode_camera3d_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_camera3d_diff.kir
      -DEXPECT=CAMERA3D_OK
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff_program.cmake)
  set_tests_properties(kirton_bytecode_camera3d_diff PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME kirton_bytecode_module_exception_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_exception.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_exception_diff.cmake)
  set_tests_properties(kirton_bytecode_module_exception_diff PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME kirton_bytecode_module_gc
    COMMAND $<TARGET_FILE:kirton_bytecode_gc_test> ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_stress.kir)
  set_tests_properties(kirton_bytecode_module_gc PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME kirton_bytecode_module_stress_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_stress.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_entry_diff.cmake)
  set_tests_properties(kirton_bytecode_module_stress_diff PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME kirton_bytecode_module_vm_repeated
    COMMAND $<TARGET_FILE:kirton_bytecode_profile> bytecode
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_module_stress.kir 3
      ${CMAKE_CURRENT_BINARY_DIR}/bytecode-module-vm-repeat.txt)
  set_tests_properties(kirton_bytecode_module_vm_repeated PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_executable(kirton_bytecode_module_read_oom_test tests/module_read_oom.c)
  target_link_libraries(kirton_bytecode_module_read_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_bytecode_module_read_oom
    COMMAND $<TARGET_FILE:kirton_bytecode_module_read_oom_test>
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_modules/counter.kir)
  add_test(NAME kirton_bytecode_ffi_rejected_without_opt_in
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_unsupported.kir
      "-DEXPECT=Внешние библиотеки отключены"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_reject.cmake)
  add_test(NAME kirton_bytecode_return_from_try_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_try_return.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff_case.cmake)
  add_test(NAME kirton_bytecode_error_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_error.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_error_diff.cmake)
  add_test(NAME kirton_bytecode_index_error_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_index_error.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_index_error_diff.cmake)
  add_test(NAME kirton_bytecode_shadow_numeric_builtin
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_shadow_numeric_builtin.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_error_kind_diff.cmake)
  foreach(bytecode_case collections scopes gc closure_gc)
    add_test(NAME kirton_bytecode_diff_${bytecode_case}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_${bytecode_case}.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff_case.cmake)
  endforeach()
  foreach(bytecode_program operator_contract loop_control numeric_ops numeric_special loop_collections bytecode_closures bytecode_struct_cycles closures user_types scope_bindings error_handling finally_loop_flow finally_error_override unhandled_error_finally)
    add_test(NAME kirton_bytecode_diff_${bytecode_program}
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${bytecode_program}.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff_program.cmake)
  endforeach()
  foreach(bytecode_error negative_shift non_integer_shift bitwise_float modulo_by_zero user_type_missing_field user_type_invalid_field user_type_invalid_assignment enum_invalid_variant bytecode_wrong_arity)
    if(bytecode_error STREQUAL "bitwise_float" OR bytecode_error STREQUAL "modulo_by_zero" OR bytecode_error MATCHES "^user_type_" OR bytecode_error STREQUAL "enum_invalid_variant" OR bytecode_error STREQUAL "bytecode_wrong_arity")
      add_test(NAME kirton_bytecode_error_${bytecode_error}
        COMMAND ${CMAKE_COMMAND}
          -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${bytecode_error}.kir
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_error_kind_diff.cmake)
    else()
      add_test(NAME kirton_bytecode_error_${bytecode_error}
        COMMAND ${CMAKE_COMMAND}
          -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${bytecode_error}.kir
          -DERROR_PATTERN=size_t
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_error_kind_diff.cmake)
    endif()
  endforeach()
  add_executable(kirton_bytecode_gc_test tests/bytecode_gc.c)
  target_link_libraries(kirton_bytecode_gc_test PRIVATE kirton_core)
  add_test(NAME kirton_bytecode_gc_reclamation
    COMMAND kirton_bytecode_gc_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_gc.kir)
  add_test(NAME kirton_bytecode_closure_gc_reclamation
    COMMAND kirton_bytecode_gc_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_closure_gc.kir)
  add_executable(kirton_bytecode_teardown_test tests/bytecode_teardown.c)
  target_link_libraries(kirton_bytecode_teardown_test PRIVATE kirton_core)
  add_test(NAME kirton_bytecode_teardown
    COMMAND kirton_bytecode_teardown_test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_teardown.kir
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_teardown_error.kir)
  add_test(NAME kirton_bytecode_call_semantic_parity
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantics.kir
      "-DEXPECT_PATTERN=MODULE_INIT[\r\n]+40[\r\n]+7[\r\n]+11[\r\n]+42[\r\n]+CALLEE[\r\n]+ARG 7[\r\n]+ARG 8[\r\n]+15[\r\n]+1[\r\n]+истина"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantic_parity.cmake)
  set_tests_properties(kirton_bytecode_call_semantic_parity PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME kirton_bytecode_call_audit_attribute
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/audit-scenarios/attribute.kir
      "-DEXPECT_ERROR=не является функцией"
      -DEXPECT_EMPTY_STDOUT=1
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantic_parity.cmake)
  add_test(NAME kirton_bytecode_call_missing_attribute
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_missing_attr.kir
      "-DEXPECT_ERROR=не является функцией"
      -DEXPECT_EMPTY_STDOUT=1
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantic_parity.cmake)
  add_test(NAME kirton_bytecode_call_non_function
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_non_function.kir
      "-DEXPECT_ERROR=не является функцией"
      -DEXPECT_EMPTY_STDOUT=1
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantic_parity.cmake)
  add_test(NAME kirton_bytecode_call_argument_error
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_argument_error.kir
      "-DEXPECT_ERROR=Деление на ноль"
      -DEXPECT_PATTERN=ARG1
      "-DFORBID_STDOUT=BODY_WRONG|ARG3_WRONG"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_call_semantic_parity.cmake)
  add_executable(kirton_bytecode_profile tests/bytecode_profile.c)
  target_link_libraries(kirton_bytecode_profile PRIVATE kirton_core)
  add_test(NAME kirton_bytecode_profile_smoke
    COMMAND kirton_bytecode_profile ast ${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_arithmetic.kir 1 ${CMAKE_CURRENT_BINARY_DIR}/bytecode-profile.txt)

  add_executable(kirton_embedding_host examples/embedding/host.c)
  target_link_libraries(kirton_embedding_host PRIVATE kirton_core)
  add_test(NAME kirton_embedding_host
    COMMAND kirton_embedding_host "${CMAKE_CURRENT_SOURCE_DIR}/examples/embedding/hello.kir")
  add_test(NAME kirton_getting_started_example
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/examples/getting_started.kir")
  set_tests_properties(kirton_getting_started_example PROPERTIES
    PASS_REGULAR_EXPRESSION "GETTING_STARTED_OK")

  target_compile_definitions(kirton_core PRIVATE KIRTON_ALLOC_TESTING)

  add_executable(kirton_oom_allocator_test tests/oom_allocator.c)
  target_compile_definitions(kirton_oom_allocator_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_oom_allocator_test PRIVATE kirton_core)
  add_executable(kirton_oom_contract_test tests/oom_contract.c)
  target_compile_definitions(kirton_oom_contract_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_oom_contract_test PRIVATE kirton_core)
  add_test(NAME kirton_oom_contract
    COMMAND ${CMAKE_COMMAND}
      -DOOM_CONTRACT_EXE=$<TARGET_FILE:kirton_oom_contract_test>
      -DBYTECODE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/oom_bytecode.kir
      -DWORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/tests
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/oom_contract.cmake)
  add_executable(kirton_list_oom_test tests/list_oom.c)
  target_compile_definitions(kirton_list_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_list_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_list_growth_oom COMMAND kirton_list_oom_test)
  add_executable(kirton_fs_read_oom_test tests/fs_read_oom.c)
  target_compile_definitions(kirton_fs_read_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_compile_definitions(kirton_bytecode_module_read_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_fs_read_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_fs_read_oom COMMAND kirton_fs_read_oom_test)
  add_executable(kirton_timers_oom_test tests/timers_oom.c)
  target_compile_definitions(kirton_timers_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_timers_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_timers_oom COMMAND kirton_timers_oom_test)
  add_executable(kirton_path_oom_test tests/path_oom.c)
  target_compile_definitions(kirton_path_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_path_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_path_oom COMMAND kirton_path_oom_test)
  add_executable(kirton_process_oom_test tests/process_oom.c)
  target_compile_definitions(kirton_process_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_process_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_process_oom COMMAND kirton_process_oom_test)
  if (WIN32)
    add_test(NAME kirton_bytecode_clipboard_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_clipboard_diff.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    add_test(NAME kirton_bytecode_window_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_window_diff.kir
        -DEXPECT=WINDOW_LIFECYCLE_OK
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    set_tests_properties(kirton_bytecode_window_diff PROPERTIES TIMEOUT 20)
    add_test(NAME kirton_bytecode_gl_context_lifecycle
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/gl_test_context_lifecycle.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_graphics_diff.cmake)
    set_tests_properties(kirton_bytecode_gl_context_lifecycle PROPERTIES TIMEOUT 45)
    add_test(NAME kirton_bytecode_bitmap_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_bitmap_diff.kir
        -DEXPECT=BITMAP_READ_OK
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    set_tests_properties(kirton_bytecode_bitmap_diff PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME kirton_bytecode_sound_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_sound_diff.kir
        -DEXPECT=SOUND_INVALID_OK
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    add_test(NAME kirton_bytecode_d3d_lifecycle
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_d3d_lifecycle.kir
        -DEXPECT=D3D_LIFECYCLE_OK
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    set_tests_properties(kirton_bytecode_d3d_lifecycle PROPERTIES TIMEOUT 45)
    add_test(NAME kirton_bytecode_process_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_process_diff.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    add_test(NAME kirton_bytecode_pkg_diff
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_pkg_diff.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_diff.cmake)
    add_executable(kirton_http_oom_test tests/http_oom.c)
    target_compile_definitions(kirton_http_oom_test PRIVATE KIRTON_ALLOC_TESTING)
    target_link_libraries(kirton_http_oom_test PRIVATE kirton_core)
    add_test(NAME kirton_http_oom COMMAND kirton_http_oom_test)
    add_executable(kirton_http_loopback_test tests/http_loopback.c)
    target_link_libraries(kirton_http_loopback_test PRIVATE kirton_core ws2_32)
    add_test(NAME kirton_http_loopback COMMAND kirton_http_loopback_test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_http_loopback.kir)
    set_tests_properties(kirton_http_loopback PROPERTIES TIMEOUT 20)
    add_executable(kirton_os_oom_test tests/os_oom.c)
    target_compile_definitions(kirton_os_oom_test PRIVATE KIRTON_ALLOC_TESTING)
    target_link_libraries(kirton_os_oom_test PRIVATE kirton_core)
    add_test(NAME kirton_os_oom COMMAND kirton_os_oom_test)
    add_executable(kirton_runtime_oom_test tests/runtime_oom.c)
    target_compile_definitions(kirton_runtime_oom_test PRIVATE KIRTON_ALLOC_TESTING)
    target_link_libraries(kirton_runtime_oom_test PRIVATE kirton_core)
    add_test(NAME kirton_runtime_oom COMMAND kirton_runtime_oom_test)
  endif()
  foreach(allocation_kind malloc realloc calloc)
    add_test(NAME kirton_oom_${allocation_kind}
      COMMAND "${CMAKE_COMMAND}"
              "-DALLOC_TEST_EXE=$<TARGET_FILE:kirton_oom_allocator_test>"
              "-DALLOC_KIND=${allocation_kind}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/oom_allocator.cmake")
  endforeach()

  add_executable(kirton_resource_finally_test tests/resource_finally.c)
  target_link_libraries(kirton_resource_finally_test PRIVATE kirton_core)
  add_test(NAME kirton_native_resource_finally
    COMMAND kirton_resource_finally_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_finally.kir")

  add_executable(kirton_bitmap_dimensions_test tests/bitmap_dimensions.c)
  target_compile_definitions(kirton_bitmap_dimensions_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_bitmap_dimensions_test PRIVATE kirton_core)
  add_test(NAME kirton_bitmap_dimensions COMMAND kirton_bitmap_dimensions_test)
  add_executable(kirton_json_depth_test tests/json_depth.c)
  target_link_libraries(kirton_json_depth_test PRIVATE kirton_core)
  add_test(NAME kirton_json_depth COMMAND kirton_json_depth_test)
  add_executable(kirton_json_numbers_test tests/json_numbers.c)
  target_compile_definitions(kirton_json_numbers_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_json_numbers_test PRIVATE kirton_core)
  add_test(NAME kirton_json_numbers COMMAND kirton_json_numbers_test)
  add_executable(kirton_native_input_fuzz_test tests/native_input_fuzz.c)
  target_link_libraries(kirton_native_input_fuzz_test PRIVATE kirton_core)
  add_test(NAME kirton_native_input_fuzz COMMAND kirton_native_input_fuzz_test)
  add_executable(kirton_lexer_contract_test tests/lexer_contract.c)
  target_link_libraries(kirton_lexer_contract_test PRIVATE kirton_core)
  add_test(NAME kirton_lexer_contract COMMAND kirton_lexer_contract_test)
  add_test(NAME kirton_string_escapes COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_escapes.kir")
  set_tests_properties(kirton_string_escapes PROPERTIES
    PASS_REGULAR_EXPRESSION "STRING_ESCAPES_OK"
    FAIL_REGULAR_EXPRESSION "STRING_ESCAPES_FAIL")
  add_test(NAME kirton_function_contract COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/function_contract.kir")
  set_tests_properties(kirton_function_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "FUNCTION_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "FUNCTION_CONTRACT_FAIL")
  add_test(NAME kirton_brace_syntax COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_syntax.kir")
  set_tests_properties(kirton_brace_syntax PROPERTIES
    PASS_REGULAR_EXPRESSION "BRACE_SYNTAX_OK"
    FAIL_REGULAR_EXPRESSION "BRACE_SYNTAX_FAIL")
  add_test(NAME kirton_handbook_examples COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/handbook_examples.kir")
  set_tests_properties(kirton_handbook_examples PROPERTIES
    PASS_REGULAR_EXPRESSION "HANDBOOK_EXAMPLES_OK")
  if (WIN32)
    find_program(KIRTON_POWERSHELL powershell.exe REQUIRED)
    add_test(NAME kirton_handbook_documented_examples
      COMMAND "${KIRTON_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_handbook_examples.ps1"
        -Interpreter "$<TARGET_FILE:kirton>"
        -Guide "${CMAKE_CURRENT_SOURCE_DIR}/docs/KIRTON_1_1_GUIDE.md"
        -WorkingDirectory "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  add_test(NAME kirton_brace_missing_open
    COMMAND "${CMAKE_COMMAND}"
      "-DKIRTON=$<TARGET_FILE:kirton>"
      "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_missing_open.kir"
      "-DEXPECTED_TEXT='{'"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_parse_error.cmake")
  add_test(NAME kirton_brace_missing_close
    COMMAND "${CMAKE_COMMAND}"
      "-DKIRTON=$<TARGET_FILE:kirton>"
      "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_missing_close.kir"
      "-DEXPECTED_TEXT='}'"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_parse_error.cmake")
  add_test(NAME kirton_brace_mixed_same_block
    COMMAND "${CMAKE_COMMAND}"
      "-DKIRTON=$<TARGET_FILE:kirton>"
      "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_mixed_same_block.kir"
      "-DEXPECTED_TEXT=Legacy"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_parse_error.cmake")
  add_test(NAME kirton_regex_replace COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/regex_replace.kir")
  set_tests_properties(kirton_regex_replace PROPERTIES
    PASS_REGULAR_EXPRESSION "REGEX_REPLACE_OK"
    FAIL_REGULAR_EXPRESSION "REGEX_REPLACE_FAIL")
  add_test(NAME kirton_string_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_contract.kir")
  set_tests_properties(kirton_string_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "STRING_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "STRING_CONTRACT_FAIL")
  add_test(NAME kirton_number_parse_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/number_parse_contract.kir")
  set_tests_properties(kirton_number_parse_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "NUMBER_PARSE_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "NUMBER_PARSE_CONTRACT_FAIL")
  add_executable(kirton_math_oom_test tests/math_oom.c)
  target_compile_definitions(kirton_math_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_math_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_math_parse_oom COMMAND kirton_math_oom_test)
  add_executable(kirton_string_oom_test tests/string_oom.c)
  target_compile_definitions(kirton_string_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_string_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_string_oom COMMAND kirton_string_oom_test)
  add_executable(kirton_regex_oom_test tests/regex_oom.c)
  target_compile_definitions(kirton_regex_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_regex_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_regex_oom COMMAND kirton_regex_oom_test)

  add_executable(kirton_memory_v2_resource_lease_test tests/memory_v2_resource_lease.c)
  target_link_libraries(kirton_memory_v2_resource_lease_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_resource_lease COMMAND kirton_memory_v2_resource_lease_test)
  add_library(kirton_ffi_v3_plugin MODULE tests/ffi_v3_plugin.c)
  target_include_directories(kirton_ffi_v3_plugin PRIVATE include)
  set_target_properties(kirton_ffi_v3_plugin PROPERTIES PREFIX "")
  add_test(NAME kirton_bytecode_ffi_v3_resource
    COMMAND kirton_resource_finally_test
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_finally.kir"
      "$<TARGET_FILE:kirton_ffi_v3_plugin>"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_ffi_v3_resource.kir")
  set_tests_properties(kirton_bytecode_ffi_v3_resource PROPERTIES TIMEOUT 20)
  add_library(kirton_ffi_v3_bad_abi MODULE tests/ffi_v3_bad_abi.c)
  target_include_directories(kirton_ffi_v3_bad_abi PRIVATE include)
  set_target_properties(kirton_ffi_v3_bad_abi PROPERTIES PREFIX "")
  add_executable(kirton_ffi_v3_api_test tests/ffi_v3_api.c)
  target_compile_definitions(kirton_ffi_v3_api_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_ffi_v3_api_test PRIVATE kirton_core)
  add_test(NAME kirton_ffi_v3_api COMMAND kirton_ffi_v3_api_test
    "$<TARGET_FILE:kirton_ffi_v3_plugin>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_v3_vm.kir"
    "$<TARGET_FILE:kirton_ffi_v3_bad_abi>")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_v3_cli_$<CONFIG>.kir"
    CONTENT "импорт память\nимпорт внешний\nвнешний.загрузить(\"$<TARGET_FILE:kirton_ffi_v3_plugin>\")\nпусть схема = память.структура(\"здоровье\",\"целое32\",\"идентификатор\",\"целое64\",\"скорость\",\"число64\")\nпусть область = память.создать(4096)\nпусть игрок = память.запись(область,схема)\nигрок.здоровье = 42\nигрок.идентификатор = 9007199254740993\nигрок.скорость = 2.5\nпусть ответ = внешний.вызвать(\"структуры\",\"проверить\",игрок)\nесли ответ != 9007199254740993 то\n  печатать(\"FFI_V3_CLI_FAIL\")\nконец\nпечатать(\"FFI_V3_CLI_OK\")\n")
  add_test(NAME kirton_ffi_v3_cli COMMAND kirton --allow-ffi "${CMAKE_CURRENT_BINARY_DIR}/ffi_v3_cli_$<CONFIG>.kir")
  set_tests_properties(kirton_ffi_v3_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "FFI_V3_CLI_OK" FAIL_REGULAR_EXPRESSION "FFI_V3_CLI_FAIL")
  add_test(NAME kirton_bytecode_ffi_v3_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/ffi_v3_cli_$<CONFIG>.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_ffi_diff.cmake)
  add_library(kirton_ffi_v2_plugin MODULE tests/ffi_v2_plugin.c)
  target_include_directories(kirton_ffi_v2_plugin PRIVATE include)
  set_target_properties(kirton_ffi_v2_plugin PROPERTIES PREFIX "")
  add_library(kirton_ffi_v2_bad_abi MODULE tests/ffi_v2_bad_abi.c)
  target_include_directories(kirton_ffi_v2_bad_abi PRIVATE include)
  set_target_properties(kirton_ffi_v2_bad_abi PROPERTIES PREFIX "")
  add_executable(kirton_ffi_v2_api_test tests/ffi_v2_api.c)
  target_link_libraries(kirton_ffi_v2_api_test PRIVATE kirton_core)
  add_test(NAME kirton_ffi_v2_api COMMAND kirton_ffi_v2_api_test
    "$<TARGET_FILE:kirton_ffi_v2_plugin>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_v2_vm.kir"
    "$<TARGET_FILE:kirton_ffi_v2_bad_abi>")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir"
    CONTENT "импорт внешний\nвнешний.загрузить(\"$<TARGET_FILE:kirton_ffi_v2_plugin>\")\nпусть ответ = внешний.вызвать(\"арифметика\", \"сложить\", 9007199254740993, 7)\nесли ответ != 9007199254741000 то\n    печатать(\"FFI_CLI_FAIL\")\nконец\nпечатать(\"FFI_CLI_OK\")\n")
  add_test(NAME kirton_ffi_v2_cli COMMAND kirton --allow-ffi "${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir")
  set_tests_properties(kirton_ffi_v2_cli PROPERTIES PASS_REGULAR_EXPRESSION "FFI_CLI_OK" FAIL_REGULAR_EXPRESSION "FFI_CLI_FAIL")
  add_test(NAME kirton_bytecode_ffi_v2_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_ffi_diff.cmake)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_invalid_$<CONFIG>.kir"
    CONTENT "импорт внешний\nвнешний.загрузить(\"$<TARGET_FILE:kirton_ffi_v2_plugin>\")\nфункция main()\n  попробовать\n    внешний.вызвать(\"арифметика\", \"сложить\", \"не целое\", 7)\n    печатать(\"FFI_INVALID_NOT_REJECTED\")\n  перехватить ошибка\n    печатать(\"FFI_INVALID_CAUGHT\")\n  конец\nконец\nmain()\n")
  add_test(NAME kirton_bytecode_ffi_invalid_argument_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/ffi_invalid_$<CONFIG>.kir
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_ffi_diff.cmake)
  set_tests_properties(kirton_bytecode_ffi_v2_diff kirton_bytecode_ffi_v3_diff kirton_bytecode_ffi_invalid_argument_diff PROPERTIES TIMEOUT 20)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v2_extended_$<CONFIG>.kir"
    CONTENT "импорт внешний\nвнешний.загрузить(\"$<TARGET_FILE:kirton_ffi_v2_plugin>\")\nпечатать(внешний.вызвать(\"арифметика\",\"умножить\",1.5,4.0))\nпечатать(внешний.вызвать(\"арифметика\",\"инвертировать\",ложь))\nпечатать(внешний.вызвать(\"арифметика\",\"повторить\",\"Киртон\"))\nпечатать(внешний.вызвать(\"арифметика\",\"пусто\"))\n")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v3_clone_$<CONFIG>.kir"
    CONTENT "импорт память\nимпорт внешний\nвнешний.загрузить(\"$<TARGET_FILE:kirton_ffi_v3_plugin>\")\nпусть схема = память.структура(\"здоровье\",\"целое32\",\"идентификатор\",\"целое64\",\"скорость\",\"число64\")\nпусть область = память.создать(4096)\nпусть игрок = память.запись(область,схема)\nигрок.здоровье = 42\nигрок.идентификатор = 9007199254740993\nигрок.скорость = 2.5\nпусть копия = внешний.вызвать(\"структуры\",\"копировать\",игрок)\nпечатать(копия.здоровье)\nпечатать(копия.идентификатор)\nпечатать(копия.скорость)\nпечатать(внешний.вызвать(\"структуры\",\"проверить\",игрок))\n")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v2_local_$<CONFIG>.kir"
    CONTENT "функция тест_ffi(путь) {\n  импорт внешний как библиотека\n  библиотека.загрузить(путь)\n  вернуть библиотека.вызвать(\"арифметика\",\"сложить\",41,1)\n}\nпечатать(тест_ffi(\"$<TARGET_FILE:kirton_ffi_v2_plugin>\"))\n")
  # Native FFI V2/V3: compile real executables and compare them against
  # both interpreter modes, including record fields and caught failures.
  # A sanitized GCC-built shared library cannot be loaded into a Clang ASan
  # executable: they have distinct, incompatible ASan runtimes. Build the
  # native FFI test plugins with the same Clang ASan instrumentation as the
  # generated EXE, preserving GCC ASan plugins for AST/Bytecode comparisons.
  set(native_ffi_clang_sanitizer OFF)
  if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_C_FLAGS MATCHES "sanitize=address" AND KIRTON_CLANG_EXECUTABLE)
    set(native_ffi_clang_sanitizer ON)
    foreach(ffi_plugin_type IN ITEMS v2 v3)
      set(plugin_asan_output
        "${CMAKE_CURRENT_BINARY_DIR}/kirton_ffi_${ffi_plugin_type}_native_clang_asan.so")
      add_custom_command(OUTPUT "${plugin_asan_output}"
        COMMAND ${KIRTON_CLANG_EXECUTABLE} -shared -fPIC
            -fsanitize=address,undefined -fno-omit-frame-pointer -g
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -o "${plugin_asan_output}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_${ffi_plugin_type}_plugin.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_${ffi_plugin_type}_plugin.c"
        VERBATIM)
      add_custom_target(kirton_ffi_${ffi_plugin_type}_native_clang_asan ALL
        DEPENDS "${plugin_asan_output}")
    endforeach()
  endif()
  if (KIRTON_CLANG_EXECUTABLE)
    foreach(native_ffi_case IN ITEMS v2 v3 v2_extended v3_clone v2_local invalid memory memory_local memory_invalid)
      if(native_ffi_case STREQUAL "v2")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir")
        set(native_ffi_expect "FFI_CLI_OK")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 ON)
      elseif(native_ffi_case STREQUAL "v3")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_v3_cli_$<CONFIG>.kir")
        set(native_ffi_expect "FFI_V3_CLI_OK")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 OFF)
      elseif(native_ffi_case STREQUAL "v2_extended")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v2_extended_$<CONFIG>.kir")
        set(native_ffi_expect "6.0\nистина\nКиртон\nпусто")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 ON)
      elseif(native_ffi_case STREQUAL "v3_clone")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v3_clone_$<CONFIG>.kir")
        set(native_ffi_expect "42\n9007199254740993\n2.5\n9007199254740993")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 OFF)
      elseif(native_ffi_case STREQUAL "v2_local")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_native_v2_local_$<CONFIG>.kir")
        set(native_ffi_expect "42")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 ON)
      elseif(native_ffi_case STREQUAL "invalid")
        set(native_ffi_source "${CMAKE_CURRENT_BINARY_DIR}/ffi_invalid_$<CONFIG>.kir")
        set(native_ffi_expect "FFI_INVALID_CAUGHT")
        set(native_ffi_flag ON)
        set(native_ffi_rc 0)
        set(native_ffi_plugin_v2 ON)
      elseif(native_ffi_case STREQUAL "memory_local")
        set(native_ffi_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_memory_local.kir")
        set(native_ffi_expect "128")
        set(native_ffi_flag OFF)
        set(native_ffi_rc 0)
      elseif(native_ffi_case STREQUAL "memory")
        set(native_ffi_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_memory_value_diff.kir")
        set(native_ffi_expect "42\n9007199254740993\n2.5\nистина")
        set(native_ffi_flag OFF)
        set(native_ffi_rc 0)
      else()
        set(native_ffi_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_memory_negative.kir")
        set(native_ffi_expect "")
        set(native_ffi_flag OFF)
        set(native_ffi_rc 1)
      endif()
      set(native_ffi_plugin "")
      set(host_ffi_plugin "")
      if(native_ffi_clang_sanitizer AND native_ffi_flag)
        if(native_ffi_plugin_v2)
          set(native_ffi_plugin "${CMAKE_CURRENT_BINARY_DIR}/kirton_ffi_v2_native_clang_asan.so")
          set(host_ffi_plugin "$<TARGET_FILE:kirton_ffi_v2_plugin>")
        else()
          set(native_ffi_plugin "${CMAKE_CURRENT_BINARY_DIR}/kirton_ffi_v3_native_clang_asan.so")
          set(host_ffi_plugin "$<TARGET_FILE:kirton_ffi_v3_plugin>")
        endif()
      endif()
      add_test(NAME kirton_native_ffi_${native_ffi_case}_diff
        COMMAND ${CMAKE_COMMAND}
          -DKIRTON=$<TARGET_FILE:kirton>
          -DSOURCE=${native_ffi_source}
          -DTEST_NAME=${native_ffi_case}
          -DUSE_FFI=${native_ffi_flag}
          "-DNATIVE_PLUGIN=${native_ffi_plugin}"
          "-DHOST_PLUGIN=${host_ffi_plugin}"
          "-DEXPECT=${native_ffi_expect}"
          -DEXIT_CODE=${native_ffi_rc}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_ffi_diff.cmake)
      set_tests_properties(kirton_native_ffi_${native_ffi_case}_diff PROPERTIES TIMEOUT 100)
    endforeach()
    add_test(NAME kirton_native_ffi_requires_opt_in
      COMMAND ${CMAKE_COMMAND}
        -DKIRTON=$<TARGET_FILE:kirton>
        -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_ffi_disabled.cmake)
  endif()
  add_test(NAME kirton_ffi_v2_cli_requires_opt_in COMMAND kirton "${CMAKE_CURRENT_BINARY_DIR}/ffi_v2_cli_$<CONFIG>.kir")
  set_tests_properties(kirton_ffi_v2_cli_requires_opt_in PROPERTIES WILL_FAIL TRUE)
  add_test(NAME kirton_ffi_disabled_by_default COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_v2_disabled.kir")
  set_tests_properties(kirton_ffi_disabled_by_default PROPERTIES WILL_FAIL TRUE)
  add_executable(kirton_memory_v2_resource_api_test tests/memory_v2_resource_api.c)
  target_link_libraries(kirton_memory_v2_resource_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_resource_api COMMAND kirton_memory_v2_resource_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_resource_vm.kir")
  add_executable(kirton_memory_v2_borrow_api_test tests/memory_v2_borrow_api.c)
  target_link_libraries(kirton_memory_v2_borrow_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_borrow_api COMMAND kirton_memory_v2_borrow_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_borrow_vm.kir")
  add_executable(kirton_memory_v2_native_api_test tests/memory_v2_native_api.c)
  target_link_libraries(kirton_memory_v2_native_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_native_api COMMAND kirton_memory_v2_native_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_native_vm.kir")
  add_executable(kirton_memory_v2_record_pressure_test tests/memory_v2_record_pressure.c)
  target_link_libraries(kirton_memory_v2_record_pressure_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_record_pressure COMMAND kirton_memory_v2_record_pressure_test)
  add_executable(kirton_memory_v2_record_api_test tests/memory_v2_record_api.c)
  target_link_libraries(kirton_memory_v2_record_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_record_api COMMAND kirton_memory_v2_record_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_record_vm.kir")
  add_test(NAME kirton_memory_v2_record_script COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_record.kir")
  set_tests_properties(kirton_memory_v2_record_script PROPERTIES PASS_REGULAR_EXPRESSION "MEMORY_V2_RECORD_OK" FAIL_REGULAR_EXPRESSION "MEMORY_V2_RECORD_FAIL")
  foreach(error_case duplicate stale range missing json capacity)
    add_test(NAME kirton_memory_v2_record_${error_case} COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_record_${error_case}.kir")
    set_tests_properties(kirton_memory_v2_record_${error_case} PROPERTIES WILL_FAIL TRUE FAIL_REGULAR_EXPRESSION "UNEXPECTED_${error_case}_SUCCESS")
  endforeach()
  add_executable(kirton_memory_v2_typed_pressure_test tests/memory_v2_typed_pressure.c)
  target_link_libraries(kirton_memory_v2_typed_pressure_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_typed_pressure COMMAND kirton_memory_v2_typed_pressure_test)
  add_executable(kirton_memory_v2_typed_api_test tests/memory_v2_typed_api.c)
  target_link_libraries(kirton_memory_v2_typed_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_typed_api COMMAND kirton_memory_v2_typed_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_typed_vm.kir")
  add_test(NAME kirton_memory_v2_typed_script COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_typed.kir")
  set_tests_properties(kirton_memory_v2_typed_script PROPERTIES PASS_REGULAR_EXPRESSION "MEMORY_V2_TYPED_OK" FAIL_REGULAR_EXPRESSION "MEMORY_V2_TYPED_FAIL")
  foreach(error_case invalid_type stale oob json)
    add_test(NAME kirton_memory_v2_typed_${error_case} COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_typed_${error_case}.kir")
    set_tests_properties(kirton_memory_v2_typed_${error_case} PROPERTIES WILL_FAIL TRUE)
  endforeach()
  add_executable(kirton_memory_v2_api_test tests/memory_v2_api.c)
  target_link_libraries(kirton_memory_v2_api_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_api COMMAND kirton_memory_v2_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_vm.kir")
  add_executable(kirton_memory_v2_pressure_test tests/memory_v2_pressure.c)
  target_link_libraries(kirton_memory_v2_pressure_test PRIVATE kirton_core)
  add_test(NAME kirton_memory_v2_pressure COMMAND kirton_memory_v2_pressure_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_pressure.kir")
  add_test(NAME kirton_memory_v2_script COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2.kir")
  set_tests_properties(kirton_memory_v2_script PROPERTIES PASS_REGULAR_EXPRESSION "MEMORY_V2_OK" FAIL_REGULAR_EXPRESSION "MEMORY_V2_FAIL")
  add_test(NAME kirton_memory_closure_error
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_closure_error.kir")
  set_tests_properties(kirton_memory_closure_error PROPERTIES
    PASS_REGULAR_EXPRESSION "MEMORY_CLOSURE_ERROR_OK"
    FAIL_REGULAR_EXPRESSION "MEMORY_CLOSURE_ERROR_FAIL")
  add_test(NAME kirton_memory_v2_json_rejected COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_json.kir")
  set_tests_properties(kirton_memory_v2_json_rejected PROPERTIES WILL_FAIL TRUE FAIL_REGULAR_EXPRESSION "AFTER_INVALID_JSON")
  add_test(NAME kirton_memory_v2_invalid_alignment COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_alignment.kir")
  set_tests_properties(kirton_memory_v2_invalid_alignment PROPERTIES WILL_FAIL TRUE FAIL_REGULAR_EXPRESSION "AFTER_INVALID_ALIGNMENT")
  add_test(NAME kirton_memory_v2_stale_rejected COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_v2_stale.kir")
  set_tests_properties(kirton_memory_v2_stale_rejected PROPERTIES WILL_FAIL TRUE FAIL_REGULAR_EXPRESSION "AFTER_INVALID_READ")
  add_executable(kirton_gc_runtime_test tests/gc_runtime.c)
  target_link_libraries(kirton_gc_runtime_test PRIVATE kirton_core)
  add_test(NAME kirton_gc_runtime
    COMMAND kirton_gc_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/gc_runtime.kir")
  set_tests_properties(kirton_gc_runtime PROPERTIES
    TIMEOUT 45
    PASS_REGULAR_EXPRESSION "GC_RUNTIME_OK")
  add_executable(kirton_gc_pin_values_test tests/gc_pin_values.c)
  target_link_libraries(kirton_gc_pin_values_test PRIVATE kirton_core)
  add_test(NAME kirton_gc_pin_values COMMAND kirton_gc_pin_values_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/gc_pin_values.kir")
  add_executable(kirton_parse_api_test tests/parse_api_errors.c)
  target_link_libraries(kirton_parse_api_test PRIVATE kirton_core)
  add_test(NAME kirton_api_rejects_invalid_ast COMMAND kirton_parse_api_test)
  add_executable(kirton_parser_oom_test tests/parser_oom_failfast.c)
  target_compile_definitions(kirton_parser_oom_test PRIVATE KIRTON_ALLOC_TESTING)
  target_link_libraries(kirton_parser_oom_test PRIVATE kirton_core)
  add_test(NAME kirton_parser_oom_failfast
    COMMAND ${CMAKE_COMMAND}
      -DPARSER_OOM_TEST=$<TARGET_FILE:kirton_parser_oom_test>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/parser_oom_failfast.cmake)

  add_executable(kirton_value_numeric_test tests/value_numeric.c)
  target_link_libraries(kirton_value_numeric_test PRIVATE kirton_core)
  add_test(NAME kirton_value_numeric_conversions COMMAND kirton_value_numeric_test)

  add_executable(kirton_bigint_heap_test tests/bigint_heap.c)
  target_link_libraries(kirton_bigint_heap_test PRIVATE kirton_core)
  add_test(NAME kirton_bigint_heap COMMAND kirton_bigint_heap_test)

  add_executable(kirton_value_bigint_roots_test tests/value_bigint_roots.c)
  target_link_libraries(kirton_value_bigint_roots_test PRIVATE kirton_core)
  add_test(NAME kirton_value_bigint_roots COMMAND kirton_value_bigint_roots_test)

  add_executable(kirton_vm_isolation_test tests/vm_isolation.c)
  target_link_libraries(kirton_vm_isolation_test PRIVATE kirton_core)
  add_test(NAME kirton_vm_import_isolation
    COMMAND kirton_vm_isolation_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_import_root.kir")
  set_tests_properties(kirton_vm_import_isolation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  add_test(NAME kirton_collection_alias_memory
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/collection_alias_memory.kir")
  set_tests_properties(kirton_collection_alias_memory PROPERTIES
    PASS_REGULAR_EXPRESSION "COLLECTION_ALIAS_MEMORY_OK"
    FAIL_REGULAR_EXPRESSION "COLLECTION_ALIAS_MEMORY_FAIL")

  add_test(NAME kirton_truthiness
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/truthiness.kir")
  set_tests_properties(kirton_truthiness PROPERTIES
    PASS_REGULAR_EXPRESSION "TRUTHINESS_OK"
    FAIL_REGULAR_EXPRESSION "TRUTHINESS_FAIL")
  add_executable(kirton_truthiness_resource_test tests/truthiness_resource.c)
  target_link_libraries(kirton_truthiness_resource_test PRIVATE kirton_core)
  add_test(NAME kirton_truthiness_resource COMMAND kirton_truthiness_resource_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/truthiness_resource.kir")

  add_test(NAME kirton_bigint_retention
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/bigint_retention.kir")
  set_tests_properties(kirton_bigint_retention PROPERTIES
    PASS_REGULAR_EXPRESSION "BIGINT_RETENTION_OK"
    FAIL_REGULAR_EXPRESSION "BIGINT_RETENTION_FAIL")

  if (KIRTON_LEGACY_DRAW2D)
    add_executable(kirton_draw2d_api_test tests/draw2d_api.c)
    target_link_libraries(kirton_draw2d_api_test PRIVATE kirton_core)
    add_test(NAME kirton_draw2d_optional_fill_arguments COMMAND kirton_draw2d_api_test)
  endif()

  add_test(NAME kirton_core_smoke
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/core_smoke.kir")
  set_tests_properties(kirton_core_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "ИТОГ: ВСЁ ОК"
    FAIL_REGULAR_EXPRESSION "FAIL:;ИТОГ: ЕСТЬ ОШИБКИ")

  add_test(NAME kirton_loop_control
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/loop_control.kir")
  set_tests_properties(kirton_loop_control PROPERTIES
    PASS_REGULAR_EXPRESSION "LOOP_CONTROL_OK"
    FAIL_REGULAR_EXPRESSION "LOOP_CONTROL_FAIL")

  add_test(NAME kirton_string_index
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_index.kir")
  set_tests_properties(kirton_string_index PROPERTIES
    PASS_REGULAR_EXPRESSION "STRING_INDEX_OK"
    FAIL_REGULAR_EXPRESSION "STRING_INDEX_FAIL")
  add_executable(kirton_string_index_api_test tests/string_index_api.c)
  target_link_libraries(kirton_string_index_api_test PRIVATE kirton_core)
  add_test(NAME kirton_string_index_api COMMAND kirton_string_index_api_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_index_api.kir")
  add_test(NAME kirton_rejects_string_index_out_of_range
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_index_oob.kir")
  set_tests_properties(kirton_rejects_string_index_out_of_range PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "STRING_INDEX_OOB_FAIL_AFTER")
  add_test(NAME kirton_rejects_negative_string_index
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_index_negative.kir")
  set_tests_properties(kirton_rejects_negative_string_index PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "STRING_INDEX_NEGATIVE_FAIL_AFTER")

  add_test(NAME kirton_numeric_operators
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/numeric_ops.kir")
  set_tests_properties(kirton_numeric_operators PROPERTIES
    PASS_REGULAR_EXPRESSION "NUMERIC_OPS_OK"
    FAIL_REGULAR_EXPRESSION "NUMERIC_OPS_FAIL")
  add_test(NAME kirton_numeric_special
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/numeric_special.kir")
  set_tests_properties(kirton_numeric_special PROPERTIES
    PASS_REGULAR_EXPRESSION "NUMERIC_SPECIAL_OK"
    FAIL_REGULAR_EXPRESSION "NUMERIC_SPECIAL_FAIL")
  add_test(NAME kirton_value_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/value_contract.kir")
  set_tests_properties(kirton_value_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "VALUE_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "VALUE_CONTRACT_FAIL")
  add_test(NAME kirton_operator_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/operator_contract.kir")
  set_tests_properties(kirton_operator_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "OPERATOR_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "OPERATOR_CONTRACT_FAIL")
  add_test(NAME kirton_flow_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/flow_contract.kir")
  set_tests_properties(kirton_flow_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "FLOW_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "FLOW_CONTRACT_FAIL")
  add_test(NAME kirton_module_path_contract
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_path_contract.kir")
  set_tests_properties(kirton_module_path_contract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    ENVIRONMENT "KIRTON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/tests/path_search"
    PASS_REGULAR_EXPRESSION "MODULE_PATH_CONTRACT_OK"
    FAIL_REGULAR_EXPRESSION "MODULE_PATH_CONTRACT_FAIL")
  add_test(NAME kirton_module_exports
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_exports.kir")
  set_tests_properties(kirton_module_exports PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "MODULE_EXPORTS_OK"
    FAIL_REGULAR_EXPRESSION "MODULE_EXPORTS_FAIL")
  add_test(NAME kirton_cli_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DKIRTON_EXE=$<TARGET_FILE:kirton>"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.cmake")
  add_test(NAME kirton_cli_version COMMAND kirton --version)
  set_tests_properties(kirton_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "Kirton ${KIRTON_PRODUCT_VERSION}")
  add_test(NAME kirton_cli_utf8_script_path
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/путь_скрипта.kir")
  set_tests_properties(kirton_cli_utf8_script_path PROPERTIES
    PASS_REGULAR_EXPRESSION "CLI_UTF8_PATH_OK")

  add_test(NAME kirton_rejects_bitwise_float
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/bitwise_float.kir")
  set_tests_properties(kirton_rejects_bitwise_float PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "AFTER_ERROR")

  add_test(NAME kirton_rejects_negative_shift
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative_shift.kir")
  set_tests_properties(kirton_rejects_negative_shift PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "AFTER_ERROR")

  add_test(NAME kirton_rejects_non_integer_shift
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/non_integer_shift.kir")
  set_tests_properties(kirton_rejects_non_integer_shift PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "AFTER_ERROR")

  add_test(NAME kirton_rejects_division_by_zero_in_integer_division
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/integer_division_by_zero.kir")
  set_tests_properties(kirton_rejects_division_by_zero_in_integer_division PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "AFTER_ERROR")

  add_test(NAME kirton_rejects_modulo_by_zero
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/modulo_by_zero.kir")
  set_tests_properties(kirton_rejects_modulo_by_zero PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "AFTER_ERROR")

  add_test(NAME kirton_rejects_break_outside_loop
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/break_outside_loop.kir")
  set_tests_properties(kirton_rejects_break_outside_loop PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_rejects_continue_outside_loop
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/continue_outside_loop.kir")
  set_tests_properties(kirton_rejects_continue_outside_loop PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_rejects_break_inside_function
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/break_inside_function.kir")
  set_tests_properties(kirton_rejects_break_inside_function PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_legacy_syntax_aliases
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_syntax_aliases.kir")
  set_tests_properties(kirton_legacy_syntax_aliases PROPERTIES
    PASS_REGULAR_EXPRESSION "LEGACY_ALIAS_OK"
    FAIL_REGULAR_EXPRESSION "LEGACY_ALIAS_FAIL")

  add_test(NAME kirton_module_declaration
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_declaration.kir")
  set_tests_properties(kirton_module_declaration PROPERTIES
    PASS_REGULAR_EXPRESSION "MODULE_HEADER_OK"
    FAIL_REGULAR_EXPRESSION "MODULE_HEADER_FAIL")

  add_test(NAME kirton_scoped_import_aliases
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/scoped_import_aliases.kir")
  set_tests_properties(kirton_scoped_import_aliases PROPERTIES
    PASS_REGULAR_EXPRESSION "2 1"
    FAIL_REGULAR_EXPRESSION "SCOPED_NATIVE_FAIL")

  add_test(NAME kirton_module_function_scope
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_function_scope.kir")
  set_tests_properties(kirton_module_function_scope PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "MODULE_FUNCTION_SCOPE_OK"
    FAIL_REGULAR_EXPRESSION "MODULE_FUNCTION_SCOPE_FAIL")

  add_test(NAME kirton_rejects_unqualified_imported_function
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/unqualified_import_function.kir")
  set_tests_properties(kirton_rejects_unqualified_imported_function PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "UNQUALIFIED_IMPORTED_FUNCTION_FAIL_AFTER")

  add_test(NAME kirton_error_handling
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/error_handling.kir")
  set_tests_properties(kirton_error_handling PROPERTIES
    PASS_REGULAR_EXPRESSION "ERROR_HANDLING_OK"
    FAIL_REGULAR_EXPRESSION "ERROR_HANDLING_FAIL|ERROR_FAIL")
  add_test(NAME kirton_native_error_catch
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_error_catch.kir")
  set_tests_properties(kirton_native_error_catch PROPERTIES
    PASS_REGULAR_EXPRESSION "NATIVE_ERROR_CATCH_OK"
    FAIL_REGULAR_EXPRESSION "NATIVE_ERROR_FAIL")

  add_test(NAME kirton_finally_loop_flow
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/finally_loop_flow.kir")
  set_tests_properties(kirton_finally_loop_flow PROPERTIES
    PASS_REGULAR_EXPRESSION "FINALLY_LOOP_FLOW_OK"
    FAIL_REGULAR_EXPRESSION "FINALLY_LOOP_FLOW_FAIL")

  add_test(NAME kirton_catches_import_error
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/catch_import_error.kir")
  set_tests_properties(kirton_catches_import_error PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "CATCH_IMPORT_ERROR_OK"
    FAIL_REGULAR_EXPRESSION "CATCH_IMPORT_ERROR_FAIL")

  add_test(NAME kirton_catches_cyclic_import
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/cyclic_import_catch.kir")
  set_tests_properties(kirton_catches_cyclic_import PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PASS_REGULAR_EXPRESSION "CYCLE_IMPORT_CAUGHT_OK"
    FAIL_REGULAR_EXPRESSION "CYCLE_IMPORT_CAUGHT_FAIL")

  add_test(NAME kirton_finally_error_overrides
    COMMAND "${CMAKE_COMMAND}" "-DKIRTON_EXE=$<TARGET_FILE:kirton>"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/finally_error_override.kir"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/finally_error_override.cmake")

  add_test(NAME kirton_user_types
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_types.kir")
  set_tests_properties(kirton_user_types PROPERTIES
    PASS_REGULAR_EXPRESSION "USER_TYPES_OK"
    FAIL_REGULAR_EXPRESSION "USER_TYPES_FAIL")

  add_test(NAME kirton_user_type_integration
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_type_integration.kir")
  set_tests_properties(kirton_user_type_integration PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "USER_TYPE_INTEGRATION_OK"
    FAIL_REGULAR_EXPRESSION "USER_TYPE_INTEGRATION_FAIL")

  add_test(NAME kirton_task_tracker_example
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/task_tracker.kir")
  set_tests_properties(kirton_task_tracker_example PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps"
    PASS_REGULAR_EXPRESSION "TASK_TRACKER_OK")

  add_test(NAME kirton_expense_report_example
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/expense_report.kir")
  set_tests_properties(kirton_expense_report_example PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples/apps"
    PASS_REGULAR_EXPRESSION "EXPENSE_REPORT_OK"
    FAIL_REGULAR_EXPRESSION "EXPENSE_REPORT_FAIL")

  add_test(NAME kirton_kirtos_init_example_diff
    COMMAND ${CMAKE_COMMAND}
      -DKIRTON=$<TARGET_FILE:kirton>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/apps/kirtos_init.kir
      -DEXPECT=KirtonShell
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/example_launch_both_modes.cmake)
  set_tests_properties(kirton_kirtos_init_example_diff PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  add_test(NAME kirton_rejects_invalid_user_type_field
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_type_invalid_field.kir")
  set_tests_properties(kirton_rejects_invalid_user_type_field PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "USER_TYPE_INVALID_FAIL_AFTER")

  add_test(NAME kirton_rejects_missing_user_type_field
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_type_missing_field.kir")
  set_tests_properties(kirton_rejects_missing_user_type_field PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "USER_TYPE_MISSING_FAIL_AFTER")

  add_test(NAME kirton_rejects_invalid_user_type_assignment
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_type_invalid_assignment.kir")
  set_tests_properties(kirton_rejects_invalid_user_type_assignment PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "USER_TYPE_ASSIGN_FAIL_AFTER")

  add_test(NAME kirton_rejects_unknown_enum_variant
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/enum_invalid_variant.kir")
  set_tests_properties(kirton_rejects_unknown_enum_variant PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ENUM_VARIANT_FAIL_AFTER")

  add_test(NAME kirton_rejects_duplicate_user_type_member
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/user_type_duplicate_member.kir")
  set_tests_properties(kirton_rejects_duplicate_user_type_member PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME kirton_unhandled_error_runs_finally
    COMMAND "${CMAKE_COMMAND}" "-DKIRTON_EXE=$<TARGET_FILE:kirton>"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/unhandled_error_finally.kir"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/error_finally.cmake")

  add_test(NAME kirton_module_cache_growth
    COMMAND "${CMAKE_COMMAND}" "-DKIRTON_EXE=$<TARGET_FILE:kirton>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_cache_growth.cmake")

  add_test(NAME kirton_rejects_unknown_variable
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/unknown_variable.kir")
  set_tests_properties(kirton_rejects_unknown_variable PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ПОСЛЕ_ОШИБКИ|nil")

  add_test(NAME kirton_scope_bindings
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/scope_bindings.kir")
  set_tests_properties(kirton_scope_bindings PROPERTIES
    PASS_REGULAR_EXPRESSION "SCOPE_BINDINGS_OK"
    FAIL_REGULAR_EXPRESSION "SCOPE_BINDINGS_FAIL")

  add_test(NAME kirton_closures
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/closures.kir")
  set_tests_properties(kirton_closures PROPERTIES
    PASS_REGULAR_EXPRESSION "CLOSURE_OK"
    FAIL_REGULAR_EXPRESSION "CLOSURE_FAIL")

  add_test(NAME kirton_rejects_assignment_to_unknown_variable
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/assignment_unknown.kir")
  set_tests_properties(kirton_rejects_assignment_to_unknown_variable PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ASSIGNMENT_UNKNOWN_FAIL_AFTER")

  add_test(NAME kirton_rejects_wrong_arity
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/wrong_arity.kir")
  set_tests_properties(kirton_rejects_wrong_arity PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_rejects_extra_arguments
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/wrong_arity_extra.kir")
  set_tests_properties(kirton_rejects_extra_arguments PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_stops_on_import_runtime_error
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/import_runtime_error.kir")
  set_tests_properties(kirton_stops_on_import_runtime_error PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ПОСЛЕ_ОШИБКИ")

  add_test(NAME kirton_keeps_imported_source_location
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/imported_function_error.kir")
  set_tests_properties(kirton_keeps_imported_source_location PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ПОСЛЕ_ОШИБКИ")

  add_test(NAME kirton_rejects_invalid_syntax
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_syntax.kir")
  set_tests_properties(kirton_rejects_invalid_syntax PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_rejects_unknown_string_escape
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_string_escape.kir")
  set_tests_properties(kirton_rejects_unknown_string_escape PROPERTIES WILL_FAIL TRUE)

  add_test(NAME kirton_reports_division_error_location
    COMMAND kirton "${CMAKE_CURRENT_SOURCE_DIR}/tests/division_by_zero.kir")
  set_tests_properties(kirton_reports_division_error_location PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "ПОСЛЕ_ОШИБКИ")
endif()

if (BUILD_TESTING AND KIRTON_NATIVE_ASAN_LINK)
  get_property(_kirton_all_tests DIRECTORY PROPERTY TESTS)
  set(_kirton_test_path "${KIRTON_NATIVE_ASAN_DLL_DIR};$ENV{PATH}")
  string(REPLACE ";" "\\;" _kirton_test_path "${_kirton_test_path}")
  foreach(_kirton_test IN LISTS _kirton_all_tests)
    get_property(_kirton_test_environment TEST "${_kirton_test}" PROPERTY ENVIRONMENT)
    set(_kirton_test_environment_without_path "")
    foreach(_kirton_environment_entry IN LISTS _kirton_test_environment)
      if(NOT _kirton_environment_entry MATCHES "^PATH=")
        list(APPEND _kirton_test_environment_without_path "${_kirton_environment_entry}")
      endif()
    endforeach()
    list(APPEND _kirton_test_environment_without_path "PATH=${_kirton_test_path}")
    set_tests_properties(${_kirton_test} PROPERTIES
      ENVIRONMENT "${_kirton_test_environment_without_path}")
  endforeach()
endif()
