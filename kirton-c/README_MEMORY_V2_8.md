# Kirton Memory System V2.8 — исходники

Продолжение архива V2.6. Содержит старый FFI ABI V1, новый FFI ABI V2
(проверяемые копии C-совместимых структур, scoped native resource tokens),
долгоживущие native resource leases, исправленный порядок завершения VM.

Подробное описание и точные ограничения: **docs/MEMORY_V2_8.md**.

Linux: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`.

Исторический прогон Linux/GCC прошёл 64/64 Release и 64/64 ASan/UBSan/LeakSanitizer
для прежнего состояния тестового набора; текущие Linux-результаты не подтверждены.
В текущем Windows x64/MSVC окружении Debug, Release и AddressSanitizer прошли
полный набор: 125/125 каждый. Реальные асинхронные GPU операции не проверялись.
