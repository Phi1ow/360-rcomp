#!/usr/bin/env python3
"""Bounded discovery + actual PPC tail-call execution, using original fixtures.

Run on the host after preparing/building XenonRecomp. All generated artifacts
stay in a fresh directory under build/; generated C++ is never modified.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--prefix', default='powerpc-linux-gnu-')
    ap.add_argument('--cxx', default='clang++')
    args = ap.parse_args()
    source, build, out = (p.resolve() for p in (args.source, args.build, args.out))
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be a fresh directory under build/')
    out.mkdir(parents=True)
    steps = []

    def run(name, command, timeout=30):
        command = [str(x) for x in command]
        try:
            p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
            step = dict(name=name, command=command, exit_code=p.returncode,
                        stdout=p.stdout, stderr=p.stderr)
        except subprocess.TimeoutExpired:
            step = dict(name=name, command=command, exit_code=124,
                        stdout='', stderr=f'timeout after {timeout}s')
        steps.append(step)
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if step["exit_code"] == 0 else "FAIL"} rc={step["exit_code"]}', flush=True)
        if step['exit_code']:
            raise SystemExit(1)
        return step['stdout'] + step['stderr']

    def expect(name, ok, detail):
        step = dict(name=name, command=[], exit_code=0 if ok else 1,
                    stdout=detail, stderr='')
        steps.append(step)
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            raise SystemExit(1)

    unit = out / 'analyse_bounds'
    run('compile_bounds', [args.cxx, '-std=c++17', '-O2', '-DNDEBUG',
        '-I' + str(source / 'XenonAnalyse'), '-I' + str(source / 'XenonUtils'),
        '-I' + str(source / 'thirdparty/disasm'), ROOT / 'cpu/tests/test_analyse_bounds.cpp',
        source / 'XenonAnalyse/function.cpp', build / 'XenonUtils/libXenonUtils.a',
        build / 'thirdparty/disasm/libdisasm.a', '-o', unit])
    unit_log = run('bounds', [unit], timeout=5)
    expect('bounds_verdict', unit_log.count(' PASS ') == 10 and 'FAIL' not in unit_log,
           'Expected ten bounded discovery cases')

    bin_dir, gen_dir = out / 'bin', out / 'gen'
    bin_dir.mkdir()
    gen_dir.mkdir()
    obj = bin_dir / 'analyse_tail_branch.o'
    run('assemble', [args.prefix + 'as', '-a32', '-be', '-mregnames', '-mpower7',
                     '-o', obj, ROOT / 'cpu/tests/analyse_tail_branch.s'])
    dis = run('disassemble', [args.prefix + 'objdump', '-d', '-EB', obj])
    relocs = run('relocations', [args.prefix + 'objdump', '-r', obj])
    expect('relocation_verdict', not re.search(r'^[0-9a-f]{8} ', relocs, re.MULTILINE),
           'The original fixture must not contain unresolved relocations')
    (bin_dir / 'analyse_tail_branch.dis').write_text(dis)
    generator = build / 'XenonRecomp/XenonRecomp'
    log = run('recompile', [generator, bin_dir, gen_dir], timeout=5)
    expect('generator_verdict', not re.search(r'unimplemented|Unrecognized|Direct call|ERROR:', log),
           'The generator must report no unsupported instruction or missing call')
    generated = (gen_dir / 'analyse_tail_branch.cpp').read_text()
    expect('tail_call_verdict', 'analyse_tail_branch_4(ctx, base);' in generated and 'longjmp(' not in generated,
           'Expected the ordinary conditional tail call')
    # Preserve the upstream test driver as evidence, compile our independent
    # assertions alongside the completely unmodified generated translation unit.
    (gen_dir / 'main.cpp').rename(out / 'upstream_main.cpp.txt')
    execution = out / 'tail_execution'
    run('compile_execution', [args.cxx, '-std=c++17', '-O2', '-march=x86-64-v3',
        '-ffp-contract=on', '-I' + str(source / 'XenonUtils'),
        '-I' + str(source / 'thirdparty/simde'), '-I' + str(ROOT / 'include'),
        '-include', ROOT / 'include/rcomp/ppc_prelude.h',
        ROOT / 'cpu/tests/test_analyse_tail_execution.cpp',
        gen_dir / 'analyse_tail_branch.cpp', '-o', execution])
    executed = run('tail_execution', [execution], timeout=5)
    expect('execution_verdict', 'analyse/tail_execution PASS taken=77 fallthrough=11' in executed,
           'Both the taken and fallthrough outcomes must be reported')


if __name__ == '__main__':
    main()
