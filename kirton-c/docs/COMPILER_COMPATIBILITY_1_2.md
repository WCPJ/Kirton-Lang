# Kirton 1.1 / Bytecode compatibility inventory

Audit scope: current AST/Bytecode sources, native registrations in `src/native.c` and `src/mod_*.c`, named examples, and the current CTest suite. DirectX/D3D11 is excluded from this inventory as requested. “Works” is limited to the named forms, test cases, and platform; untested behavior is called out separately.

## Concrete status summary

| Capability | Status | Evidence and remaining boundary |
|---|---|---|
| Literals, variables, assignment, print, expressions, covered operators, collections, indexing, loops, named calls, functions/closures, structures/enums, user-module imports | **WORKS** | AST/Bytecode differential fixtures cover these forms; `task_tracker.kir` and `expense_report.kir` both run in AST and Bytecode. Computed-callee calls (`factory()(...)`, `table[key](...)`) are not specified as a required call form and are unsupported by both evaluators. |
| Exceptions and `finally` | **WORKS** | Covered throw/catch/finally and module-error cases pass in both VMs. Exhaustive interaction with every production native resource family is **NOT CHECKED**. |
| Parser/AST OOM recovery | **DOES NOT WORK** | Controlled allocation injection verifies fail-fast diagnostics only. Recoverable parsing is an open Runtime/compiler task. |
| Memory System V2.8 typed arrays, arenas, buffers, fixed-layout records | **WORKS** | Existing API and Bytecode differential/pressure tests pass. Script-level borrowing and pinning are **NOT CHECKED** as language features and are not claimed to exist. |
| Generic native resources and FFI V2/V3 | **WORKS** | Covered generic resource lifecycle and controlled FFI fixtures pass with explicit opt-in. Production native-handle coverage and third-party plugin ecosystem coverage are **NOT CHECKED**. |
| Collections/string/math/json/regex/path and filesystem CRUD | **WORKS** | Covered native APIs pass; filesystem evidence is Windows-specific. Broader platform path/encoding behavior is **NOT CHECKED**. |
| OS/environment, time formatting, process timeout/working directory, exact runtime argv values | **NOT CHECKED** | Only valid environment set/readback, monotonic time, process echo/exit, and argv/name result shape are covered. |
| HTTP GET/POST and response/error handling | **WORKS** | Loopback GET/POST, 404 body/fields, malformed URL, invalid argument, and refused-port behavior pass. The shared overall HTTP deadline **DOES NOT WORK** and remains an open Runtime defect; see below. |
| Timers and `sys` result shape | **WORKS** | Creation, polling, cancellation, invalid timer input, and result-type invariants pass. Broader platform-specific content is **NOT CHECKED**. |
| Linux `pkg` pacman operations | **NOT CHECKED** | Only the Windows stub behavior is tested; no host package-manager operation was run. |
| Bitmap decoding, sound playback, clipboard writes, window input/rendering, OpenGL visual output | **NOT CHECKED** | Bounded lifecycle/read/invalid-argument cases exist for some APIs, but decoded-image corpus, device output, clipboard mutation, interactive input, and rendered visuals were not verified. |
| Optional legacy `draw2d` | **NOT CHECKED** | Not built in the default configuration. |
| TCP/UDP | **NOT AVAILABLE** | No 1.1 syntax, module, or callback exists; no API is being added. |
| Persistent `.kbc` bytecode | **NOT AVAILABLE / DEFERRED** | Kirton 1.1 does not require persistent bytecode; current VM compiles in memory. |

## Open Runtime defects

- **HTTP overall deadline — OPEN.** GET/POST, response fields, HTTP error responses, malformed URLs, invalid arguments, and connection errors have passing loopback coverage. The timeout argument still does not impose the promised end-to-end deadline: the recorded reproduction returned HTTP 200 after a 500 ms delayed response despite a 100 ms timeout. Fixing this requires a separate asynchronous WinHTTP execution/cancellation design. Do not describe HTTP as fully ready. This task did not repeat the WinHTTP timeout experiment and did not change the public callback/API contract.
- **Parser/AST OOM recovery — OPEN.** `kirton_parser_oom_failfast` confirms controlled fail-fast behavior, not recoverable parsing. Parser recursion, AST factories, and ownership-transfer helpers still lack fallible status propagation and partial-tree cleanup.

## Computed-callee specification check

The normative `SYNTAX_V1.md` lists calls as expressions and specifies resolution
for an unqualified *name* and for module-qualified calls, but it does not give a
postfix grammar or semantic rule for invoking an arbitrary returned/indexed
function value. The 1.1 technical EBNF also does not define such a production.
The parser currently accepts repeated postfix calls such as `factory()()` and
`table[key](...)`, but this syntax is not a specified 1.1 requirement; neither
AST nor Bytecode executes it. Preview treats these forms as unsupported and
does not add language behavior. This is not recorded as an AST/Bytecode
divergence.

