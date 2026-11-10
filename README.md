# Kirton 1.3.0

**Kirton** is a UTF-8 scripting language and embeddable C runtime. Version 1.3.0 includes an AST interpreter, a Bytecode VM, and an LLVM-based Native compiler. Source files use the `.kir` extension.

## Русский

### Возможности

- Интерпретатор AST как основной эталонный режим и отдельная Bytecode VM.
- Экспериментальный Native-компилятор: генерирует LLVM IR и вызывает внешний Clang/LLVM для создания исполняемого файла.
- Строки UTF-8, функции и замыкания, списки и словари, модули, структуры, BigInt и обработка исключений.
- Встроенные системные модули, C API для встраивания и FFI для доверенных нативных библиотек.
- Готовые SDK для Windows x64 и Linux x86-64.

AST — рекомендуемый режим исполнения. Native и Bytecode не поддерживают все конструкции AST; Native-компилятор явно сообщает об неподдерживаемых случаях и не переключается автоматически на интерпретатор. C Runtime API пока не гарантирует стабильный ABI. FFI выключен по умолчанию и выполняет нативный код в процессе Kirton.

### Установка SDK

Откройте **Releases** этого репозитория на GitHub и скачайте архив для своей платформы. Перед распаковкой сверьте SHA-256 с `SHA256SUMS` в соответствующем релизе.

**Windows x64:** распакуйте `Kirton-Compiler-1.3.0-windows-x64.zip`. Добавьте папку `bin` распакованного SDK в `PATH` или запускайте CLI по полному пути.

```powershell
& .\Kirton-Compiler-1.3.0-windows-x64\bin\kirton.exe --version
```

**Linux x86-64:**

```sh
tar -xzf Kirton-SDK-1.3.0-linux-x86_64.tar.gz
cd Kirton-SDK-1.3.0-linux-x86_64
./bin/kirton --version
```

Добавьте каталог `bin` распакованного SDK в `PATH`, если хотите запускать `kirton` из любой папки.

### Первая программа за 5 минут

Сохраните в UTF-8 файл `hello.kir`:

~~~text
пусть имя = "мир"
печатать("Привет, " + имя + "!")
~~~

Запустите программу из папки с файлом:

```sh
kirton hello.kir
```

Вывод:

~~~text
Привет, мир!
~~~

Дополнительные режимы:

```sh
kirton --bytecode hello.kir
kirton --emit-llvm hello.kir -o hello.ll
kirton --native hello.kir -o hello
```

Для `--native` требуется внешний совместимый Clang/LLVM; компилятор не входит в SDK. Чтобы загрузить FFI-библиотеки, явно передайте `--allow-ffi` и используйте только доверенный нативный код.

### Сборка из исходников

Нужны CMake 3.15+, компилятор C11 и инструменты сборки. Для полного набора Native-тестов также нужен Clang. Команды для Windows и Linux приведены в [BUILDING.md](BUILDING.md).

Полезные документы: [руководство по запуску](kirton-c/docs/GETTING_STARTED.md), [руководство Kirton 1.3](kirton-c/docs/KIRTON_1_3_GUIDE.md), [справочник CLI](kirton-c/docs/CLI_REFERENCE.md), [ограничения](kirton-c/docs/KNOWN_LIMITATIONS.md).

### Лицензия

Исходники Kirton распространяются по MPL-2.0. Для стороннего компонента `stb_image` действуют отдельные upstream-условия; см. [LICENSE](LICENSE) и [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## English

### Features

- AST interpreter as the reference execution mode, plus a separate Bytecode VM.
- Experimental Native compiler that emits LLVM IR and invokes an external Clang/LLVM toolchain to produce executables.
- UTF-8 strings, functions and closures, lists and dictionaries, modules, structures, BigInt, and exception handling.
- Built-in system modules, a C embedding API, and FFI for trusted native libraries.
- SDK archives for Windows x64 and Linux x86-64.

AST is the recommended execution mode. Bytecode and Native do not support every AST construct. The Native compiler reports unsupported constructs and does not silently fall back to interpretation. The C Runtime API is experimental and has no stable ABI guarantee. FFI is disabled by default and runs native code in the Kirton process.

### Install the SDK

Open **Releases** on GitHub and download the archive for your platform. Verify its SHA-256 against the release's `SHA256SUMS` before extracting it.

**Windows x64:** extract `Kirton-Compiler-1.3.0-windows-x64.zip`. Add the extracted SDK's `bin` directory to `PATH`, or invoke the CLI by its full path.

```powershell
& .\Kirton-Compiler-1.3.0-windows-x64\bin\kirton.exe --version
```

**Linux x86-64:**

```sh
tar -xzf Kirton-SDK-1.3.0-linux-x86_64.tar.gz
cd Kirton-SDK-1.3.0-linux-x86_64
./bin/kirton --version
```

Add the extracted SDK's `bin` directory to `PATH` to run `kirton` from any directory.

### Your first program in five minutes

Save this as UTF-8 text in `hello.kir`:

~~~text
пусть имя = "мир"
печатать("Привет, " + имя + "!")
~~~

Run it from the directory containing the file:

```sh
kirton hello.kir
```

Output:

~~~text
Привет, мир!
~~~

Other execution modes:

```sh
kirton --bytecode hello.kir
kirton --emit-llvm hello.kir -o hello.ll
kirton --native hello.kir -o hello
```

`--native` requires a compatible external Clang/LLVM toolchain, which is not bundled with the SDK. To load FFI libraries, pass `--allow-ffi` explicitly and use trusted native code only.

### Build from source

You need CMake 3.15+, a C11 compiler, and build tools. Clang is also needed to register and run the full Native test suite. See [BUILDING.md](BUILDING.md) for Windows and Linux commands.

Read the [getting started guide](kirton-c/docs/GETTING_STARTED.md), [Kirton 1.3 guide](kirton-c/docs/KIRTON_1_3_GUIDE.md), [CLI reference](kirton-c/docs/CLI_REFERENCE.md), and [known limitations](kirton-c/docs/KNOWN_LIMITATIONS.md).

### License

Kirton source files are distributed under MPL-2.0. The third-party `stb_image` component has separate upstream terms; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
