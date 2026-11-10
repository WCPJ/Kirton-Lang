#!/usr/bin/env python3
"""Deterministic differential probe for compiled user-module dependency graphs.

This is not a proof of full language equivalence. It verifies module identity,
lazy initialization, relative imports, ordinary calls and dynamic values.
"""
import argparse
import os
import pathlib
import random
import subprocess
import tempfile


def run(command, env):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=15, env=env)
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f"timed out running {command}") from exc


def check(kirton, count, seed):
    rng = random.Random(seed)
    with tempfile.TemporaryDirectory(prefix="kirton-module-diff-") as scratch:
        root = pathlib.Path(scratch)
        env = os.environ.copy()
        env["KIRTON_PATH"] = str(root)
        for ix in range(count):
            start = rng.randint(0, 5000)
            shift = rng.randint(1, 120)
            prefix = f"m{ix}"
            (root / f"{prefix}_shared.kir").write_text(
                f'модуль общий\nпечатать("INIT_{ix}")\nпусть значение = {start}\n'
                'функция следующий() {\n'
                '    значение = значение + 1\n'
                '    вернуть значение\n}\n', encoding="utf-8")
            (root / f"{prefix}_left.kir").write_text(
                f'модуль левый\nимпорт "{prefix}_shared" как общий\n'
                'функция извлечь() { вернуть общий.следующий() }\n', encoding="utf-8")
            (root / f"{prefix}_right.kir").write_text(
                f'модуль правый\nимпорт "{prefix}_shared.kir" как общий\n'
                'функция извлечь() { вернуть общий.следующий() }\n', encoding="utf-8")
            # Some graphs invoke the first dependency lazily inside a function.
            top_lines = [
                f'импорт "{prefix}_left" как слева',
                f'импорт "{prefix}_right" как справа',
                f'импорт "{prefix}_shared" как один',
                f'импорт "{prefix}_shared.kir" как два',
            ]
            if rng.random() < 0.5:
                rng.shuffle(top_lines)
            lines = top_lines + [
                'печатать(слева.извлечь())',
                'печатать(справа.извлечь())',
                'печатать(один.следующий())',
                'печатать(два.следующий())',
                f'печатать({shift} + 9223372036854775808)',
            ]
            if ix % 3 == 0:
                lines.extend([
                    'функция внутри() {',
                    f'    импорт "{prefix}_shared" как внутренний',
                    '    вернуть внутренний.следующий()',
                    '}',
                    'печатать(внутри())',
                ])
            entry = root / f"entry_{ix}.kir"
            entry.write_text("\n".join(lines) + "\n", encoding="utf-8")
            expected = None
            for mode in ("ast", "bytecode", "native"):
                command = [str(kirton)]
                if mode == "bytecode":
                    command += ["--bytecode"]
                elif mode == "native":
                    command += ["--native"]
                command += [str(entry)]
                if mode == "native":
                    exe = root / f"exe_{ix}"
                    command += ["-o", str(exe)]
                compiled = run(command, env)
                if mode == "native":
                    if compiled.returncode != 0 or compiled.stderr or compiled.stdout:
                        raise AssertionError(f"case {ix}, native compile: {compiled}")
                    result = run([str(exe)], env)
                else:
                    result = compiled
                actual = (result.returncode, result.stdout, result.stderr)
                if expected is None:
                    expected = actual
                    if expected[0] != 0 or expected[2]:
                        raise AssertionError(f"case {ix}, ast unexpectedly failed: {actual}")
                elif actual != expected:
                    raise AssertionError(f"case {ix}, {mode}: {actual!r} != {expected!r}")
    print(f"module differential {count}/{count} PASS, seed={seed}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--kirton", type=pathlib.Path, required=True)
    parser.add_argument("--count", type=int, default=40)
    parser.add_argument("--seed", type=int, default=20261109)
    args = parser.parse_args()
    check(args.kirton.resolve(), args.count, args.seed)
