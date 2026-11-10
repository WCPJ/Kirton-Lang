# Kirton Compiler 1.2 — progress log

Release gate: **NOT CLEARED**. Bytecode remains experimental; do not publish it as stable SDK 1.2. The concrete compatibility statuses and open defects are listed in `COMPILER_COMPATIBILITY_1_2.md`.

## Starting point

- Inspected `COMPILER_ARCHITECTURE_1_2.md`, `COMPILER_STAGE5.md`, CMake configuration, Bytecode/native dispatch, registered module implementations, test inventories, and SDK/version files.
- The supplied directory is not a Git repository (`git status` reports “not a git repository” both at the workspace root and in `kirton-c`). No existing release directories were modified. There is no prior Git history to preserve or checkpoint.
- Existing project version and checked-in distributables remain 1.1.0. No SDK 1.2 package was produced.

## Checkpoint — task 1, MSVC ASan recovery

- Root cause: `build-stage5-asan` was compiled by MSVC 14.44 and depends on that toolchain's `clang_rt.asan_dynamic-x86_64.dll`. The CTest process PATH did not include the compiler's runtime directory, so Windows returned `0xc0000135` before Kirton entered `main`. Adding the 14.52 Insiders runtime directory did not work: the 14.44 binary stalled at startup and was terminated after five seconds. The matching 14.44 runtime directory makes `kirton --version` exit 0.
- Fixed: pinned the ASan CMake cache to the absolute VS 2022 14.44 compiler and matching Ninja, and added `tests/run_msvc_asan.ps1`, which derives the runtime DLL beside the configured compiler, performs a startup check, prepends the matching runtime path, uses a 30-second per-test timeout, and saves `Testing/Temporary/MSVC_ASAN_CTest.log`.
- Verification: full MSVC ASan CTest passed **189/189** in 35.81 seconds. The 54 Bytecode-specific tests passed, including memory/filesystem/OS/time differential fixtures, GC stress, module errors, FFI rejection, and closure tests. The full suite ran under ASan as well.
- Errors found/fixed: this was an instrumentation launch/configuration issue, not a Kirton runtime failure. The wrong-version runtime PATH reproduced the startup hang independently of test execution.
- Remaining: Windows ASan is now usable. Linux Clang/GCC ASan/UBSan/LSan was not run.
- Next: finish the compatibility inventory, then port and test the remaining APIs.

## Checkpoint — stage 6 compatibility audit

- Implemented: explicitly allowlisted existing `память`, `fs`, and `os` native modules and the global time callbacks for Bytecode. Memory callbacks allocate through the active Value heap already scoped by BVM. Added differential coverage for arenas, buffers, typed arrays, fixed-layout records, handle survival through 1,200 transient collection allocations, read-only filesystem existence, current working directory, and monotonic elapsed time.
- Tests added: `kirton_bytecode_memory_v2_diff`, `kirton_bytecode_fs_diff`, `kirton_bytecode_os_diff`, `kirton_bytecode_time_diff`; FFI rejection assertion now checks UTF-8 diagnostics.
- Checks: full MSVC Release CTest from `build-release` passed **189/189** in 23.50 seconds. The existing `build-stage5-asan` test run could not provide valid sanitizer evidence: the MSVC ASan executable and numerous child processes exited with Windows status `0xc0000135` (missing runtime dependency); tests that did not launch those executables are not evidence of memory safety.
- Errors found: the VM explicitly rejects registered modules not marked Bytecode-safe; the safe marker now includes selected global callbacks, `math3d`, `память`, `fs`, `os`, and time callbacks. Other resource, process/network, graphics, and FFI paths remain unavailable to Bytecode.
- Errors fixed: Bytecode's old unsupported-import test incorrectly treated the newly supported `память` import as unsupported; replaced it with the FFI opt-in rejection contract. CMake now decodes this diagnostic as UTF-8 so the test verifies the specific error rather than failing on mojibake.
- Remaining limitations: broad module/FFI/resource compatibility, parser OOM fail-fast paths noted in stage 5, Windows ASan runtime setup, graphics visual validation, and a reliable full AST/Bytecode application differential run.
- Next: unify or safely adapt the Runtime context used by native callbacks and the separate `BVM`, then port/test one resource family at a time while retaining opt-in FFI policy. This is a substantial Runtime integration project, not a marker-only change.

## Checkpoint — stage 7 compatibility and release audit

