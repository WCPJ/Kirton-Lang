# Kirton Compiler 1.2: architecture and bytecode prototype

Status: experimental prototype; stages 1–7 have partial implementation, including native API, FFI V2/V3, resource, and selected graphics lifecycle integration. Compatibility is not complete. The AST interpreter remains the default and is not
replaced. Run the experimental path with `kirton --bytecode file.kir`; combine
it with `-e/--entry` to call a zero-argument function after module initialization.

## Reuse boundaries

| Existing component | Reuse in the prototype | Limits |
| --- | --- | --- |
| Lexer, parser, AST | Full reuse: bytecode compilation consumes `AstProgram`. | No syntax or parser changes. AST source positions are copied into instructions. |
| `Value`, numeric helpers | Reuse `Value`, truthiness, numeric operations, structural equality, strings, and `v_print`. | Prototype's execution frames are not the AST VM's frames. |
| Value heap and BigInt heap | Reuse existing mark/sweep heaps; roots include stack slots, globals, active environments, constants, closures, and structure fields. | Safepoints run after instructions; closure callbacks trace/release captured environments. |
| AST interpreter / scopes | Kept intact as default and behavioral oracle. | Mirrors the normative contract: module scope and per-call function scope; nested blocks reuse their containing environment. |
| Structures and enums | Reuse existing Value-owned objects and AST type descriptors. | User modules, exceptions, and allowlisted native calls are supported; unsupported callbacks fail explicitly. |
| C API | Existing API signatures remain unchanged; new additive `kirton/bytecode.h` API. | API currently runs a program and returns a status, not an inspectable bytecode object. |

## Design

The prototype compiles parsed AST directly. Instruction records are logical
fixed-width C records (`opcode`, integer operand, `AstPos`); branches address
instruction indices. Each function has an instruction array and a constant
pool of `Value`s. Names are string constants and are resolved through the
active lexical environment chain. Function calls use a recursive C call
frame, one operand stack per run, and dictionary-backed parameters/locals.
This favors a small, easy-to-debug C11 prototype and shares Kirton values; a
stack model avoids assigning stable machine registers before the language's
scope and closure rules are lowered. Instructions are in-memory only. There is
no `.kbc` format or compatibility promise yet; one must define magic, format
version, endianness, numeric encoding, string encoding, validation, and
compatibility policy before serialization.

Each instruction carries the originating AST position (`file`, 1-based line
and column when available). Runtime faults and unsupported constructs report
that position. Parser positions are not guaranteed on every AST node today,
so callers may see zero coordinates.

## Instruction set

| Opcode | Operand | Stack effect | Meaning |
| --- | --- | --- | --- |
| `CONST` | constant index | `+1` | Push literal. |
| `LOAD` | name constant | `+1` | Load local, then global; error if missing. |
| `LET`, `SET` | name constant | `-1` | Bind local/global declaration or update existing local/global. |
| `POP` | — | `-1` | Discard expression result. |
| `PRINT` | argument count | `-n` | Print values in source order, separated by spaces. |
| `NEG`, `NOT`, `BITNOT`, `BOOLIFY` | — | `0` | Numeric negation, truthiness negation, integer complement, boolean normalization. |
| `ADD`, `SUB`, `MUL`, `DIV`, `FLOOR_DIV`, `MOD` | — | `-1` | Arithmetic; string concatenation is supported by `ADD`. |
| `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHL`, `SHR` | — | `-1` | Integer bitwise and shift operations. |
| `MATH_NAN`, `MATH_INF` | name constant | `+1` | Push NaN/infinity if the name is not shadowed in the current environment. |
| `EQ`, `NE`, `LT`, `LE`, `GT`, `GE` | — | `-1` | Structural equality or numeric comparison. |
| `MAKE_LIST` | item count | `1 - n` | Build a list from the top `n` values. |
| `MAKE_DICT` | key descriptor | `1 - n` | Build a dictionary from compiled keys and top stack values. |
| `INDEX` | — | `-1` | Read a list index, string index, or dictionary string key. |
| `SETINDEX` | — | `-3` | Mutate a list element or dictionary string key. |
| `ITER_NEXT` | iterator name, end IP | `0` / loop exit `-2` | Advance live list, UTF-8 string, or typed-array iteration and bind the item. |
| `ITER_END` | — | `-2` | Release iterator operands when a loop exits through `прервать`. |
| `JUMP` | instruction index | `0` | Unconditional branch. |
| `JFALSE`, `JTRUE` | instruction index | `-1` | Pop condition and branch by truth value. |
| `BIND_FUNC` | function ID, name constant | `0` | Create a function Value in the current lexical environment. |
| `PREP_CALL` | call descriptor | `+1` | Resolve callable or constructor before evaluating arguments. |
| `ARG_CHECK`, `ARG_STORE` | call descriptor, argument index | dynamic | Validate/store structure fields in source evaluation order. |
| `CALL_VALUE` | call descriptor | `1 - arity` | Call function Value or finish structure construction. |
| `GET_ATTR`, `ATTR_NAME`, `CHECK_SET_ATTR`, `SET_ATTR` | member/name descriptor | dynamic | Read enum variants/structure fields and mutate structure/dictionary fields. |
| `RET` | — | frame return | Return top value. |
| `END` | — | frame return | Return nil at fallthrough. |

