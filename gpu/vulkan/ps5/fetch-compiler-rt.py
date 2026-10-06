#!/usr/bin/env python3
"""Pin a sparse official compiler-rt checkout for the isolated PS5 RADV build."""
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/vulkan-gta-radv-20260928'
SOURCE=OUT/'compiler-rt-reference'
PIN='ca7933e47d3a3451d81e72ac174dcb5aa28b59d1'
URL='https://github.com/llvm/llvm-project.git'

def command(args):
    subprocess.run(args,cwd=ROOT,check=True,timeout=600)

def main():
    if not SOURCE.exists():
        command(['git','-c','core.autocrlf=false','-c','core.longpaths=true','clone',
                 '--filter=blob:none','--depth=1','--no-checkout','--branch','llvmorg-22.1.8',URL,str(SOURCE)])
        command(['git','-C',str(SOURCE),'sparse-checkout','init','--cone'])
        command(['git','-C',str(SOURCE),'sparse-checkout','set','compiler-rt','cmake','llvm/cmake','llvm/include/llvm/Config'])
        command(['git','-C',str(SOURCE),'checkout','--detach',PIN])
    actual=subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()
    if actual!=PIN: raise RuntimeError('compiler-rt reference pin mismatch')
    if subprocess.check_output(['git','-C',str(SOURCE),'status','--porcelain'],text=True).strip():
        raise RuntimeError('compiler-rt reference contains changes')
    if not (SOURCE/'compiler-rt/lib/builtins/emutls.c').is_file():
        raise RuntimeError('incomplete sparse compiler-rt checkout')
    (OUT/'compiler-rt-reference.json').write_text(json.dumps(
        {'url':URL,'tag':'llvmorg-22.1.8','commit':PIN,'license':'Apache-2.0 WITH LLVM-exception'},indent=2)+'\n')
    print('PASS compiler-rt pinned sparse source',PIN)

if __name__=='__main__':main()