- Implemented: compatibility and release status documented in `COMPILER_STAGE7.md`.
- Tests added: none.
- Checks: existing AST application tests for task tracker and expense report passed as part of Release CTest. Manual Bytecode runs from `examples/apps` completed for both and printed their `*_OK` markers. CTest includes 54 Bytecode-focused tests in this build (differential, error, GC, modules, and profiling cases); the full 189-test run passed. The existing MSVC Release benchmark harness was rerun against the final build with three samples per mode across 18 scenarios; AST/Bytecode output matched in all cases. Direct C API measurements and limitations are in `COMPILER_STAGE7.md`.
- Errors found: required native-resource and graphics scenarios are not covered by current Bytecode support; SDK 1.2 is not present and the full requested stage 7 measurement set is not available.
- Errors fixed: none; the three-run benchmark confirms the current native-call slowdown, but the task's compatibility gate and causal profiling are not complete.
- Remaining limitations: see the stage 7 matrix; stable release criteria are not met.
- Next: resolve the Runtime/BVM integration architecture, fill module and FFI coverage, run comparative/stress/sanitizer/graphics validation, profile the native-call regression, and build a separate SDK 1.2 candidate only after compatibility evidence is complete.

## Checkpoint — tasks 2–3, compatibility matrix and standard callbacks

- Implemented: added `COMPILER_COMPATIBILITY_1_2.md` with AST/Bytecode language and Value-kind coverage plus a source-derived inventory of native APIs, functions, platform branches, explicit blockers, and test status. Enabled existing HTTP, process, timer, `sys`, and stateless Runtime callbacks through explicit Bytecode safety lists. Expanded filesystem tests to cover controlled temporary-directory creation, read/write/list/delete, invalid paths, and cleanup. HTTP tests run GET and POST against an in-process loopback server in AST and Bytecode and check invalid arguments. Process tests use a local Windows echo command, exit code, aliases, and invalid argument. Timer tests cover one-shot/repeating events, cancellation, invalid inputs, and cleanup.
- Tests added: `bytecode_http_loopback.kir`, `bytecode_process_diff.kir`, `bytecode_timers_diff.kir`, `bytecode_runtime_diff.kir`, `bytecode_sys_diff.kir`; filesystem fixture now uses a unique CTest temporary directory with cleanup before/after both modes.
- Checks: focused module tests passed. Full MSVC Release CTest passed **193/193** in 22.08 seconds. Full MSVC ASan CTest passed **193/193** in 36.80 seconds with a 30-second per-test timeout; logs are retained at `build-stage5-asan/Testing/Temporary/MSVC_ASAN_CTest.log`.
- Errors found/fixed: adding the HTTP harness initially omitted `<stdlib.h>` for `malloc`, causing a test-only pointer truncation/segfault; fixed before integration tests passed. No production HTTP callback changes beyond enabling the existing shared callbacks.
- Remaining limitations: FFI/resource leases and native resources still require a BVM owner/heap adapter; package-manager APIs need a safe stubbed test environment; graphics are Windows-only and visually unverified; Runtime `аргументы`, `progname`, and `scriptname` return Values from process-global storage outside the active BVM heap and remain gated.
- Next: complete FFI/resource ownership analysis, then graphics lifecycle integration and OOM fault-injection audit. Continue updating the compatibility matrix after each family.

## Recovery notes

The working tree has no Git metadata. These reports are the recovery point for this pass; existing SDK 1.0/1.1 artifacts and project sources remain untouched. Linux/Clang/GCC and visual graphics inspection were not used in this pass.

## Checkpoint — module GC roots, profiling, and final verification (2026-11-05)

