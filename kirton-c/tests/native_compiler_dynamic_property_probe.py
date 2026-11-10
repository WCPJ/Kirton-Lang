#!/usr/bin/env python3
"""Deterministic AST/Bytecode/Native generated-program differential probe.

Not a replacement for sanitizers; reproduces seeds and failing source files.
Examples: python3 tests/native_compiler_dynamic_property_probe.py --kirton /path/kirton --count 120 --seed 20261109
"""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

VALUES = ["0", "1", "-3", "42", "9223372036854775808", "-18446744073709551616",
          '""', '"Киртон"', '"🧭"', '"значение"', "истина", "ложь", "пусто"]

def generate(rng):
    a, b, c, d, e, f = (rng.choice(VALUES) for _ in range(6))
    flag = rng.choice(["истина", "ложь"])
    # Every program contains polymorphic calls of the same functions.
    lines = [
        f"пусть общее = {a}",
        "функция сохранить(x) { общее = x\n вернуть общее }",
        "функция удвоить(x) { вернуть x + x }",
        "функция эквивалентно(x,y) { вернуть x == y }",
        "функция выбрать(flag,x,y) { если flag { вернуть x } вернуть y }",
        "функция частично(flag,x) { если flag { вернуть x } }",
        "функция сравнить(x,y) { вернуть x < y }",
        f"печатать(сохранить({b}))",
        f"печатать(удвоить({c}))",
        f"печатать(удвоить({d}))",
        f"печатать(эквивалентно({c},{e}))",
        f"печатать(эквивалентно({f},{b}))",
        f"печатать(выбрать({flag},{b},{c}))",
        f"печатать(выбрать(не {flag},{d},{e}))",
        f"печатать(частично({flag},{f}))",
        f"печатать(частично(не {flag},{e}))",
        f"печатать(сравнить({a},{d}))",
        f"печатать(сравнить({f},{c}))",
        f"общее = {d}",
        "печатать(общее)",
        "печатать(общее == пусто)",
    ]
    return "\n".join(lines) + "\n"

def run(args, timeout):
    try:
        p = subprocess.run(args, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", b"", b"")
    return (p.returncode, p.stdout.replace(b"\r\n", b"\n"), p.stderr)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kirton", type=Path, required=True)
    ap.add_argument("--count", type=int, default=120)
    ap.add_argument("--seed", type=int, default=20261109)
    ap.add_argument("--timeout", type=int, default=20)
    ap.add_argument("--failures", type=Path, default=None)
    opts = ap.parse_args()
    rng = random.Random(opts.seed)
    failure_dir = opts.failures
    with tempfile.TemporaryDirectory(prefix="kirton-native-dynamic-probe-") as td:
        workspace = Path(td)
        for i in range(opts.count):
            src = workspace / f"generated_{i}.kir"
            src.write_text(generate(rng), encoding="utf-8")
            ast = run([str(opts.kirton), str(src)], opts.timeout)
            bytecode = run([str(opts.kirton), "--bytecode", str(src)], opts.timeout)
            binary = workspace / f"generated_{i}.native"
            compile_result = run([str(opts.kirton), "--native", str(src), "-o", str(binary)], opts.timeout)
            native = run([str(binary)], opts.timeout) if compile_result[0] == 0 else compile_result
            if ast != bytecode or ast != native or ast[0] != 0 or ast[2]:
                if failure_dir:
                    failure_dir.mkdir(parents=True, exist_ok=True)
                    (failure_dir / f"failure_seed{opts.seed}_case{i}.kir").write_bytes(src.read_bytes())
                print(f"MISMATCH seed={opts.seed} index={i}\n{src.read_text()}", file=sys.stderr)
                for name, output in [("AST", ast), ("BYTECODE", bytecode),
                                     ("NATIVE_COMPILE", compile_result), ("NATIVE", native)]:
                    print(name, repr(output), file=sys.stderr)
                return 1
            if (i + 1) % 25 == 0:
                print(f"{i+1}/{opts.count} PASS", flush=True)
    print(f"{opts.count}/{opts.count} PASS; seed={opts.seed}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
