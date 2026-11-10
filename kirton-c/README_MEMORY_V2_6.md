# Kirton Memory System V2.6

Изменения в системе памяти: первый ограниченный C ABI/FFI для внешних библиотек.

Описание архитектуры, API и ограничений: `docs/MEMORY_V2_6.md`.

Сборка и тесты:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Нативные библиотеки запускаются только с явным разрешением `kirton --allow-ffi script.kir`.