- Fixed an intermittent imported-module corruption found in `d3d11_horror_demo.kir`. `gc_mark()` traced `vm->init` and compiled function chunks, but missed a currently running frame's chunk. The temporary init chunk for an imported module contains function-name constants; collection during its `BC_BIND_FUNC` sequence could reclaim these strings before `v_dict_set` copied them, producing missing or corrupted module exports and globals. `BFrame` now holds the active `Chunk*`, and GC marks every active frame's chunk.
- Added `kirton_bytecode_camera3d_diff`, which imports the real `examples/lib/camera3d.kir`, exercises exported and private functions plus module globals, and compares AST/Bytecode. It passed **25 consecutive repetitions**. `d3d11_horror_demo.kir` also exited 0 in AST and Bytecode with identical output when run from the project root.
- Removed one redundant native-callback registry search. Before the change, Bytecode looked up the safe callback during `prepare_call` and performed the same linear lookup again at invocation. The descriptor now retains the validated safe slot and invokes it directly. The 100,000-call `корень(81)` probe moved from 18.517 ms AST / 20.364 ms Bytecode to 17.058 ms AST / 15.338 ms Bytecode (11 samples; parse excluded). The 18-scenario Release harness's 10k `корень()` case measured 1.703 ms AST vs 1.454 ms Bytecode after the change (7 samples; Bytecode compile 0.001 ms). This confirms the local optimization but does not guarantee a general speedup.
- Release benchmark harness completed all 18 scenarios with 7 samples per mode and exact AST/Bytecode output matching. Example separate medians (parse excluded): arithmetic 9.822 ms AST vs 0.003 ms compile + 7.634 ms execute Bytecode; calls 3.434 vs 0.002 + 1.382 ms; closures 3.451 vs 0.003 + 2.186 ms; collections 2.088 vs 0.005 + 1.097 ms; first module import 0.059 vs 0.032 + 0.054 ms. The full log is `build-stage7-release/Testing/Temporary/bench_bytecode_stage7_final.txt`.
- Real-application output was compared after the final native dispatch change: `task_tracker.kir` and `expense_report.kir` each matched with exit 0; `d3d11_horror_demo.kir` matched with exit 0 from the project root. Seven-run profiler wrappers for the task tracker measured AST 0.149 ms vs Bytecode 0.083 ms compile + 0.123 ms execute; expense report measured 0.141 ms vs 0.076 + 0.114 ms. These are small examples and not representative of larger programs.
- Final verification after the implementation and unused-wrapper cleanup: clean MSVC 14.44 Release build, full CTest **209/209 passed** in 29.59 seconds; full MSVC ASan CTest **209/209 passed** in 46.44 seconds with a 45-second per-test timeout. ASan log: `build-stage5-asan/Testing/Temporary/MSVC_ASAN_CTest.log`. The 25-repeat camera3d regression run passed separately.
- Parser OOM remains fail-fast. Fault injection verifies controlled diagnostics and exit behavior; it does not establish recoverable parser/AST construction. Safely returning errors would require ownership-aware failures across recursive parser and AST factory APIs to dispose partial trees. Do not mark stage 6/7 or SDK 1.2 complete based only on the passing suite.
- SDK status is unchanged: `VERSION` and stable packages remain 1.1.0; no SDK 1.2 candidate was built because parser recovery, Linux package-manager verification, broader platform/API coverage, visual graphics inspection, and additional real-application stress remain release blockers.

## Checkpoint — OS environment differential coverage (2026-11-05)

- Added a controlled `os.установить_окружение` followed by `os.окружение` readback to `bytecode_os_diff.kir`, alongside the existing current-directory check.
- Verification: `kirton_bytecode_os_diff` passed in the configured MSVC Release build, exercising both AST and Bytecode via the differential harness.
- Remaining: this verifies current Windows behavior for one valid variable; invalid names/values and other platforms are not covered. Broader release blockers above remain.

## Checkpoint — parser OOM API audit (2026-11-05)

- Confirmed the existing `kirton_parser_oom_failfast` test exercises subprocess termination, not recoverable parse errors; it passed in `build-stage7-release`.
- The parser has no status/error return for allocation failure: `parse_program` returns only `AstProgram*`; AST factories call panic-on-OOM `kir_malloc`/`kir_realloc`; parser ownership-transfer helpers (`ast_*_push`, `ast_func_add_param`, and user-type additions) return `void`. Recursive parse routines assume every factory succeeds. Therefore converting only the allocator to return `NULL` would create lost children, invalid partially populated trees, and possible use-after-free; this requires a staged fallible AST factory + parser propagation change, not an allocator-only patch.
- No recovery claim is made. Next implementation step is to introduce checked AST collection/factory operations and propagate an explicit parser status while retaining the legacy `parse_program` wrapper; do not switch panic allocators globally until all parser ownership-transfer sites handle failure and partial-tree destruction is verified.

## Checkpoint — HTTP/network compatibility pass (2026-11-05)

- Source/registration inventory found four HTTP names (`http_получить`, `веб_получить`, `http_запрос`, `веб_запрос`), all routed to the same existing WinHTTP callbacks and marked Bytecode-safe. No TCP/UDP module, callbacks, persistent socket resources, or network-handle API exists in this Kirton 1.1 checkout.
- Extended the controlled loopback differential test to cover GET, POST, 404 response/body, invalid argument, malformed URL, and refused local port. `kirton_http_loopback` passed AST and Bytecode.
- Full MSVC Release CTest passed **209/209**. Full MSVC ASan CTest passed **209/209** (48.25 seconds); updated loopback test passed under ASan.
- Blocker: a local server delaying response headers for 500 ms still produced a successful HTTP 200 for `http_получить(url, 100)`. `WinHttpSetTimeouts` return values were unchecked; checks are now in place on session and request handles, but neither those settings nor `WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT` enforce the observed 100 ms limit. The delayed-response case remains reproducible and is not left as a known-failing default CTest. A single overall deadline still requires asynchronous WinHTTP operations and cancellation with callback lifetime tracking; that implementation is outstanding. No TCP/UDP API was added because none exists in Kirton 1.1 sources.
- GET/POST and listed HTTP error cases work; the shared overall deadline does not. Keep the HTTP deadline as an open Runtime defect until it is fixed and covered reproducibly.