## Supported subset and rejection

Supported operators: +, -, *, /, нацело, %, &, |, ^, <<, >>, comparisons,
и, или, and unary не, -, ~. Logical operators short-circuit. Compound
assignment operators do not exist in the Kirton 1.1 grammar and are not added.

Supported constructs: numeric/string/bool/nil literals; variables and
assignment; print; если/иначе; пока; для каждого; прервать; продолжить; return;
functions as values, argument passing, returned functions, nested functions,
lexical captures and shared captured mutation; structures, enums and their
attributes; lists/dictionaries, nesting, indexing and mutation. Kirton's
existing Value, function-closure callbacks, GC, BigInt, numeric, equality,
structure, and enum implementations are reused. The special zero-argument
builtins нан() and беск() remain supported for NaN/infinity cases.

## Scope contract check

[SYNTAX_V1.md](SYNTAX_V1.md) is normative. [SYNTAX_V1_1.md](SYNTAX_V1_1.md)
states that 1.1 preserves 1.0 language semantics and changes block notation.
The normative contract says if/else/while/for-each do not create separate
environments. AST exec_block passes the current environment into these bodies,
and env_assign searches outward for the nearest binding. Conformance tests
scope_bindings.kir and loop_control.kir exercise these rules. No
specification/AST discrepancy was found, and Bytecode preserves that behavior.

## Coverage matrix

| Area | Bytecode coverage | Boundary |
| --- | --- | --- |
| Operators | Full for Kirton 1.1 expression operators | No compound assignment exists. |
| Logical short-circuit | и / или skip RHS when required | Side effects checked with operator_contract.kir. |
| Scope | Module and function environments; nested blocks reuse the current environment | Function locals shadow module names, matching AST. |
| While | Supported | Nested loops, break, continue, return. |
| Functions/closures | Values, argument/return, nested lexical capture, mutual recursion | Captured environments are callback-traced and reference-retained; cyclic graphs are mark/swept. |
| Structures | Constructor, typed fields, get/set attributes, identity equality, collections | No methods are defined by 1.1; FFI is opt-in and covered separately. |
| Enums | Variants, type identity and variant equality, conditions and collections | Same declaration identity as AST. |
| Attributes | Dictionary/structure reads and writes; enum variant lookup | Missing structure fields and invalid variants are errors. |
| For-each | Lists, UTF-8 strings, and typed arrays | Typed arrays can be constructed through the now-allowlisted `память` callbacks; iteration behavior remains covered by the existing iterator implementation. |
| Numbers | Integers/BigInt, binary64, NaN, infinity, signed zero | Shared Kirton arithmetic helpers and edge tests. |
| Collections | Lists/dictionaries, nested access/mutation, cyclic equality | Existing GC and Value heap. |
| Errors | Division, modulo, shift/bitwise validation, index range | Status/output and error category tests. |

Windows x64/MSVC Release full CTest passed 167/167. The Bytecode-specific MSVC
AddressSanitizer suite passed 32/32. UBSan was unavailable because this host has
no Clang/GCC compiler or configured WSL distribution.

At the end of stage 4, general native builtins except нан()/беск(), modules/imports,
FFI, exceptions, and calls that create typed arrays remained unsupported. Bytecode
never falls back to AST. Stage 5 subsequently added user-module loading and
exceptions. The limited Memory System integration below is a later change.

## Differential checks and benchmarks

