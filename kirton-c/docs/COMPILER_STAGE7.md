# Kirton Compiler 1.2 — stage 7 report

Release gate: **NOT CLEARED**. Bytecode remains experimental; no stable SDK 1.2 was created. The current concrete capability statuses and open defects are in `COMPILER_COMPATIBILITY_1_2.md`.

## Compatibility matrix

| Feature group | AST | Bytecode | Differential evidence | Status |
|---|---|---|---|---|
| Kirton 1.1 syntax, literals, declarations, assignment, scope | Supported | Implemented covered forms | Scope and contract fixtures | WORKS for named-call forms; computed-callee forms are unsupported by both evaluators |
| Arithmetic, comparison, logical and bitwise operators | Supported | Supported tested forms | Operator/numeric fixtures | WORKS for tested operators; broader edge/error corpus NOT CHECKED |
| Lists, dictionaries, strings, indexing and mutation | Supported | Core Values and existing list/dict/string callbacks are marked safe | Collection and native builtin differential tests | WORKS for covered APIs |
| Functions, recursion, closures, structures and enums | Supported | Implemented | Closures, user types, cycles and GC differential tests | WORKS for tested behavior |
| Loops and flow control | Supported | Implemented | Loop control/collection differential tests | WORKS for covered cases |
| Exceptions and `finally` | Supported | Implemented for tested paths | Error, finally and module exception tests | WORKS for tested cases; exhaustive production-resource cleanup NOT CHECKED |
| User modules/import/include | Supported | Implemented | Import, cycle, alias, repeat and error differential tests | WORKS for tested behavior |
| Built-in functions/modules | Supported by AST registry | Broad selected native callbacks are allowlisted | 210-test suite and named per-family differential fixtures | WORKS for covered APIs; Linux pacman and some platform API behavior NOT CHECKED |
| GC and Memory System V2.8 | Supported by shared Runtime APIs | Shared Value GC; `память`, generic resources, and FFI resource leases use active Value heap | GC stress, 1,200 transient collection allocations, core smoke, resource destructor-once/finalizer/visitor tests | WORKS for covered tests; cross-platform and very long-running stress NOT CHECKED |
| FFI | Supported with explicit opt-in and trust checks | V2/V3 with explicit Bytecode opt-in | AST/Bytecode positive, negative, no-opt-in and resource integration fixtures | WORKS for controlled fixtures; third-party plugin ecosystem NOT CHECKED |
| Files, paths, HTTP, processes, OS, timers, runtime | Supported where implemented in project modules | `fs`, HTTP, process, timers, `os`, `sys`, time, and Runtime callbacks are allowlisted | Temporary filesystem CRUD, local HTTP loopback, controlled child process, timer polling, system/type/environment/argv/name invariants | HTTP overall deadline DOES NOT WORK; Linux package-manager and listed platform/error breadth NOT CHECKED |
| Parser/AST OOM recovery | Shared parser and AST factories | No recoverable Bytecode-specific path | `kirton_parser_oom_failfast` verifies fail-fast only | DOES NOT WORK (recoverable OOM); open Runtime/compiler defect |
| TCP/UDP | Not present in this checkout | NOT AVAILABLE | Source/registration inventory | No 1.1 language API exists; do not add one in this task |
| Windows window/audio/input/OpenGL | Supported by existing platform modules | Selected window/clipboard/bitmap/sound/GL callbacks allowlisted | Window and GL lifecycle differential tests; bitmap/sound/clipboard checks; demo startup smoke | NOT CHECKED for visual output and full lifecycle/API breadth; DirectX excluded from this inventory |
| `.kbc` serialization | No persistent format required | In-memory bytecode only | No serialization tests | NOT AVAILABLE (intentionally deferred) |
| SDK and CLI release | SDK 1.1.0 present; AST remains default | `--bytecode` is experimental | Existing CLI tests | SDK 1.2 release gate NOT CLEARED |

The matrix is evidence-bounded: “WORKS” means only the forms listed in the differential evidence work.

## Program and suite checks

