#!/usr/bin/env python3
"""Deterministic AST/Bytecode/Native differential probe of managed user types.

Usage: python3 tests/native_compiler_user_type_property_probe.py \
 --kirton ./build-release/kirton --seed 20261109 --count 100
"""
import argparse
import random
import subprocess
import tempfile
from pathlib import Path


def checked_run(argv, timeout=20):
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout)
    return (result.returncode, result.stdout, result.stderr)


def generate(rng):
    name = rng.choice(["Киртон 🧭", "Живая строка", "север", "é 🐢", "hello"])
    number = rng.choice([0, 1, 3, -99, 9223372036854775808,
                         115792089237316195423570985008687907853269984665640564039457584007913129639936])
    step = rng.randint(0, 60)
    left = rng.randint(-9, 17)
    right = rng.randint(-9, 17)
    index = rng.randint(-2, 10)
    phase = rng.choice(["готово", "активно"])
    data = f"[{left}, {right}, 3]"
    action = rng.choice(["список_вставить", "список_получить", "список_удалить"])
    if action == "список_вставить":
        action_call = f"печатать(список_вставить(массив, {index}, {step}))"
    elif action == "список_получить":
        action_call = f'печатать(список_получить(массив, {index}, "нет"))'
    else:
        action_call = f"печатать(список_удалить(массив, {index}))"
    return f'''структура Запись {{
    имя: строка
    значение: целое
    массив: список
}}
перечисление Статус {{ готово активно }}
функция шаг(объект, величина) {{
    объект.значение = объект.значение + величина
    вернуть объект
}}
пусть массив = {data}
пусть объект = Запись(имя: "{name}", значение: {number}, массив: массив)
пусть копия = объект
пусть статус = Статус.{phase}
{action_call}
пусть новый = шаг(копия, {step})
печатать(объект.имя, объект.значение, новый == объект, объект.массив == массив)
печатать(статус == Статус.{phase}, статус != Статус.готово, сумма(массив))
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kirton", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--count", type=int, default=100)
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be nonnegative")
    rng = random.Random(args.seed)
    compiler = str(args.kirton.absolute())
    with tempfile.TemporaryDirectory(prefix="kirton-native-types-") as folder:
        root = Path(folder)
        for i in range(args.count):
            source = root / f"case_{i}.kir"
            native_exe = root / f"case_{i}.bin"
            program = generate(rng)
            source.write_text(program, encoding="utf-8")
            ast = checked_run([compiler, str(source)])
            bytecode = checked_run([compiler, "--bytecode", str(source)])
            native_compile = checked_run([compiler, "--native", str(source),
                                          "-o", str(native_exe)])
            if native_compile != (0, "", ""):
                raise RuntimeError(f"case {i}: native compile {native_compile}\n{program}")
            native = checked_run([str(native_exe)])
            if ast != bytecode or ast != native or ast[0] != 0:
                raise RuntimeError(f"case {i}: AST={ast} Bytecode={bytecode} Native={native}\n{program}")
    print(f"PASS {args.count}/{args.count} seed={args.seed}")


if __name__ == "__main__":
    main()
