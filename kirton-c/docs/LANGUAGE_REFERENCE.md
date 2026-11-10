# Kirton language reference

The frozen Kirton 1.0 contract is in [SYNTAX_V1.md](SYNTAX_V1.md). Kirton 1.1
recommends brace-delimited blocks while keeping 1.0 `конец` blocks compatible;
see [SYNTAX_V1_1.md](SYNTAX_V1_1.md).

- UTF-8 source; identifiers may use Cyrillic and Latin characters.
- Statements end at line boundaries or block delimiters. New code should use
  `{ ... }`; `конец` remains supported as legacy syntax.
- Bindings use `пусть`; conditions use `если условие { ... } иначе { ... }`;
  loops use `пока условие { ... }` or `для каждого имя в список { ... }`.
- Functions use `функция имя(параметры) { ... }`; `вернуть` returns a value.
- Lists use `[...]`; dictionaries use `{key: value}` in expression position.
- Modules use `импорт` and `включить`; resolution details are documented in
  `SYNTAX_V1.md` and `include/kirton/eval.h`.
- `попытка`/`поймать`/`наконец` provide the existing exception handling form.

See [CONFORMANCE_V1.md](CONFORMANCE_V1.md) for behavior backed by tests. Native
module names and signatures are evolving; use the checked-in examples and
module source as the current reference.
