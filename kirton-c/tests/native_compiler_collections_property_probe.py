#!/usr/bin/env python3
"""Seeded AST/Bytecode/Native differential probe for collection Value semantics.

Runs compiled native ELF/EXE and compares stdout, stderr, exit status of valid
programs. Failed sources are preserved with the supplied --failures directory.
"""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

VALUES = ('0', '1', '42', '-7', '9223372036854775808',
          '"русский"', '"🐢"', 'истина', 'ложь', 'пусто')
KEYS = ('"α"', '"value"', '"🐢"', '""')


def generate(rng):
    vals = [rng.choice(VALUES) for _ in range(5)]
    key = rng.choice(KEYS)
    first, second, third, fourth, fifth = vals
    out = [
        'функция identity(x) { вернуть x }',
        'функция set_at(x, v) { x[0] = v\n вернуть x }',
        f'пусть a = [{first}, {second}, {third}]',
        'пусть alias = identity(a)',
        f'пусть d = {{{key}: a, "other": [{fourth}, {fifth}]}}',
        'пусть e = identity(d)',
        'печатать(a, d["other"], e["missing"])',
        f'alias[1] = {rng.choice(VALUES)}',
        'печатать(a[1], alias[0], d["other"][1])',
        f'пусть x = set_at(a, {rng.choice(VALUES)})',
        'печатать(x[0], a[0], a == alias)',
        f'e["new"] = [{rng.choice(VALUES)}, {rng.choice(VALUES)}]',
        'печатать(d["new"][0], e["new"][1])',
        f'a = {rng.choice(VALUES)}',
        'печатать(a)',
        'печатать(identity(e)["new"])' if False else 'пусть z = identity(e)\nпечатать(z["new"])',
    ]
    # Unspecified dict key order is not relied on except deterministic print of
    # the same language implementation's insertion-order dictionary.
    return '\n'.join(out) + '\n'


def execute(args, timeout):
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ('TIMEOUT', b'', b'')
    return p.returncode, p.stdout.replace(b'\r\n', b'\n'), p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kirton', required=True, type=Path)
    ap.add_argument('--count', default=100, type=int)
    ap.add_argument('--seed', default=20261109, type=int)
    ap.add_argument('--timeout', default=20, type=int)
    ap.add_argument('--failures', type=Path)
    opts = ap.parse_args()
    rng = random.Random(opts.seed)
    with tempfile.TemporaryDirectory(prefix='kirton-native-collections-') as temp:
        root = Path(temp)
        for i in range(opts.count):
            src = root / f'case_{i}.kir'
            src.write_text(generate(rng), encoding='utf-8')
            ast = execute([str(opts.kirton), str(src)], opts.timeout)
            bytecode = execute([str(opts.kirton), '--bytecode', str(src)], opts.timeout)
            exe = root / f'case_{i}.native'
            compile_result = execute([str(opts.kirton), '--native', str(src), '-o', str(exe)], opts.timeout)
            native = execute([str(exe)], opts.timeout) if compile_result[0] == 0 else compile_result
            # Valid programs must succeed in all three modes, including stderr.
            if ast[0] != 0 or bytecode[0] != 0 or native[0] != 0 or ast != bytecode or ast != native:
                print(f'MISMATCH seed={opts.seed} index={i}', file=sys.stderr)
                for name, result in [('ast', ast), ('bytecode', bytecode), ('native', native)]:
                    print(f'{name}: {result!r}', file=sys.stderr)
                print(src.read_text(), file=sys.stderr)
                if opts.failures:
                    opts.failures.mkdir(parents=True, exist_ok=True)
                    (opts.failures / f'collection_seed{opts.seed}_case{i}.kir').write_bytes(src.read_bytes())
                return 1
    print(f'{opts.count}/{opts.count} PASS, seed={opts.seed}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
