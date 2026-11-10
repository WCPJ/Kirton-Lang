# Kirton Compiler 1.2 — stage 6 report

Status: **PARTIAL**. The 208-test Release and MSVC ASan suites pass. Bytecode now integrates selected native APIs, FFI V2/V3, generic resources, and tested Windows graphics lifecycles, but platform breadth, visual validation, and parser OOM recovery remain incomplete. No claim of full Runtime compatibility is made.

## Module inventory

All listed implementations are registered through the shared native registry and callable by AST. Bytecode resolves only registrations explicitly marked safe. “Global” means existing functions are registered without a module prefix; it does not mean every function is Bytecode-enabled.

| Module/API | AST registration / call path | Bytecode | Native resources / permissions | Memory and platform notes | Tests / status |
|---|---|---|---|---|---|
| `list`, `dict`, `str` | Global `KirNativeFn` callbacks (`mod_list.c`, `mod_dict.c`, `mod_str.c`) | Selected callbacks marked safe | Managed `Value` collections/strings; no external capability | Shared Value heap; portable | Core and Bytecode tests; selected functions supported |
| `math`, `json`, `regex`, `path` | Global callbacks (`mod_math.c`, `mod_json.c`, `mod_regex.c`, `mod_path.c`) | Selected callbacks marked safe | No persistent native resource | JSON/regex/path allocate through shared allocators; portable with platform path differences | Release tests cover contracts/OOM for several; selected functions supported |
| `math3d` | Native module `math3d`, `mod_math3d.c` | Module marked safe | Managed values, no external handle | Portable | Bytecode module tests; supported |
| `fs` | Native module and global aliases (`mod_fs.c`) | Explicitly allowlisted; callbacks use the active Value heap where needed | File-system read/write/list/create/delete; paths and permissions follow Runtime/host policy | Platform file APIs; no persistent handles escape callbacks | Controlled temporary-directory AST/Bytecode read/write/list/create/delete/invalid-path test; AST OOM tests |
| `time` | Global callbacks (`mod_time.c`) | Explicitly allowlisted | Reads clock; nondeterministic external state | Platform clock behavior; differential test checks monotonic invariants, not timestamp equality | Bytecode elapsed-time invariant test; partial |
| `http` | Global callbacks (`mod_http.c`) | Explicitly allowlisted | Network access, response buffers | Windows WinHTTP; no persistent handle escapes callback | Local loopback AST/Bytecode GET/POST plus invalid-argument test; partial |
| `process` | Global callbacks (`mod_process.c`) | Explicitly allowlisted | Starts external processes; same host capability as AST | OS-specific process behavior and ownership | Windows controlled echo/exit-code/invalid-argument differential; timeout/cwd cases remain |
| `os` | Native module plus global aliases (`mod_os.c`) | Explicitly allowlisted | Reads/updates environment and reports current directory | OS-specific paths and environment semantics | Bytecode current-directory invariant test; partial |
| `sys` | Native module plus global alias (`mod_sys.c`) | Explicitly allowlisted | Read-only host/KirtonOS system information | Platform and KirtonOS-specific result shapes | AST/Bytecode result-kind differential; partial by platform |
| `pkg` | Native module plus global aliases (`mod_pkg.c`) | Marked safe | Package install/remove/search; platform implementation applies | Windows stub behavior is tested; Linux pacman search/install/remove is not exercised | Windows stub only; partial |
| `runtime` | Global callbacks (`mod_runtime.c`) | All callbacks explicitly allowlisted | Reads process environment/argv and may write a diagnostic message for `ошибка` | Values are constructed in active BVM heap; exact args vary by invocation | Type/env/error/argv/name invariants; partial |
| `память` | Native module (`mod_memory.c`) | Explicitly allowlisted; arenas, buffers, typed arrays, and fixed-layout records run through the active Value heap | Arenas, buffers, typed arrays, record layouts | Shared Value heap; native Values stay rooted by BVM environments during collection | AST/API suite plus Bytecode differential/stress test; PARTIAL pending broader borrow/pin/resource-lifetime coverage |
| `ресурсы` | Native module (`mod_native_resource.c`) | `открыт`/`закрыть` callbacks explicitly allowlisted; native callbacks and FFI can adopt into the active Value heap | Opaque handles, destructor/finalize, close state | Shared `KirValueHeap`; only tested producers/adapters are integrated | AST/Bytecode create/open/catch/finally/close/invalid-descriptor test and FFI resource lease integration; graphics/audio handle breadth remains partial |
| `внешний` / FFI | Explicit module in `native.c`, dispatched via AST VM FFI state (`eval.c`, `ffi.c`) | V2/V3 are available through explicit Bytecode opt-in | Loads trusted native libraries only with `--allow-ffi` | FFI state is owned by BVM; resource finalizers run before plugin unload | AST/Bytecode controlled V2/V3, negative cases, resource leases; partial |
| `bitmap`, `sound` | Global callbacks (`mod_bitmap.c`, `mod_sound.c`) | Marked safe | Image/audio resources and device state | Windows multimedia/device constraints; bitmap also uses `stb_image` | Bitmap load/pixel/free and sound WAV load/free covered; playback not run; partial |
| `window`, `clipboard`, `gl`, `d3d11` | Global callbacks or modules (`mod_window.c`, `mod_clipboard.c`, `mod_gl.c`, `mod_d3d11.c`) | Marked safe | Windows windows, input, graphics contexts and GPU resources | Windows-only; lifecycle tests cover a subset of each API | Bytecode lifecycle and demo smoke tests pass; no visual inspection; partial |
| `draw2d` | Optional global callbacks (`mod_draw2d.c`) | Marked safe when built | CPU framebuffer/BMP buffer | Built only with `KIRTON_LEGACY_DRAW2D=ON` | Optional/not built by default; untested |
| TCP / UDP | No corresponding module implementation found in `src` | NOT AVAILABLE | — | Not present in this checkout; no feature added | NOT AVAILABLE |