Differential tests cover stdout/status and evaluation order, short-circuit side
effects, captured-variable lifetime, function passing/return, mutual recursion,
shared closures, structures/enums/identity/attributes, invalid fields/variants,
wrong-arity side-effect ordering, cyclic struct/collection graphs, and closure
collection. GC pressure results:

| Stress input | Collections | Reclaimed managed objects | Peak managed objects | Live at end |
| --- | ---: | ---: | ---: | ---: |
| 12,000 collection allocations | 308 | 48,000 | 256 | 20 |
| 5,000 returned closures | 98 | 15,003 | 256 | 19 |

Retained collection/closure cycles stayed readable. Final root-aware collection
runs before heap release.

`tests/bench_bytecode.ps1` is the reproducible end-to-end CLI benchmark harness.
It has two measurements: repeated CLI medians (process start, parse, compile,
execute) and a direct C API profiler (parse excluded, compile and execution
separately reported). Both compare exact stdout. Scenarios cover arithmetic,
logical/bitwise operators, calls, while/foreach loops, variable reads, nested
blocks, collections, closure creation/calls, and structure field/transfer
operations. Performance conclusions use only Release-build measurements.

Release measurements on Windows x64/MSVC 14.52, medians across 9 runs per
mode. Direct C API execution excludes parsing; Bytecode compile time is shown
separately. CLI medians varied around 94–109 ms and were dominated by process
startup/runtime initialization, so they do not resolve VM differences.

| Scenario | AST (ms) | Bytecode stage 3 (ms) | Bytecode stage 4 (ms) | Compile stage 4 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Arithmetic | 8.202 | 6.473 | 6.555 | 0.003 |
| Logical/bitwise operators | 5.851 | 4.491 | 4.389 | 0.003 |
| Function calls | 3.266 | 1.043 | 1.212 | 0.001 |
| While loops | 6.812 | 5.069 | 5.249 | 0.001 |
| For-each loops | 2.381 | 2.212 | 2.046 | 0.002 |
| Variable access | 2.071 | 1.750 | 1.686 | 0.002 |
| Nested blocks | 1.756 | 1.413 | 1.415 | 0.001 |
| Collections | 3.267 | 0.925 | 0.874 | 0.003 |
| Closure creation | 3.374 | — | 2.404 | 0.002 |
| Closure calls | 3.256 | — | 2.064 | 0.003 |
| Structure field access | 4.101 | — | 3.062 | 0.002 |
| Structure transfer | 7.864 | — | 4.725 | 0.002 |

Static named calls use a pre-resolved function hint while preserving runtime
scope checks. Variable-held closure calls retain dynamic dispatch. The shared
simple-call fixture is about 15% slower than stage 3 (about 0.16 ms absolute).
Profiling confirmed and removed an extra managed dictionary allocation and
repeated code lookup per call. New closure and structure fixtures are faster
than AST in this run. These single-machine measurements do not establish a
general speedup; compile times are near timer resolution.

## Remaining migration sequence

1. Integrate and test the remaining existing native modules, resource lifetimes,
   and opt-in FFI behavior.
2. Complete AST/Bytecode differential and real-application coverage.
3. Profile and address the native-call regression only after confirming its cause.
4. Harden bytecode verification and run sanitizer builds when available.
5. Consider versioned `.kbc` serialization only after verifier and limits exist.
6. Revisit native compilation separately; LLVM/JIT remains out of scope.

## Stage 6–7 audit update

`память` is now explicitly marked Bytecode-safe in `src/mod_memory.c`. Its existing
callbacks use the currently active Value heap, which the Bytecode VM already sets
around execution. `tests/bytecode_memory_v2_diff.kir` differentially checks arena
and buffer operations, typed arrays, fixed-layout records, and survival across
1,200 transient collection allocations. This does not establish full Memory
System V2.8 parity: parser OOM recovery, Linux package actions, and broader
resource-family coverage remain open.

The current tree has 208 Release and 208 MSVC ASan tests; current full-suite
results are recorded in `COMPILER_PROGRESS_1_2.md`. FFI V2/V3 is available in
Bytecode only with explicit opt-in. `fs`, `os`, `память`, HTTP, process, timers,
`sys`, Runtime, generic resources, bitmap/sound/window/clipboard, GL and D3D11
have selected allowlists and focused tests. Graphics visuals were not inspected.
See `COMPILER_STAGE6.md` and `COMPILER_STAGE7.md` for current limits and release
status.
Kirton 1.2 remains experimental and is not a stable SDK release.
