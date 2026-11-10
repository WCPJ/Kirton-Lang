#!/usr/bin/env python3
"""Deterministic AST/Bytecode/native differential probe for the whitelisted stdlib.

This is a supplementary test. It does not assert full language equivalence.
"""
import argparse
import os
from pathlib import Path
import random
import subprocess
import tempfile


def run(argv, cwd, timeout=20):
    result = subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout,
                            encoding="utf-8", errors="replace")
    return (result.returncode, result.stdout, result.stderr)


def program(rng):
    style = rng.randrange(10)
    n = rng.randint(0, 150)
    m = rng.randint(1, 150)
    word = rng.choice(("Киртон", "ёлка", "Привет", "окно", "🧭"))
    if style == 0:
        return f'печатать(строка_заменить("{word}-аа-{word}", "{word}", "Б"))\n'
    if style == 1:
        return f'печатать(строка_разделить("{word}|а|б", "|"))\n'
    if style == 2:
        return (f'пусть исходник = [{n}, {m}, 2]\n'
                'пусть копия = список_копия(исходник)\n'
                'список_установить(копия, 1, 999)\n'
                'печатать(копия, исходник, список_индекс(копия, 999))\n')
    if style == 3:
        return (f'пусть значение = {{"слово": "{word}", "число": {n}}}\n'
                'пусть копия = словарь_копия(значение)\n'
                'печатать(словарь_ключи(копия), словарь_значения(копия))\n')
    if style == 4:
        return f'печатать(минимум({n}, {m}), максимум({m}, {n}), ограничить({n}, 10, 50))\n'
    if style == 5:
        return (f'пусть список = [{n}, {m}, 11]\n'
                'печатать(среднее(список), минимум_списка(список), максимум_списка(список))\n')
    if style == 6:
        return (f'печатать(строка_длина("{word}"), '
                f'начинается_с("{word}", "{word}"), заканчивается_на("{word}", "друг"))\n')
    if style == 7:
        return f'печатать(пол({n}.75), потолок({m}.25), усечь(-{m}.75))\n'
    if style == 8:
        return (f'пусть а = ["{word}", {n}]\n'
                'пусть б = список_соединить(а, [99])\n'
                'печатать(а, б, подсписок(б, 0, 2))\n')
    return (f'пусть словарь = {{"а": {n}, "б": {m}}}\n'
            'печатать(словарь_пары(словарь))\n'
            'печатать(словарь_очистить(словарь), словарь)\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kirton', type=Path, required=True)
    parser.add_argument('--count', type=int, default=100)
    parser.add_argument('--seed', type=int, default=20261109)
    args = parser.parse_args()
    kirton = str(args.kirton.resolve())
    rng = random.Random(args.seed)
    with tempfile.TemporaryDirectory(prefix='kirton-native-stdlib-probe-') as folder:
        cwd = Path(folder)
        for i in range(args.count):
            source = cwd / 'probe.kir'
            exe = cwd / ('probe.exe' if os.name == 'nt' else 'probe')
            source.write_text(program(rng), encoding='utf-8')
            baseline = run([kirton, str(source)], cwd)
            bytecode = run([kirton, '--bytecode', str(source)], cwd)
            native_compilation = run([kirton, '--native', str(source), '-o', str(exe)], cwd)
            if native_compilation != (0, '', ''):
                raise AssertionError(f'case {i}: native compilation failed: {native_compilation}\n{source.read_text()}')
            native = run([str(exe)], cwd)
            if baseline != bytecode or baseline != native:
                raise AssertionError(f'case {i}: mismatched modes:\nAST={baseline}\nBytecode={bytecode}\nNative={native}\n{source.read_text()}')
            if baseline[0] != 0:
                raise AssertionError(f'case {i}: unexpectedly failed: {baseline}\n{source.read_text()}')
    print(f'{args.count}/{args.count} property programs PASS (seed {args.seed})')


if __name__ == '__main__':
    main()
