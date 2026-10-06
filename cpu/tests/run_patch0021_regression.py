#!/usr/bin/env python3
"""Generator patch 0021 on original synthetic XEX fixtures; never edits output.

Builds patch0021_original.s with cpu/tools/mkxex.py, runs the patched
XenonAnalyse and XenonRecomp, compiles the unmodified generated C++ with
test_patch0021_execution.cpp (runtime hooks: patch0021_hooks.h test doubles)
and executes it. patch0021_diagnostics.s checks what must stay reported (an
unbounded jump table, an undecodable word reached by the flow of a declared
function). With --baseline-build (a generator without the patch), checks that
it fails on the same fixture.
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
BLOCKING = r'ERROR:|outside|Unrecognized|unimplemented|Direct call|RC bit|no switch table|Unable to decode'
NEW_INSTRUCTIONS = ('lhzu', 'lhau', 'lfsu', 'lfdu', 'sthu', 'stfsu', 'stfdu', 'lbzux', 'lhzux', 'lhaux', 'lwzux',
                    'lwaux', 'ldux', 'lfsux', 'lfdux', 'stbux', 'sthux', 'stdux', 'stfsux', 'stfdux', 'lhbrx',
                    'bdzf', 'bdzt', 'vaddsws', 'vsubuwm', 'vsububm', 'vadduhs', 'vsubuws', 'vcmpgtuw', 'vrlw',
                    'vrlw128', 'lvxl', 'stvxl', 'lvxl128', 'stvlxl128', 'mfctr')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--baseline-build', type=Path, help='generator built without patch 0021 (negative evidence)')
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
        ok = p.returncode == expected if expected is not None else True
        record(name, ok, command, p.stdout, p.stderr, p.returncode)
        return p.stdout + p.stderr

    def generate(tag, fixture, generator, tables_from=None, recomp_exit=0):
        work = out / tag
        work.mkdir()
        run(f'{tag}_make_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py', ROOT / 'cpu/tests' / fixture,
                                '--out', work / 'fixture'])
        meta = json.loads((work / 'fixture.json').read_text())
        data = (work / 'fixture.xex').read_bytes()
        header = int.from_bytes(data[8:12], 'big')
        (work / 'image.bin').write_bytes(data[header:])
        tables = work / 'switch_tables.toml'
        analyse = run(f'{tag}_analyse', [generator / 'XenonAnalyse/XenonAnalyse', work / 'fixture.xex', tables])
        if tables_from:
            tables.write_text(tables_from.read_text())
        gen = work / 'generated'
        gen.mkdir()
        config = work / 'fixture.toml'
        config.write_text('[main]\nfile_path="fixture.xex"\nout_directory_path="generated"\n'
                          'switch_table_file_path="switch_tables.toml"\n')
        log = run(f'{tag}_generate', [generator / 'XenonRecomp/XenonRecomp', config,
                                      source / 'XenonUtils/ppc_context.h'], expected=recomp_exit)
        diagnostics = '\n'.join(line for line in log.splitlines() if not re.fullmatch(HARMLESS, line))
        return work, meta, tables.read_text(), gen, diagnostics, analyse

    work, meta, tables, gen, diagnostics, analyse = generate('original', 'patch0021_original.s', build)
    functions = meta['functions']
    bases = [int(b, 16) for b in re.findall(r'^base = (0x[0-9A-F]+)', tables, re.M)]
    ranges = sorted(functions.values())

    def owner(address):
        return max((name for name, a in functions.items() if a <= address), key=lambda n: functions[n])
    owners = sorted(owner(b) for b in bases)
    expected_owners = sorted(['fn_sw_sched', 'fn_sw_computed', 'fn_sw_byte', 'fn_sw_short', 'fn_sw_bge', 'fn_sw_reuse',
                              'fn_sw_copy', 'fn_sw_skip', 'fn_sw_inline_gap', 'fn_sw_other_compare', 'fn_sw_unreachable'])
    record('eleven_switch_tables_none_for_the_dispatch', owners == expected_owners, stdout=json.dumps(owners))
    record('one_ctr_target_table', tables.count('target = "ctr"') == 1, stdout=tables)
    record('analyser_reports_nothing', 'WARNING' not in analyse, stdout=analyse)
    record('generator_diagnostics', not re.search(BLOCKING, diagnostics), stdout=diagnostics)
    sources = '\n'.join(p.read_text() for p in sorted(gen.glob('ppc_recomp.*.cpp')))
    zero_word = functions['fn_returns_two_again'] - 4
    record('zero_word_case_traps', f'PPC_TRAP(0x{zero_word:X});' in sources, stdout=f'0x{zero_word:X}')
    decls = [f'#define execute_{name[3:]} sub_{address:X}' for name, address in functions.items() if name.startswith('fn_')]
    decls.append(f'constexpr uint32_t kZeroWord = 0x{zero_word:X};')
    (work / 'patch0021_decls.h').write_text('#include <cstdint>\n' + '\n'.join(decls) + '\n')
    binary = work / 'patch0021_execution'
    run('compile_execution', [args.cxx, '-std=c++17', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
        '-I' + str(source / 'XenonUtils'), '-I' + str(source / 'thirdparty/simde'),
        '-I' + str(ROOT / 'include'), '-I' + str(gen), '-I' + str(work),
        '-include', ROOT / 'include/rcomp/ppc_prelude.h', '-include', ROOT / 'cpu/tests/patch0021_hooks.h',
        ROOT / 'cpu/tests/test_patch0021_execution.cpp', *sorted(gen.glob('ppc_*.cpp')), '-o', binary], timeout=300)
    execution = run('execute', [binary, work / 'image.bin'])
    record('execution_verdict', 'patch0021/execution PASS' in execution and ' FAIL' not in execution, stdout=execution)

    dwork, dmeta, dtables, dgen, ddiagnostics, danalyse = generate('diagnostics', 'patch0021_diagnostics.s', build)
    dfun = dmeta['functions']
    unbounded_bctr = dfun['fn_unbounded'] + 20
    undecodable = dfun['fn_undecodable'] + 4
    dsources = '\n'.join(p.read_text() for p in sorted(dgen.glob('ppc_recomp.*.cpp')))
    record('unbounded_table_reported', f'WARNING: unresolved jump table at {unbounded_bctr:X}' in danalyse
           and '[[switch]]' not in dtables, stdout=danalyse)
    record('unbounded_table_stays_indirect_call', 'PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);' in dsources, stdout=dsources)
    # The diagnostic prints the word as the host read it (upstream format).
    record('undecodable_word_reported', re.search(rf'^Unable to decode instruction [0-9A-F]+ at {undecodable:X}$',
                                                  ddiagnostics, re.M) is not None, stdout=ddiagnostics)
    record('undecodable_word_traps', f'PPC_TRAP(0x{undecodable:X});' in dsources, stdout=dsources)

    if args.baseline_build:
        base = args.baseline_build.resolve()
        _, _, base_tables, _, base_diagnostics, _ = generate('baseline', 'patch0021_original.s', base, recomp_exit=None)
        record('baseline_misses_scheduled_tables', base_tables.count('[[switch]]') < 11, stdout=base_tables)
        record('baseline_rejects_other_register_guard', 'ERROR: invalid switch table' in base_diagnostics,
               stdout=base_diagnostics)
        _, _, _, _, base_diagnostics, _ = generate('baseline_tables', 'patch0021_original.s', base,
                                                   tables_from=work / 'switch_tables.toml', recomp_exit=None)
        missing = [name for name in NEW_INSTRUCTIONS
                   if not re.search(rf'Unrecognized instruction at 0x[0-9A-F]+: {re.escape(name)}\.?$', base_diagnostics, re.M)]
        record('baseline_reports_the_instructions', not missing, stdout=json.dumps(missing) + '\n' + base_diagnostics)
        record('baseline_reports_data_as_code', 'Unable to decode' in base_diagnostics, stdout=base_diagnostics)


if __name__ == '__main__':
    main()