## Checkpoint — concrete inventory and application launch regression (2026-11-05)

- Reproduced an existing `kirtos_init.kir` launch failure in both AST and Bytecode. The example called an imported module function without its module qualifier; its helper also reused the native math name `лог`, which caused different behavior between the two evaluators.
- Fixed the example to call `kirtos_shell.оболочка_начать()` and renamed the helper to `init_сообщить`. Added `kirton_kirtos_init_example_diff`, which checks that AST and Bytecode each finish and print the shell startup marker. A direct Release CLI comparison from the project root also confirmed exit 0 and identical output in both modes. No language API or public contract changed.
- Rechecked the documented task tracker and expense report from their documented working directory; both pass in AST and Bytecode. The concrete inventory now gives each covered family a works / does-not-work / not-checked status and excludes DirectX. The normative documents do not specify arbitrary computed-callee invocation; the parser accepts those shapes but both evaluators do not execute them, so no language behavior was added.
- Release verification: MSVC 14.44 Release build succeeded; CTest **210/210 passed** in 28.38 seconds. Full MSVC ASan build and CTest **210/210 passed** in 48.23 seconds using `tests/run_msvc_asan.ps1` with a 45-second per-test timeout. ASan log: `build-stage5-asan/Testing/Temporary/MSVC_ASAN_CTest.log`.
- The first Release relink attempt failed because the shell lacked Windows SDK library paths (`LNK1181: winhttp.lib`); the build passed after setting `LIB` to the installed MSVC x64, Windows SDK UCRT, and UM library directories. This was a local build-environment setup issue.
- Still open: parser/AST OOM recovery and the shared HTTP overall deadline. HTTP GET/POST and listed errors pass, but HTTP is not fully ready while the deadline defect remains. No timeout experiment was repeated, no native compilation was started, and SDK 1.2 remains experimental/unreleased.

## Checkpoint — Kirton Compiler 1.2 Preview 1 packaging (2026-11-05)

- Built the separate package `dist/Kirton-Compiler-1.2.0-preview.1-windows-x64.zip` and setup EXE `dist/Kirton-Compiler-1.2.0-preview.1-windows-x64-setup.exe`. Both identify `1.2.0-preview.1`; root `VERSION` and stable SDK 1.0/1.1 artifacts were not changed. ZIP includes CLI, static C API library, headers, source, test source, installer C#/PowerShell sources, examples, docs, MPL-2.0, provenance status, and third-party notices.
- Final Release CTest for the Preview-version build passed **210/210** (28.51 seconds). Full MSVC ASan CTest passed **210/210** (48.85 seconds; 45-second per-test timeout). A Preview critical-group rerun passed **65/65**, including AST/Bytecode differential cases, GC, FFI V2/V3 with and without opt-in, filesystem, HTTP loopback, modules/exceptions, and OpenGL context lifecycle.
- The archive was extracted in a Unicode/space path; packaged CLI version, AST and Bytecode applications, module example, and C host passed. A clean CMake rebuild from the packaged source also reported `Kirton 1.2.0-preview.1`.
- Windows setup EXE was built and its embedded install/uninstall smoke passed. A real per-user install through the bundled installer scripts passed in a unique Preview directory: PATH resolved to the installed executable, `--version` was correct, the task tracker matched in AST/Bytecode, and the installed C host linked and ran. Uninstall removed Preview files, its registry entry, shortcuts, and only its PATH entry; PATH was restored exactly. No stable 1.1 installation was present before the test, and the stable 1.1 ZIP hash remained unchanged.
- The Setup wizard window was compiled but not manually stepped through; the EXE smoke exercised its embedded install/uninstall code path, and the real user-profile installation exercised the exact bundled scripts. Rendered OpenGL output was not visually inspected; only context/shader lifecycle tests were run.
- Final ZIP (1,300,126 bytes) SHA-256: `23459946717A106C5CE3D5A0AFB871C2B43F967297C2B239A9303ACD1548D3D5`. Final setup EXE (1,347,584 bytes) SHA-256: `D7ED8A265A8C1478F0D3841C0613EAFFC13A7C0E3E4DDBBC0FABBD48233E0E2D`. Setup is unsigned because no signing certificate is available.
- OpenGL context/shader lifecycle ran successfully in the Windows graphical test environment; rendered output was not visually inspected. FFI remains opt-in and loads trusted code in-process. Parser OOM and the HTTP overall deadline remain open defects, so this is Preview only and not Stable.
