# Getting started

## 1. Install

Unpack the Windows SDK ZIP, or build from source on Linux as described in
[GENTOO.md](GENTOO.md). No separate interpreter setup is required for a portable
Windows copy. Run `kirton --version` to identify the executable.

## 2. Create and run a program

Save this UTF-8 text as `hello.kir`:

```kirton
печатать("Привет, мир!")
```

From that directory, run `kirton hello.kir` (or `./kirton hello.kir` on Linux).

## 3. Variables

```kirton
пусть имя = "Аня"
пусть счет = 3
печатать("Привет, " + имя)
счет = счет + 1
```

`пусть` declares a binding. Assignment changes an existing binding.

## 4. Conditions

```kirton
пусть температура = 21
если температура > 20 {
    печатать("тепло")
} иначе {
    печатать("прохладно")
}
```

## 5. Loops

```kirton
пусть i = 0
пока i < 3 {
    печатать(i)
    i = i + 1
}
```

## 6. Functions

```kirton
функция удвоить(число) {
    вернуть число * 2
}
печатать(удвоить(6))
```

## 7. Collections

```kirton
пусть числа = [2, 4, 6]
печатать(числа[0])
пусть карточка = {"имя": "Киртон", "версия": 1}
печатать(карточка["имя"])
```

## 8. Modules

Imports resolve relative to the importing source, current directory, and
`KIRTON_PATH`. Module names can use dotted notation and map to `.кир` files.
See `examples/hello_module.kir` and `examples/lib/мир.kir` for the exact
module declaration and import form used by this source tree.

## 9. A larger example

Run `examples/apps/task_tracker.kir` from its directory. It uses multiple
functions and data structures and is part of the Windows CTest suite.

For the full tutorial see [KIRTON_1_1_GUIDE.md](KIRTON_1_1_GUIDE.md), and for
the exact contract see [TECHNICAL_REFERENCE_V1_1.md](TECHNICAL_REFERENCE_V1_1.md).
Legacy `конец` programs remain supported; optional native modules depend on the
operating system.
