# Kirton 1.2 Guide

This guide covers the tested Kirton 1.2 syntax and command line. Save source as
UTF-8 `.kir` files. Use `kirton --help` for command options.

## Values, conditions, and functions

```kirton
пусть name = "Mira"
пусть scores = [2, 4, 6]
пусть profile = {"name": name, "active": истина}

функция twice(value) {
    вернуть value * 2
}

если profile["active"] {
    печатать(twice(scores[1]))
}
```

Run `examples/guide_1_2_values.kir` with `kirton examples/guide_1_2_values.kir`;
add `--bytecode` to run it on Bytecode.
The supported syntax uses brace-delimited blocks. Existing legacy programs
using `конец` remain accepted where covered by the compatibility tests.

## Modules and libraries

The runnable sample library is `examples/lib/greetings.kir`:

```kirton
функция greet(name) {
    вернуть "Hello, " + name
}
```

The importing program is `examples/guide_1_2_library.kir`:

```kirton
импорт "examples.lib.greetings"
печатать(greetings.greet("Mira"))
```

Run `kirton examples/guide_1_2_library.kir` or
`kirton --bytecode examples/guide_1_2_library.kir`. Imports are resolved
relative to the importing file and configured module search paths. The
`examples/hello_module.kir` sample demonstrates the same form. Module files
are ordinary Kirton source; the SDK does not require a separate package build.

## Calling functions

Functions can be called directly, through variables, and through supported
attributes or index expressions. For instance, a function assigned to a local
binding can be invoked by that binding. Bytecode evaluates callable expressions
and arguments in source order. Missing attributes and non-callable values raise
runtime errors. See the attribute-call and differential tests for the covered
cases; native module objects may have additional limitations.

## Errors and memory

Use the language's `попробовать` / `поймать` constructs for ordinary runtime
errors. Out-of-memory behavior is a separate runtime failure: some allocation
sites return an error, while others follow the documented fail-fast policy.
The Runtime API does not promise safe continuation after arbitrary OOM. See
`KIRTON_OOM_CONTRACT.md` in this documentation directory.

## Create a reusable library

Place functions in a `.kir` module and import it by its relative module name.
Keep examples beside the module and test both AST and Bytecode when using
features covered in both modes. Native FFI libraries execute in-process and
must be treated as trusted code. C embedding is experimental; consult
`EMBEDDING.md` before linking `lib/kirton`.

## Platform-specific modules

The Linux build omits Windows windowing, clipboard, OpenGL and Direct3D
modules. POSIX HTTP is unsupported, and some POSIX process options are not
implemented. See `KNOWN_LIMITATIONS.md` for the tested platform boundaries.
