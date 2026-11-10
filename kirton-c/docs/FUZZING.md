# Coverage-guided fuzzing

В дереве есть libFuzzer harness для JSON parser/serializer и BMP loader:
`tests/fuzz_json_libfuzzer.c`, `tests/fuzz_bmp_libfuzzer.c`. Для сборки нужен
Clang с libFuzzer; включите option `KIRTON_BUILD_FUZZERS`:

```sh
CC=clang cmake -S . -B build-fuzz -G Ninja \
  -DBUILD_TESTING=OFF -DKIRTON_BUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
mkdir -p corpus-json corpus-bmp
build-fuzz/kirton_fuzz_json -max_len=65536 corpus-json
build-fuzz/kirton_fuzz_bmp -max_len=1048576 corpus-bmp
```

JSON harness создаёт отдельные VM heaps для каждого input и прогоняет проверку,
разбор и сериализацию. BMP harness записывает input в временный BMP, запускает
загрузчик, освобождает успешно загруженный ID и удаляет файл.

В текущем Windows toolchain есть MSVC и ASan runtime, но Clang/libFuzzer compiler
не установлен. Поэтому harness и CMake option добавлены, но coverage-guided run
ещё не подтверждён. Deterministic malformed-input smoke тесты входят в CTest.
