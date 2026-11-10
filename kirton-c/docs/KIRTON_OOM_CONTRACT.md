# Kirton OOM Contract — Compiler 1.2

Status: **the command-line program has a fail-fast OOM path; the public Runtime API is not safe to embed in a host that must survive OOM.** Fallible behavior exists in a limited set of operations below. No general promise is made that a Runtime can continue after an allocation failure.

## Command-line behavior

- The common fatal allocators `kir_malloc`, `kir_realloc`, `kir_xmallocn`, `kir_xreallocn`, and `kir_callocn` print `[kirton] OOM (...)` or a size-overflow diagnostic and call `exit(1)`.
- The CLI source-file reader has a separate allocation check and exits with status 70 after printing `kirton: OOM`.
- Fatal allocation/overflow does not unwind parser, AST, VM, GC, BigInt, FFI, or resource state. C `exit()` runs process-level `atexit` callbacks, but Kirton does not guarantee that each live Runtime or adopted resource receives its normal destructor on this path.
- There is no `longjmp`-based recovery. Native plugins are trusted in-process code and may independently crash or terminate the host.

## Public Runtime API behavior

The following APIs **may terminate the host process** when a reachable path calls a fatal allocator: `parse_program`, Value and BigInt constructors, `vm_new`, AST evaluation, and `kirton_bytecode_run*`. Callers must not interpret their integer/pointer return types as a promise that every OOM becomes a return value.

`vm_new()` now returns `NULL` if the separate FFI-state allocation fails after the VM, BigInt heap, and Value heap have been created; those partial objects are destroyed first. Earlier failures while allocating the VM or heaps still use the fatal allocator. `eval_program()` detects this specific `NULL` and returns failure. Bytecode FFI-state initialization returns a runtime error and tears down its temporary VM. These are narrow guarantees, not a general embedded-safe mode.

After a fatal OOM, the host process is gone. After a recoverable operation-level failure, only the local guarantees in the table below apply. There is no general poisoned-runtime state that prevents every later API call after every possible OOM.

## Allocation paths and ownership

| Area / owner | Allocation path | Current result on OOM | Cleanup and evidence |
|---|---|---|---|
| Lexer | `kir_lexer_init*` and `kir_lexer_next` borrow source bytes and allocate no memory | No lexer-owned OOM path; allocating/reading the source is the caller's or CLI reader's responsibility | `kirton_oom_contract lexer` arms the injector and confirms tokenization succeeds without consuming it |
| Parser and AST | AST nodes, token-derived strings, and growing parser arrays use fatal `kir_malloc` / `kir_realloc` | Process exits; parser has no recoverable OOM return and does not destroy the partial tree before exit | `kirton_parser_oom_failfast` runs failure indices in child processes; the parent verifies the OOM diagnostic and also observes later successful indices |
| AST Runtime and roots | `vm_new`, environment/Value/BigInt creation, strings, call argument roots, GC worklists, and many VM tables use fatal `kir_*` allocators | Process exits. `gc_root_push` and GC mark-work growth have no rollback status | `kirton_oom_contract ast_root_sweep` injects across an 80-argument call that grows the root vector; child failures must be fail-fast OOMs. One FFI-init failure is recoverable as described above |
| Bytecode VM | Value/BigInt heaps and Value operations still use fatal `kir_*`; compiler/VM metadata and temporary buffers now use fallible `kir_oom_malloc`, `kir_oom_calloc`, or `kir_oom_realloc` | Fatal Value/BigInt OOM exits. Fallible compiler/VM allocation sets VM failure, returns nonzero, and tears down the temporary BVM | `kirton_oom_contract` sweeps 513 failure indices with an 80-value stack-pressure fixture. It verifies fail-fast exits, fallible errors followed by a successful second VM run, and indices where injection is not reached. Chunk growth retains its old pointer on failed `realloc` |
| Value GC and root registration | Value heap construction, tracking entries, marking worklists, and AST root arrays use fatal allocators | Process exits; no in-process GC recovery is promised | GC mark-worklist OOM is exercised in an isolated child; root growth is covered by the AST root sweep |
| Arena backing store | Arena descriptor uses fatal `kir_malloc`; backing bytes use fallible `kir_oom_calloc` | Backing-store failure frees the untracked descriptor and returns `VAL_NIL`; arena bytes and heap object count stay unchanged | `kirton_oom_contract arena` asserts unchanged accounting and successfully creates/closes another arena |
| BigInt | Heap, BigInt objects, limbs, and arithmetic temporaries use fatal `kir_*` allocators | Process exits; a partially computed BigInt is not returned | `kirton_oom_contract bigint` tests allocation failure in a child process; ordinary BigInt teardown regressions remain in CTest |
| FFI state and calls | State creation, DLL path conversion, loaded-module array growth, and V2 record snapshots use fallible helpers | State creation returns `NULL`; load/call returns failure with an error string. Failed record snapshots end the active borrow and free earlier copies | OOM contract tests state creation and VM initialization; `kirton_ffi_v3_api` injects path/snapshot failures and verifies later FFI calls still work |
| Strings and collections | Core Value string/list/dict allocation uses fatal `kir_*`; selected native module routines use fallible `kir_oom_*` | Core allocation exits. Selected module functions return nil/false/empty values or an error; those local results do not establish a Runtime-wide recovery guarantee | Existing `kirton_string_oom`, `kirton_list_growth_oom`, JSON, regex, path, process, bitmap, and related tests exercise their individual fallible contracts |
| Diagnostics | Most diagnostic storage is fixed-size; Windows console UTF-8 conversion uses a temporary `malloc` buffer | Conversion allocation failure falls back to byte output on stderr | Diagnostic text may be degraded, but this path does not itself call the Kirton fatal allocator |
| CLI input | `src/main.c` uses its own `malloc` wrapper | Prints `kirton: OOM`, exits 70 | CLI-only; not part of the public Runtime API |