- Release CTest (`build-stage7-release`): **210/210 passed** after the current MSVC 14.44 Release build (28.38 seconds). Includes module capture, memory/GC, filesystem CRUD, HTTP, process, timers, Runtime, FFI/resource, platform lifecycle, parser fail-fast, and application launch tests.
- AST and Bytecode outputs match for `task_tracker.kir` (`TASK_TRACKER_OK`), `expense_report.kir` (`EXPENSE_REPORT_OK`), and `kirtos_init.kir` (shell startup marker; exit 0). `core_smoke` has a differential CTest. The camera3d demo failure exposed an unrooted active module-init chunk; it is fixed and covered by 25 repeated differential runs.
- `core_smoke`: AST/Bytecode output differential passes. Pong-like and other OpenGL demos were smoke-started in Bytecode and stopped after bounded runs. Orbital Siege and RTS Benchmark are **NOT AVAILABLE** in this checkout. Interactive graphics and rendered images were not visually inspected because the desktop window helper could not expose the Kirton process window.
- Long-duration stress, deep recursion, resource-heavy runs, visual graphics correctness, and installed SDK smoke tests: **NOT RUN** in this pass.
- MSVC ASan CTest: **210/210 passed** (48.23 seconds with a 45-second per-test timeout). Linux sanitizers were not run.

## Performance and packaging

- The MSVC Release harness (`tests/bench_bytecode.ps1`, `build-stage7-release/kirton.exe`, `kirton_bytecode_profile.exe`) ran with **7 samples per mode across 18 scenarios**; all AST/Bytecode outputs matched. CLI medians ranged roughly **92–95 ms**, dominated by process startup. Direct profiler medians (parse excluded; compile and execution are separate) included:

  | Scenario | AST execute (ms) | Bytecode compile (ms) | Bytecode execute (ms) |
  |---|---:|---:|---:|
| Arithmetic | 9.822 | 0.003 | 7.634 |
| Calls | 3.434 | 0.002 | 1.382 |
| Closure calls | 3.451 | 0.003 | 2.186 |
| Collections | 2.088 | 0.005 | 1.097 |
| First module import | 0.059 | 0.032 | 0.054 |
| Module calls | 3.027 | 0.037 | 1.913 |
| 10k native calls (`корень`) | 1.703 | 0.001 | 1.454 |
| Exceptions | 2.077 | 0.002 | 1.497 |

  The initial 100k `корень(81)` probe measured 18.517 ms AST vs 20.364 ms Bytecode. Inspection showed Bytecode looked up the same native slot once during call preparation and again during invocation. The descriptor now carries the resolved safe slot; after this change the same 100k probe measured 17.058 ms AST vs 15.338 ms Bytecode over 11 samples. The 10k harness case likewise shifted from 1.911/1.993 ms (AST/Bytecode) before the change to 1.703/1.454 ms after it. This removes the confirmed duplicate lookup; it is not a general performance guarantee. The direct profiler excludes parse time and does not report GC time or peak memory. CLI timings are process-start dominated.
- Separate 7-run application profiling wrappers measured `task_tracker`: AST 0.150 ms execute, Bytecode 0.078 ms compile + 0.125 ms execute; `expense_report`: AST 0.141 ms execute, Bytecode 0.077 ms compile + 0.117 ms execute. These small programs are not representative of large application workloads.
- `.kbc` remains deferred; in-memory bytecode avoids introducing an unvalidated file format.
- Stable `VERSION` and SDK distributions remain 1.1.0. The separate `Kirton-Compiler-1.2.0-preview.1` ZIP and setup EXE are prepared and install-checked; they are explicitly Preview, not Stable. Preserve AST mode and label Bytecode experimental while parser OOM recovery and the HTTP overall deadline remain open.

## Release gate

Remaining Stable-release work: recoverable parser/AST OOM; the shared HTTP overall deadline; Linux package-manager verification in an isolated stub environment; broader application differentials; visual non-DirectX graphics inspection; and long-duration/resource-heavy stress. The Preview package is not a Stable SDK 1.2 release.
