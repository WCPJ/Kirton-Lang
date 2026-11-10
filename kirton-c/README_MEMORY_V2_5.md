# Kirton Memory System V2.5

Изменения V2.5: тип `VAL_NATIVE_RESOURCE`, C API `vm_native_resource_*`,
встроенный модуль Kirton `ресурсы.открыт` / `ресурсы.закрыть`,
GC-финализация нативных ресурсов и контролируемый синхронный `visit`.

Документация, гарантии и ограничения: [`docs/MEMORY_V2_5.md`](docs/MEMORY_V2_5.md).

Локальная проверка:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Не считать универсальным FFI и готовым решением для асинхронных GPU-вызовов.