The `kir_oom_*` helpers are fallible and deterministic under `KIRTON_ALLOC_TESTING`. They are not used everywhere. Other direct C-runtime allocation paths have these local owners/results:

| Direct path | Owner | OOM result / cleanup |
|---|---|---|
| `src/main.c` input buffer | CLI invocation | `xmalloc` prints `kirton: OOM` and exits 70 |
| `src/diag.c` console conversion buffer | One diagnostic write | Falls back to byte output with `fwrite`; message rendering may lose UTF-8 console formatting |
| `src/callstack.c` frame vector | Process-global diagnostic stack | Failed `realloc` keeps the old vector and omits the new frame; execution continues with an incomplete stack trace |
| `src/mod_pkg.c` command strings | One native package call | Returns false or an empty list; command buffer is freed after use |
| `src/mod_sound.c` file and conversion buffers | One audio load | Closes the file and frees the first buffer if the second allocation fails; returns false |
| `src/mod_window.c` conversion/framebuffer/text buffers | Native window module | Conversion/text failures return or skip; framebuffer creation first releases the previous buffer, then may return false with no buffer |
| `src/mod_gl.c`, `src/mod_d3d11.c`, `src/mod_clipboard.c`, optional `src/mod_draw2d.c` | Temporary upload/conversion buffers plus module-owned graphics/image buffers | Local code generally checks NULL and returns false/zero; not all branches and partial graphics states have fault-injection coverage |
| `src/stb_image.c` decoded pixel buffers | One image load | Returns NULL and releases decoder/source storage on checked failure paths |

`src/ffi.c`, `src/value.c` Arena backing, and direct Bytecode allocations were moved onto fallible helpers in Stage 3. The remaining platform/direct paths are not all under `KIRTON_ALLOC_TESTING`; local NULL checks do not make the Runtime safe against arbitrary system allocator failure.

## Requirements for callers

- Check `vm_new()` for `NULL`, every Runtime return code, and every operation's documented nil/false result. This is necessary but does **not** prevent a fatal allocator from terminating the host.
- Do not use a catch block, signal handler, C++ exception, or `longjmp` to recover across a Kirton fatal allocator. Runtime invariants and partially constructed ownership graphs are not rolled back.
- Do not promise that a failed AST execution or native-module operation leaves the same Runtime safe for arbitrary later use unless that exact operation documents and tests such a guarantee.
- If the host must survive OOM today, isolate Kirton in a child process. This is operational isolation, not an in-process recovery guarantee.

## Confirmed tests vs. unimplemented guarantees

Confirmed on Windows:

- Parser child-process sweep: **11 injected fail-fast exits**, 53 later indices that complete parsing.
- AST/runtime/root sweep over 161 injection points: **112 fail-fast**, one recoverable FFI-init failure followed by a successful second `eval_program`, and 48 indices after the last allocation.
- Bytecode sweep over 513 injection points: **152 fail-fast**, 31 fallible allocation failures that return an error and are followed by a successful second VM run, and 330 indices after the last allocation. The sweep runs under Release and ASan; it caught a realloc-failure AV before the Bytecode `grow` callers were fixed.
- `kirton_oom_contract` directly checks lexer non-allocation, Arena accounting after backing-store failure, FFI-state creation, `vm_new` partial cleanup, Bytecode FFI-init cleanup, GC mark-worklist OOM, and BigInt allocation OOM.
- `kirton_ffi_v3_api` injects FFI path-conversion and record-snapshot failures, then retries successfully; the snapshot failure releases its borrow.
- Existing tests cover strings, lists, JSON, regex, path, filesystem, process/runtime, timers, bitmap, and related fallible native operations.
- Clean Windows Release and ASan full CTest each pass **217/217**. Stage 1 teardown/BigInt and Stage 2 AST/Bytecode parity tests are included. Debug CRT teardown accounting remains zero after successful and failed Bytecode runs.

These checks confirm the listed local outcomes only. Full Release and ASan logs, PrivateUsage measurements, the ASan runtime check, and Stage 1/2 comparisons are recorded in `KIRTON_1_2_AUDIT.md`.

Not implemented: a process-surviving OOM contract for `parse_program`, AST construction/evaluation, general Value/GC/BigInt allocation, or every platform module. Achieving that requires a fallible allocator/result propagated through these layers, rollback for partially built objects, and an explicit Runtime failure/poison state that still permits safe destruction. This needs a separate design and implementation effort. **Kirton 1.2 is not ready to be embedded where OOM must not terminate the host.**