The compiler inventory continues below with per-family API names and evidence. Statuses there describe only covered operations, not blanket platform or product readiness.

## Language constructs and value kinds

| Feature | AST | Bytecode | Differential test | Blocker / limitation | Status |
|---|---|---|---|---|---|
| Number, string, bool, nil literals | Yes | Yes | numeric, string, truthiness tests | None known in covered forms | WORKS |
| Operators `+ - * / нацело % & \| ^ << >>`, comparisons, `и/или`, unary `- не ~` | Yes | Yes | operator/numeric tests | Broader edge/error corpus not checked | WORKS (covered operators) |
| Variables, assignment, print, expression statements | Yes | Yes | scope/contract tests | None known in covered forms | WORKS |
| Lists, dictionaries, indexing, element/field mutation | Yes | Yes | collection, identity, cycle tests | None known in covered forms | WORKS |
| `если/иначе`, `пока`, `для каждого`, break/continue | Yes | Yes | loop and flow tests | Broader iterable types are not checked | WORKS (covered forms) |
| Functions, recursion, closures, captured mutation, return values | Yes | Yes | closure/GC tests | Computed-callee calls are outside the specified 1.1 contract and unsupported by both evaluators | WORKS (specified named calls) |
| Direct named calls and named/module-attribute calls | Yes | Yes | function/module tests | Normative docs do not define `factory()(...)` or `table[key](...)`; the parser accepts these shapes but neither evaluator supports them | WORKS (specified call forms); computed-callee behavior NOT SPECIFIED |
| Structures and enums | Yes | Yes | user-type/identity tests | Methods are not in the 1.1 AST/type model | WORKS (covered forms) |
| User modules, aliases, nested/repeated imports, include | Yes | Yes | module/cycle/cache tests | Native modules are separately gated by allowlist | WORKS (tested user modules) |
| `попробовать/перехватить/всегда` | Yes | Yes | error/finally/module tests | Production resource cleanup interactions are not checked exhaustively | WORKS (tested cases) |
| Parser/AST allocation failure recovery | Fail-fast `kir_panic_oom()` exits with a diagnostic | Parser behavior is shared; no recoverable Bytecode-specific path | `kirton_parser_oom_failfast` injects controlled allocation failures in subprocesses | Recursive parser and AST factory/push APIs lack fallible status propagation and partial-tree cleanup | DOES NOT WORK (recoverable OOM); fail-fast works |
| Typed arrays, arenas, buffers, fixed-layout records | Yes | Callbacks enabled through `память`; indexed/iteration Values work | `bytecode_memory_v2_diff`, memory API suite | Script-level borrowing/pinning are not exposed by current language callbacks | WORKS (tested APIs); borrowing/pinning are NOT CHECKED |
| Opaque native resources | C/AST Runtime API exists | `ресурсы.открыт/закрыть` allowlisted; synchronous callbacks/FFI can adopt into active Value heap | `resource_finally.c`; FFI V3 lease/visitor/finalizer checks | Production resource-producing callback coverage and host embedding parity are not checked | WORKS (generic resource tests) |
| FFI values/ABI | AST VM supports FFI v2/v3 with opt-in | FFI v2/v3 enabled only via `--allow-ffi` / `kirton_bytecode_run_with_ffi(..., 1)` | Bytecode FFI v2/v3 differential, resource integration, and no-opt-in rejection | Third-party plugin ecosystem and broad hostile-plugin stress are not checked | WORKS (controlled fixtures) |
| `.kbc` persistent bytecode | Not required by 1.1 | In-memory only | None | Serialization/verifier intentionally deferred | NOT AVAILABLE (deferred) |

Kirton `Value` kinds found in `include/kirton/value.h`: `VAL_NIL`, `VAL_NUM` (exact integer/BigInt or binary64), `VAL_BOOL`, `VAL_STR`, `VAL_LIST`, `VAL_DICT`, `VAL_ARENA`, `VAL_BUFFER`, `VAL_TYPED_ARRAY`, `VAL_RECORD_LAYOUT`, `VAL_RECORD`, `VAL_NATIVE_RESOURCE`, `VAL_FUNCTION`, `VAL_USER_STRUCT`, `VAL_ENUM_VALUE`. Bytecode directly creates/roots the language values; `память` callbacks now create the five arena-backed kinds in the active Bytecode Value heap. Native resources and FFI remain the gaps above.

## Native functions and modules

