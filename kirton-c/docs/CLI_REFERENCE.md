# CLI reference

```text
kirton [options] <script.kir> [--] [script arguments...]
```

Options currently implemented by `src/main.c`:

- `-h`, `--help`: print usage and exit 0.
- `--version`: print the configured Kirton version (1.3.0) and exit 0.
- `-e NAME`, `--entry NAME`, `-m NAME`: call the named top-level function after
  top-level initialization. `-m` is retained for compatibility.
- `--bytecode`: execute the script with the Bytecode VM.
- `--emit-llvm`: emit Native Compiler LLVM IR; requires `-o <path.ll>`.
- `--native`: compile to a machine-code executable; requires `-o <path>` and a
  compatible external Clang/LLVM toolchain. Unsupported constructs are rejected;
  there is no interpreter fallback.
- `--allow-ffi`: opt in to loading trusted native C libraries. FFI is disabled
  unless explicitly enabled; this applies to AST, Bytecode, and Native mode.
- `--`: terminate interpreter option parsing; subsequent arguments are passed
  to the script.

Exit codes: `0` success/help/version; `64` usage or option error; `65` parse
failure; `66` source file cannot be opened; `70` allocation failure. Runtime
errors return a nonzero result from the evaluator. Exact runtime codes are an
implementation detail and are not a stable cross-platform API.

Diagnostics are written to stderr and include source locations where the
existing diagnostic path can provide them. Paths and source text are UTF-8.
