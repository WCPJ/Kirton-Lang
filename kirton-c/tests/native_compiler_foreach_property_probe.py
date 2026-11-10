#!/usr/bin/env python3
"""Deterministic AST / Bytecode / compiled Native differential test for 0.6C."""
import argparse
import random
import subprocess
import tempfile
from pathlib import Path


def run(command, limit=20):
    try:
        p = subprocess.run(command, capture_output=True, timeout=limit)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, b"", b"TIMEOUT"


def script_for(rng, case):
    values=[rng.randint(-4,8) for _ in range(rng.randint(0,12))]
    numbers=', '.join(map(str,values))
    cutoff=rng.randint(-2,6)
    skip=rng.randint(-2,6)
    chars=['ж','🧭','a','B','🐢','ё']
    sample=''.join(rng.choice(chars) for _ in range(rng.randint(0,12)))
    stop=rng.choice(chars)
    if case%4==0:
        return f'''функция сумма_проверки_натив(значения) {{
    пусть ответ = 0
    для каждого x в значения {{
        если x == {skip} {{ продолжить }}
        если x == {cutoff} {{ прервать }}
        ответ = ответ + x
    }}
    вернуть ответ
}}
пусть значения = [{numbers}]
печатать(сумма_проверки_натив(значения))
печатать(список_длина(значения))
'''
    if case%4==1:
        return f'''пусть текст = "{sample}"
пусть вывод = ""
для каждого буква в текст {{
    если буква == "{stop}" {{ продолжить }}
    вывод = вывод + буква
}}
печатать(вывод)
'''
    if case%4==2:
        return f'''пусть значения = список_пустой()
для каждого x в [{numbers}] {{
    список_добавить(значения,x)
}}
пусть словарь = словарь_пустой()
пусть сумма = 0
для каждого x в значения {{
    сумма = сумма + x
}}
печатать(список_длина(значения), сумма)
печатать(словарь_установить(словарь,"сумма",сумма))
печатать(словарь_длина(словарь), словарь_есть(словарь,"сумма"))
'''
    limit=rng.randint(0,15)
    return f'''функция расчет(n) {{
    пусть результат = 0
    пока n > 0 {{
        n = n - 1
        для каждого x в [{numbers}] {{
            если x == {skip} {{ продолжить }}
            результат = результат + x
        }}
    }}
    вернуть результат
}}
печатать(расчет({limit}))
'''


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--kirton',type=Path,required=True)
    parser.add_argument('--count',type=int,default=40)
    parser.add_argument('--seed',type=int,default=20261109)
    options=parser.parse_args()
    rng=random.Random(options.seed)
    with tempfile.TemporaryDirectory(prefix='kirton-foreach-probe-') as directory:
        root=Path(directory)
        for case in range(options.count):
            source=root/'sample.kir'
            exe=root/'native'
            source.write_text(script_for(rng,case),encoding='utf-8')
            ref=run([str(options.kirton),str(source)])
            bc=run([str(options.kirton),'--bytecode',str(source)])
            compile_result=run([str(options.kirton),'--native',str(source),'-o',str(exe)])
            native=run([str(exe)]) if compile_result[0]==0 else compile_result
            if not (ref==bc==native and ref[0]==0 and compile_result[0]==0):
                # Preserve the failed sample outside the temporary directory.
                failed=Path.cwd()/f'kirton_foreach_failed_{options.seed}_{case}.kir'
                failed.write_text(source.read_text(encoding='utf-8'),encoding='utf-8')
                print(f'FAIL seed={options.seed} case={case} file={failed}')
                print(f'AST={ref!r}\nBC={bc!r}\ncompile={compile_result!r}\nNative={native!r}')
                return 1
        print(f'PASS {options.count}/{options.count}, seed={options.seed}, AST=Bytecode=Native')
        return 0

if __name__=='__main__':
    raise SystemExit(main())