The AST calls `KirNativeFn(int argc, const Value* argv)` through the shared registry. Bytecode uses the same callback, but `kir_native_call_bytecode` rejects every registry slot not explicitly marked safe. Rows list the existing Kirton names that remain unmarked or unimplemented; the cause column records why they were left out. `ос`/`time`/`fs`/`память`/HTTP support was added during this work; verification remains bounded to the tests listed.

| API / existing functions | AST | Bytecode | Differential test | Blocker / limitation | Status |
|---|---|---|---|---|---|
| `list`: `список_пустой`, `_длина`, `_копия`, `_добавить`, `_вставить`, `_удалить`, `_получить`, `_установить`, `_очистить`, `_соединить`, `подсписок`, `_индекс` | Yes | All callbacks marked safe | collection/native builtin tests | No module import; existing global API | WORKS (tested callbacks) |
| `dict`: `словарь_пустой`, `_длина`, `_копия`, `_ключи`, `_значения`, `_пары`, `_есть`, `_получить`, `_установить`, `_удалить`, `_объединить`, `_очистить` | Yes | All callbacks marked safe | collection/native builtin tests | Existing global API | WORKS (tested callbacks) |
| `str`: `строка_найти`, `_заменить`, `_разделить`, `_обрезать`, `начинается_с`, `заканчивается_на`, `_длина`, `срез`, `подстрока`, `_склеить`, `в_верхнем`, `в_нижнем`, plus `str_len` | Yes | All callbacks marked safe | string/native builtin tests | Existing global API | WORKS (tested callbacks) |
| `math` | Yes | All `MATH_FUNCS` callbacks marked safe | numeric/math OOM tests; Bytecode profile/differential calls | Random and time-dependent results need invariant-based rather than raw equality tests | WORKS (tested callbacks) |
| `json`, `regex`, `path` | Yes | Every current callback marked safe | JSON depth/numbers, regex, path OOM and API tests | Path results are platform dependent | WORKS (tested callbacks) |
| `math3d` | Yes | Entire module marked safe | math3d module fixture in stage 5 | No persistent native resources | WORKS (tested forms) |
| `fs`: `фс_прочитать_текст`, `фс_записать_текст`, `фс_есть`, `фс_список`, `фс_создать_папку`, `удалить_файл`, `удалить_папка` | Yes | Entire module and globals marked safe | Controlled temporary-directory AST/Bytecode read/write/list/create/delete/invalid-path test; AST OOM tests | Platform-specific path encoding/collision errors remain | WORKS (tested Windows cases) |
| `os`: `окружение`, `установить_окружение`, `текущая_папка` | Yes | Module and globals marked safe | Current directory plus controlled environment set/readback | Invalid names/values and other platforms remain untested | WORKS (tested Windows cases); remaining cases NOT CHECKED |
| Time: `время_сейчас`, `_utc`, `эпоха_сек`, `_мс`, `монотонное_сек`, `_мс`, `ждать`, `_мс`, `таймер_старт`, `_прошло`, `форматировать_время` | Yes | All callbacks marked safe | Monotonic elapsed-time invariant | Formatting/timezone behavior and delay bounds not covered by differential tests | NOT CHECKED (beyond monotonic elapsed time) |
| HTTP: `http_получить`, `веб_получить`, `http_запрос`, `веб_запрос` | Yes (WinHTTP on Windows; explicit unsupported result on other platforms) | Same callbacks marked safe for Bytecode | Local loopback AST/Bytecode GET/POST/404, response fields, malformed URL, invalid argument, closed-port connection error | Recorded delayed-header reproduction returned HTTP 200 after 500 ms for a 100 ms timeout; async cancellation and staged-body deadline cases remain | DOES NOT WORK (overall deadline); other listed cases WORK |
| Process: `процесс_выполнить`, `процесс_запуск`, `процесс_запуск_и_ждать`, `process_выполнить`, `exec` | Yes | All callbacks marked safe | Windows Bytecode/AST child process echo, exit status, invalid-argument result | Linux and timeout/working-directory/error cases remain untested | WORKS (tested Windows cases); remaining cases NOT CHECKED |
| Timers: `текущее_время_мс`, `таймер_после`, `таймер_каждый`, `таймер_отменить`, `таймеры_шаг`, `таймер_следующее_ожидание`, `таймер_инфо`, `таймеры_очистить` | Yes | All callbacks marked safe | Timer creation, due event polling, interval tick, cancellation, invalid argument, no-timer state | Process-global timer table; tests clear it before/after each run | WORKS (tested behavior) |
| Runtime: `аргументы`, `progname`, `scriptname`, `тип`, `окружение`, `переменная`, `ошибка` | Yes | All callbacks marked safe; process metadata values are constructed in the active heap | Type/env/error-value and argv/name result-kind invariants | Exact argv values differ by invocation and were not compared | WORKS (result shape); exact argv contents NOT CHECKED |
| `sys`: `система_инфо` | Yes (platform-specific information; empty result on unsupported platforms) | Module and callback marked safe | AST/Bytecode result-type invariant | Platform contents differ; only result kind is compared | WORKS (result kind) |
| `pkg`: `пакеты_установить`, `пакеты_удалить`, `пакеты_поиск` | Linux pacman implementation; Windows stub | Marked safe | `bytecode_pkg_diff` verifies Windows stub behavior | Real pacman search/install/remove path is not tested and host package manager was not invoked | NOT CHECKED (Linux implementation) |
| `ресурсы`: `закрыть`, `открыт` | Yes | Module marked safe; current-heap resource adoption is used by native callbacks and FFI | AST/Bytecode close-on-finally, destructor-once, invalid descriptor; FFI V3 lease/visitor/finalizer tests | Production resource coverage does not yet include every non-graphics/audio handle | WORKS (generic tests); broader native handles NOT CHECKED |
| FFI `внешний`: `загрузить`, `вызвать` | Yes only after host/CLI explicit opt-in | V2/V3 supported with explicit opt-in | AST/Bytecode V2/V3 positive/negative fixtures, resource integration, no-opt-in rejection | Full external plugin ecosystem has not been audited | WORKS (controlled ABI fixtures); third-party coverage NOT CHECKED |
| `bitmap`: `картинка_загрузить`, `_освободить`, `_размер`, `_пиксель` | Yes on supported builds | Marked safe | Bytecode/AST load, dimensions, pixel, free, invalid handle | Decoded image corpus and platform variation remain | WORKS (fixtures); broader corpus NOT CHECKED |
| `sound`: `звук_загрузить`, `_играть`, `_играть_цикл`, `_остановить`, `_освободить` | Windows implementation; stubbed/absent elsewhere | Marked safe | Bytecode/AST WAV load/free and invalid arguments | Audio playback/device output was not tested | NOT CHECKED (playback/device output) |
| `window`: `окно`, `окно_открыто`, `очистить`, `пиксель`, `линия`, `прямоугольник`, `окружность`, `текст`, `живо_шаг`, `FPS_цель`, keyboard/mouse/key state, `размер_окна`, `установить_заголовок`, text metrics | Windows only | Marked safe | Bytecode/AST open, clear, pixel, poll, cleanup lifecycle | Visual output and interactive input/state breadth were not fully checked | NOT CHECKED (visual/input breadth) |
| `clipboard`: `буфер_прочитать`, `_записать`, `клипборд_прочитать`, `_записать` | Windows only | Marked safe | Bytecode/AST read and invalid write arguments | Clipboard content write was not exercised | NOT CHECKED (write behavior) |
| `gl` | Windows OpenGL callbacks: window/context lifecycle, compatibility/core and modern buffers/shaders/textures/input | Marked safe | Bytecode/AST compatibility/core context, shader, vertex/index buffer lifecycle; demo smoke starts | Visual output and broad texture/input behavior were not checked | NOT CHECKED (visual/API breadth) |
| Optional `draw2d`: `img_создать`, `_удалить`, `_очистить`, `пиксель`, `линия`, `прямоугольник`, `окружность`, `img_сохранить_bmp` | Only with `KIRTON_LEGACY_DRAW2D=ON` | Marked safe when built | Not in default build | Optional legacy backend is excluded from normal build and was not built/tested | NOT CHECKED (not built) |
| TCP / UDP | No module or callback found in `src` | Not available | Source/registration inventory | Not part of this checkout's Kirton 1.1 API surface; no API added under this task | NOT AVAILABLE in Kirton 1.1 |

## Scope boundaries found in the AST/Bytecode compiler

`AstExprKind` contains all 12 kinds in `include/kirton/ast.h`. Bytecode lowers the call forms supported by the AST evaluator: a variable or a module/member attribute rooted at a variable. Computed-call shapes such as `make()()` and `table[key](...)` are not invoked by the AST evaluator in this 1.1 implementation (they return nil or fall through to a bare-member lookup), and Bytecode diagnoses them at compile time. A function first assigned to a variable and then called by name is supported.

`AstStmtKind` contains print, expression, let/set, return, break/continue, if, while, foreach, indexed/attribute assignment, import/include, nested function, and try/catch/finally; each has a Bytecode lowering. The additional compile-time rejection is import of a registered native module with no safe callback. No compound assignment or TCP/UDP syntax exists in this 1.1 grammar. Bytecode has no AST fallback.
