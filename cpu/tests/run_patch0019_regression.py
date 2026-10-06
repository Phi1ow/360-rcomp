#!/usr/bin/env python3
"""Generator patch 0019 on original synthetic XEX fixtures; never edits output.

Builds patch0019_original.s with cpu/tools/mkxex.py, runs the patched
XenonAnalyse and XenonRecomp, compiles the unmodified generated C++ with
test_patch0019_execution.cpp and executes it. Also checks that a tail which
is not closed (patch0019_open_tail.s) keeps its blocking diagnostic and,
with --baseline-build, that the generator before the patch fails both ways.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HARMLESS = (r'^ERROR: __(?:restgprlr_14|savegprlr_14|restfpr_14|savefpr_14|restvmx_14|savevmx_14|restvmx_64|savevmx_64)'
            r' address is unspecified$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--baseline-build', type=Path, help='generator built without patch 0019 (negative evidence)')
    ap.add_argument('--cxx', default='clang++')
    args = ap.parse_args()
    source, build, out = (p.resolve() for p in (args.source, args.build, args.out))
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be fresh and under build/')
    out.mkdir(parents=True)
    steps = []

    def record(name, ok, command=(), stdout='', stderr='', actual_exit=0):
        steps.append(dict(name=name, exit_code=0 if ok else 1, actual_exit=actual_exit,
                          command=[str(x) for x in command], stdout=stdout, stderr=stderr))
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            raise SystemExit(1)

    def run(name, command, expected=0, timeout=60):
        command = [str(x) for x in command]
        try:
            p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            record(name, False, command, stderr='timeout', actual_exit=124)
        record(name, p.returncode == expected, command, p.stdout, p.stderr, p.returncode)
        return p.stdout + p.stderr

    def generate(tag, fixture, generator):
        work = out / tag
        work.mkdir()
        run(f'{tag}_make_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py', ROOT / 'cpu/tests' / fixture,
                                '--out', work / 'fixture'])
        meta = json.loads((work / 'fixture.json').read_text())
        data = (work / 'fixture.xex').read_bytes()
        header = int.from_bytes(data[8:12], 'big')
        (work / 'image.bin').write_bytes(data[header:])
        tables = work / 'switch_tables.toml'
        run(f'{tag}_analyse', [generator / 'XenonAnalyse/XenonAnalyse', work / 'fixture.xex', tables])
        gen = work / 'generated'
        gen.mkdir()
        config = work / 'fixture.toml'
        config.write_text('[main]\nfile_path="fixture.xex"\nout_directory_path="generated"\n'
                          'switch_table_file_path="switch_tables.toml"\n')
        log = run(f'{tag}_generate', [generator / 'XenonRecomp/XenonRecomp', config,
                                      source / 'XenonUtils/ppc_context.h'])
        diagnostics = '\n'.join(line for line in log.splitlines() if not re.fullmatch(HARMLESS, line))
        return work, meta, tables.read_text(), gen, diagnostics

    work, meta, tables, gen, diagnostics = generate('original', 'patch0019_original.s', build)
    record('one_switch_table_found', tables.count('[[switch]]') == 1, stdout=tables)
    record('generator_diagnostics', not re.search(r'ERROR:|outside|Unrecognized|unimplemented|Direct call|RC bit|no switch table',
                                                  diagnostics), stdout=diagnostics)
    functions = meta['functions']
    tails = {'owner_tail': functions['fn_owner'] + 4, 'chain_tail': functions['fn_chain_owner'] + 4}
    shared = (gen / 'ppc_recomp_shared.h').read_text()
    declared = set(re.findall(r'PPC_EXTERN_FUNC\((\w+)\)', shared))
    expected = {f'sub_{a:X}' for name, a in functions.items() if name != '_start'} | \
               {f'sub_{a:X}' for a in tails.values()} | {'_xstart'}
    record('tail_entries_are_functions', expected <= declared and len(declared) == len(expected),
           stdout=json.dumps({'declared': sorted(declared), 'expected': sorted(expected)}))
    decls = [f'#define execute_{name[3:]} sub_{address:X}' for name, address in functions.items() if name.startswith('fn_')]
    (work / 'patch0019_decls.h').write_text('\n'.join(decls) + '\n')
    binary = work / 'patch0019_execution'
    run('compile_execution', [args.cxx, '-std=c++17', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
        '-I' + str(source / 'XenonUtils'), '-I' + str(source / 'thirdparty/simde'),
        '-I' + str(ROOT / 'include'), '-I' + str(gen), '-I' + str(work),
        '-include', ROOT / 'include/rcomp/ppc_prelude.h',
        ROOT / 'cpu/tests/test_patch0019_execution.cpp', *sorted(gen.glob('ppc_recomp.*.cpp')), '-o', binary], timeout=300)
    execution = run('execute', [binary, work / 'image.bin'])
    record('execution_verdict', execution.count(' PASS') == 58 and 'FAIL' not in execution, stdout=execution)

    _, _, _, _, open_diagnostics = generate('open_tail', 'patch0019_open_tail.s', build)
    record('open_tail_keeps_its_diagnostic', 'Direct call' in open_diagnostics, stdout=open_diagnostics)

    if args.baseline_build:
        base = args.baseline_build.resolve()
        _, _, base_tables, _, base_diagnostics = generate('baseline', 'patch0019_original.s', base)
        for needle in ('vnor128', 'vcfpuxws128', 'bdnzt', 'Direct call'):
            record(f'baseline_reports_{needle.replace(" ", "_")}', needle in base_diagnostics, stdout=base_diagnostics)
        record('baseline_misses_hoisted_table', base_tables.count('[[switch]]') == 0, stdout=base_tables)


if __name__ == '__main__':
    main()
