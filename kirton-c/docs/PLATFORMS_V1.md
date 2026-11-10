# Матрица поддерживаемых платформ Kirton 1.0

## Поддерживаемая платформа

| Target | Компилятор / SDK | Debug | Release CTest | ASan | Установленный Release package |
|---|---|---:|---:|---:|---:|
| Windows x64 | MSVC 14.52.37025, Windows SDK 10.0.26100 | **126/126, 2026-11-04** | **129/129, 2026-11-04** | **126/126, 2026-11-04** | **Собран, установлен и smoke-проверен 2026-11-04** |

Release выполнен из Visual Studio 2026 Developer Command Prompt v18.12.0-insiders
командами:

```powershell
cmake -S . -B build-release -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_INSTALL_PREFIX=<локальный-install-prefix>
cmake --build build-release --config Release --parallel
cmake --install build-release --config Release
cmake --build build-release-test --config Release --parallel
ctest --test-dir build-release-test -C Release --output-on-failure
```

Финальный прогон 2026-11-04 завершился сообщением `100% tests passed out of 126`.
Release package содержит `kirton.exe`, все обязательные документы и примеры.
Из установленной папки примеров успешно запущены `task_tracker.kir`
(`TASK_TRACKER_OK`) и `expense_report.kir` (`EXPENSE_REPORT_OK`); `kirton.exe
--help` также завершился с кодом 0. Production-конфигурация собрана с
`BUILD_TESTING=OFF`.

Debug, Release и ASan прогоны включали все 126 тестов, включая финальное
расширение lexer conformance для цифр, подчёркивания и combining marks в
идентификаторах. Все три конфигурации прошли 2026-11-04.

При подготовке SDK Release был повторно собран с `BUILD_TESTING=OFF`, а полный
CTest из `build-sdk-test` прошёл **129/129**. Новые тесты проверяют C-host
через VM API, пример из Getting Started и флаг `--version`. Portable ZIP распакован в путь с
кириллицей и пробелами; CLI, модульный пример и отдельно собранный C-host
успешно запущены из распакованного SDK.

## Не заявляются в 1.0

| Платформа / сценарий | Результат | Классификация |
|---|---|---|
| Linux/GCC | **BLOCKED**: в текущем workspace нет `gcc`/`clang` и доступной Linux toolchain; исторически 64/64 на более раннем наборе без записанной версии GCC/архитектуры | Не поддерживается в 1.0; повторная квалификация — кандидат 1.0.1 |
| Coverage-guided fuzzing | **BLOCKED**: Clang/libFuzzer не найден в доступном Windows toolchain; WSL Linux-дистрибутив не доступен | 1.0.1 hardening; deterministic malformed-input тесты остаются в CTest |
| Реальный OpenGL/D3D/GPU, async driver lifetimes, окно и аудиоустройство | Не проверялись с реальным драйвером/устройством | Ограничение optional extensions; ручная проверка — 1.0.1 |
| Параллельные вызовы одной VM | Не поддерживаются и не тестируются | За пределами контракта 1.0 |

## Ограничения реализации 1.0

- `window`, `gl`, `d3d11`, `sound` и `bitmap` используют process-global ресурсы.
  Поддерживается одна последовательно используемая VM на процесс; ресурсы не
  изолируются между несколькими VM.
- Внешние HTTP endpoints и реальная доступность сети не входят в offline CTest
  suite; GET и POST проверяются на локальном loopback сервере. Ответы и process
  output ограничены 64 МиБ.
- POSIX process не реализует `рабочий_каталог` и `таймаут_мс`; POSIX HTTP
  возвращает ошибку «не поддержан». Linux не заявлен как платформа 1.0.
- Набор OOM fault-injection покрывает перечисленные в conformance-матрице
  модули, но не каждую ветвь каждого legacy/optional native module.

Полный конечный gate и классификация работ приведены в
[`RELEASE_GATE_V1.md`](RELEASE_GATE_V1.md).
