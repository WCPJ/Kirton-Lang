# Установка и первый запуск Kirton

Этот путь собирает интерпретатор из исходников и устанавливает его в локальный
каталог. Для 1.0 заявлена Windows x64/MSVC; результаты и точные ограничения
приведены в [матрице платформ](PLATFORMS_V1.md) и [release gate](RELEASE_GATE_V1.md).

## Сборка и установка

Нужны CMake и C-компилятор C11. Для Windows используется Visual Studio C++
Build Tools с Windows SDK; для Linux — GCC и стандартные build tools.

```sh
cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix ./install
```

Одноконфигурационные генераторы игнорируют `--config`; Visual Studio использует
его для выбора Release. Каталог установки содержит исполняемый файл в `bin/`,
примеры в `share/kirton/examples/apps/` и документацию в `share/kirton/docs/`.

## Первая программа

Из каталога `share/kirton/examples/apps/` запустите:

```sh
kirton task_tracker.kir
```

Либо укажите полный путь к `bin/kirton` (Windows: `kirton.exe`). Пример выводит
открытые задачи и показывает обработку ошибки. Второй пример запускается как
`kirton expense_report.kir`.

Чтобы убедиться, что собранные исходники проходят проверки:

```sh
cmake -S . -B build-test -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test --config Debug --parallel
ctest --test-dir build-test -C Debug --output-on-failure
```

`BUILD_TESTING=ON` нужен для разработческой проверки. OOM fault-injection hooks
не включаются в production-сборку.