AST registration is initiated by `native_init()` in `src/native.c`; module-specific registration and safety declarations are in the source files listed above. The AST dispatch is in `src/eval.c`. Bytecode's import checks and callback invocation are in `src/bytecode.c`; the callback safety table is in `src/native.c`.

## Memory, exceptions, and FFI findings

- Bytecode reuses the project's `Value` and mark/sweep heap; it does not add a second GC. Stage 5 documents GC roots for stack slots, globals, environments, constants, closures, and imported modules. Existing Bytecode GC tests pass in Release. The `память` module is explicitly allowlisted and tested for arena/buffer, typed-array, fixed-record operations, plus 1,200 transient collection allocations while those handles remain live. Filesystem operations, HTTP, process, timers, Runtime, OS, and system-info callbacks now have explicit allowlists and focused AST/Bytecode tests.
- Ordinary callbacks use the active Value heap. FFI was adapted to `KirValueHeap*` and Bytecode owns its FFI state; generic native resources can be adopted into the active heap. Controlled FFI tests cover leases, visitors, invalid handles, and finalizer ordering. Runtime argv/name callbacks construct their results in the active heap.
- Current Bytecode exception handling covers user functions/modules and callback diagnostics. Generic resources and FFI resource leases are exercised through catch/finally, close, visitor, and destructor tests; not every graphics/audio handle family is covered.
- Parser and AST construction still use fail-fast allocators. `kirton_parser_oom_failfast` injects allocation failures in child processes and confirms deterministic diagnostics/termination, but it does not provide recoverable parser OOM. Recoverable unwinding needs an ownership/API refactor to avoid leaking partial ASTs.

## Verification

- Full Release CTest: **209/209 passed** (final run 29.59 seconds).
- Full MSVC ASan CTest: **209/209 passed** (final run 46.44 seconds; 45-second per-test timeout). Test output is saved in `build-stage5-asan/Testing/Temporary/MSVC_ASAN_CTest.log`; use `tests/run_msvc_asan.ps1` to repeat.
- Root cause of the earlier `0xc0000135`: the build was compiled with MSVC 14.44, but its runtime directory was absent from CTest's PATH. A trial with the 14.52 Insiders runtime stalled for five seconds; the matching 14.44 DLL starts correctly. This was an instrumentation-launch problem, not a Kirton failure.
- Module-init constants are now traced from active BVM frames. Without that root, GC could free names from the temporary imported-module init chunk and cause intermittent missing exports/globals. `kirton_bytecode_camera3d_diff` exercises the affected module and passed repeated runs; full application output also matches AST.
- Linux ASan/UBSan/LSan: **not run**; no configured Linux toolchain was established.
- Graphics visual correctness and interactive input: **NOT VERIFIED**.

## Remaining work

Remaining work includes recoverable parser OOM, Linux package-manager behavior, full cross-platform CI, broader native API edge coverage, visual graphics inspection, and more real-application differential runs. Do not call stage 6 complete until the mandatory compatibility gaps close.
