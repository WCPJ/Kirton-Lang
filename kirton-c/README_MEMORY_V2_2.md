# Kirton — Memory System V2.2

Продолжение V2.1: появились **структуры с фиксированной раскладкой** и записи внутри арен с проверкой времени жизни, типов и диапазонов.

- Подробности и ограничения: `docs/MEMORY_V2_2.md`
- Пример: `tests/memory_v2_record.kir`
- C API и тест ABI: `tests/memory_v2_record_api.c`
- Нагрузочный тест: `tests/memory_v2_record_pressure.c`
- Код: `src/value.c`, `src/mod_memory.c`, `src/eval.c`, `include/kirton/value.h`

Это **экспериментальный этап**; ни FFI, ни безопасная работа с произвольными C-указателями ещё не реализованы.
