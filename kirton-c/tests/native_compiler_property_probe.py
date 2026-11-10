#!/usr/bin/env python3
"""Reproducible opt-in AST/Bytecode/Native differential smoke probe.

Usage: python3 tests/native_compiler_property_probe.py --kirton <path> --count 400
This test uses only Python stdlib and does not modify the source tree.
"""
import argparse
import pathlib
import random
import subprocess
import sys
import tempfile

NUM = ['-9223372036854775809', '-15', '-1', '0', '1', '2', '7',
       '9223372036854775807', '9223372036854775808',
       '18446744073709551616', '1.25', '-2.5']
INTEGER = NUM[:-2]
STRINGS = ['""', '"A"', '"Б"', '"Киртон"', '"🧭"', '"ab"', '"x\\ny"']
BOOL = ['истина', 'ложь']
NIL = ['пусто']


def expression(rng):
    mode = rng.randrange(10)
    if mode == 0:
        a, b = rng.choices(NUM, k=2)
        return f'({a}) {rng.choice(["+", "-", "*", "/", "нацело", "%"])} ({b})' if b != '0' else f'({a}) + ({b})'
    if mode == 1:
        a, b = rng.choices(NUM, k=2)
        return f'({a}) {rng.choice(["==", "!=", "<", "<=", ">", ">="])} ({b})'
    if mode == 2:
        a, b = rng.choices(INTEGER, k=2)
        return f'({a}) {rng.choice(["&", "|", "^"])} ({b})'
    if mode == 3:
        a = rng.choice(INTEGER)
        return f'({a}) {rng.choice(["<<", ">>"])} ({rng.randrange(0, 8)})'
    if mode == 4:
        a, b = rng.choices(STRINGS, k=2)
        return f'{a} {rng.choice(["+", "==", "!="])} {b}'
    if mode == 5:
        a, b = rng.choices(BOOL, k=2)
        return f'{a} {rng.choice(["и", "или", "==", "!="])} {b}'
    if mode == 6:
        a = rng.choice(NUM + STRINGS + BOOL + NIL)
        b = rng.choice(NUM + STRINGS + BOOL + NIL)
        return f'({a}) {rng.choice(["==", "!="])} ({b})'
    if mode == 7:
        a = rng.choice(NUM + STRINGS + BOOL + NIL)
        return f'не ({a})'
    if mode == 8:
        a = rng.choice(NUM + STRINGS + BOOL + NIL)
        b = rng.choice(NUM + STRINGS + BOOL + NIL)
        return f'({a}) {rng.choice(["и", "или"])} ({b})'
    a = rng.choice(NUM + STRINGS + BOOL + NIL)
    b = rng.choice(NUM + STRINGS + BOOL + NIL)
    return f'не (({a}) == ({b}))'


def run(args):
    try:
        result = subprocess.run(args, capture_output=True, timeout=20)
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -99, b'', b'timeout'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kirton', type=pathlib.Path, required=True)
    parser.add_argument('--count', type=int, default=400)
    parser.add_argument('--seed', type=int, default=20261109)
    ns = parser.parse_args()
    rng = random.Random(ns.seed)
    passed, failures = 0, []
    with tempfile.TemporaryDirectory(prefix='kirton-native-probe-') as tmp:
        work = pathlib.Path(tmp)
        src = work / 'program.kir'
        native_exe = work / ('program.exe' if sys.platform.startswith('win') else 'program')
        for i in range(ns.count):
            expr = expression(rng)
            mode = rng.choice(('direct', 'variable', 'function'))
            if mode == 'direct':
                source = f'печатать({expr})\n'
            elif mode == 'variable':
                source = f'пусть результат = {expr}\nпечатать(результат)\n'
            else:
                source = f'функция передать(v) {{\n    вернуть v\n}}\nпечатать(передать({expr}))\n'
            src.write_text(source, encoding='utf-8')
            ast = run([str(ns.kirton), str(src)])
            bytecode = run([str(ns.kirton), '--bytecode', str(src)])
            compilation = run([str(ns.kirton), '--native', str(src), '-o', str(native_exe)])
            native = run([str(native_exe)]) if compilation[0] == 0 else compilation
            if ast[0] == bytecode[0] == native[0] == 0 and ast[1] == bytecode[1] == native[1] and all(not x[2] for x in (ast,bytecode,native)):
                passed += 1
            else:
                failures.append((i, source, ast, bytecode, native))
                if len(failures) >= 10:
                    break
    print(f'cases={ns.count}, seed={ns.seed}, pass={passed}, mismatch={len(failures)}')
    for i, source, *results in failures:
        print(f'CASE {i}: {source!r}')
        for mode, value in zip(('AST','BYTECODE','NATIVE'), results):
            print(mode, value[0], value[1][:160].decode('utf-8','replace'), value[2][:160].decode('utf-8','replace'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
