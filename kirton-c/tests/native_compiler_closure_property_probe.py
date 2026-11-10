#!/usr/bin/env python3
import os, random, subprocess, sys
from pathlib import Path
import argparse, tempfile, shutil
parser=argparse.ArgumentParser(description="Kirton AST/Bytecode/Native closure differential probe")
parser.add_argument('--kirton',required=True,help='Path to the compiled kirton CLI')
parser.add_argument('--count',type=int,default=150)
parser.add_argument('--seed',type=int,default=927506)
parser.add_argument('--workdir',default=None)
opts=parser.parse_args()
N=opts.count
if N<1:parser.error('--count must be positive')
kirton=Path(opts.kirton).resolve()
if not kirton.is_file():parser.error('kirton executable not found')
temporary=not opts.workdir
work=Path(tempfile.mkdtemp(prefix='kirton-closure-probe-')) if temporary else Path(opts.workdir).resolve()
work.mkdir(exist_ok=True,parents=True)
r=random.Random(opts.seed)
variants=[]
for n in range(N):
 a=r.randint(-1000000,1000000); b=r.randint(1,250); d=r.randint(0,700)
 t=n%5
 if t==0:
  code=f'''функция make(x) {{\n  пусть y = {a}\n  функция f(z) {{ y = y + z\n    вернуть y + x }}\n  вернуть f\n}}\nпусть f=make({b})\nпечатать(f({d}))\nпечатать(f({d+1}))\n'''
 elif t==1:
  code=f'''функция make(x) {{\n  функция first(d) {{ x = x + d\n    вернуть x }}\n  функция second() {{ вернуть x }}\n  вернуть [first,second]\n}}\nпусть fs=make({a})\nпечатать(fs[1]())\nпечатать(fs[0]({d}))\nпечатать(fs[1]())\n'''
 elif t==2:
  code=f'''функция outer(x) {{\n  пусть y={b}\n  функция middle(z) {{\n    функция inner(w) {{ вернуть x+y+z+w }}\n    вернуть inner\n  }}\n  вернуть middle\n}}\nпусть f=outer({a})\nпусть g=f({d})\nпечатать(g(5))\n'''
 elif t==3:
  code=f'''функция make(word) {{\n  пусть seq=[word, {b}]\n  функция f(i) {{\n    если i == 0 {{ вернуть seq[0] }}\n    seq[1] = seq[1] + i\n    вернуть seq[1]\n  }}\n  вернуть f\n}}\nпусть fn=make("Киртон_тест_{n}")\nпечатать(fn(0))\nпечатать(fn({d+1}))\nпечатать(fn({d+2}))\n'''
 else:
  code=f'''функция make(x) {{\n  пусть list=[]\n  для каждого x в [{b}, {d}, {a}] {{\n    функция see() {{ вернуть x }}\n    список_добавить(list,see)\n  }}\n  вернуть list\n}}\nпусть fns=make(100)\nпечатать(fns[0]())\nпечатать(fns[1]())\nпечатать(fns[2]())\n'''
 variants.append(code)

def cmd(args,timeout=12):
 try:
  p=subprocess.run(args,capture_output=True,timeout=timeout,cwd=str(work))
  return p.returncode,p.stdout,p.stderr
 except subprocess.TimeoutExpired:return -99,b'',b'timeout'
for n,code in enumerate(variants):
 path=work/f'p_{n}.kir';path.write_text(code,encoding='utf8')
 ast=cmd([str(kirton),str(path)])
 bc=cmd([str(kirton),'--bytecode',str(path)])
 exe=work/f'p_{n}'
 build=cmd([str(kirton),'--native',str(path),'-o',str(exe)],30)
 native=cmd([str(exe)]) if build[0]==0 else build
 if any(v!=ast for v in [bc,native]) or ast[0]!=0:
  print('MISMATCH',n,'type',n%5)
  print('AST:',ast,'BC:',bc,'BUILD:',build,'NATIVE:',native)
  print(code);print('Reproducer saved to:',path);sys.exit(1)
 if n%25==0:print('passed',n+1,flush=True)
print('ALL_PASS',N,'seed',opts.seed)
if temporary:shutil.rmtree(work)
