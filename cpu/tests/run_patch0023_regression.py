#!/usr/bin/env python3
"""Generator patch 0023 (non-trapping integer division); never edits output.

1. Host test of the helpers of include/rcomp/ppc_prelude.h (test_ppc_division.cpp), also with
   -fsanitize=undefined -fsanitize-trap=undefined (undefined behaviour in a helper traps).
2. Builds patch0023_division.s (divw divwu divd divdu, divw. divwu. divdu., rD == rB, the Halo 3
   shape) and patch0023_overflow.s (the OE forms divwo divwuo divdo divduo and record + OE) with
   cpu/tools/mkxex.py, runs the patched XenonRecomp, checks that no generator diagnostic appears,
   that every division goes through PPC_DIVW / PPC_DIVWU / PPC_DIVD / PPC_DIVDU (one helper per
   guest division, no raw C++ integer division left) and that an OE form computes XER[OV] before
   its quotient. Compiles the unmodified generated C++ with test_patch0023_execution.cpp with the
   prelude and with the ppc_context.h defaults alone, each also with the UB trap, runs every case
   and a division by zero (the process must survive).
3. patch0023_diagnostics.s: divd. and divdo. keep no CR0 update and stay reported ("RC bit").
With --baseline-build/--baseline-source (a generator without patch 0023, e.g. v22): its output for
patch0023_division.s differs only in the quotient lines (a / b -> PPC_DIV*(a, b)), its division by
zero does not survive, it reports the OE forms as unrecognized and divd. as today.
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
HELPER = {'divw': 'DIVW', 'divwu': 'DIVWU', 'divd': 'DIVD', 'divdu': 'DIVDU'}
TYPE = {'DIVW': 's32', 'DIVWU': 'u32', 'DIVD': 's64', 'DIVDU': 'u64'}
RAW_INT_DIV = re.compile(r'\.(?:s32|u32|s64|u64) / ')
REG = r'(?:\w+\.)?\w+'  # ctx.r3, or r3 when registers are local variables
QUOTIENT_OLD = re.compile(rf'^\t({REG})\.(s32|u32|s64|u64) = ({REG})\.\2 / ({REG})\.\2;$')
QUOTIENT_NEW = re.compile(rf'^\t({REG})\.(s32|u32|s64|u64) = PPC_(DIVWU|DIVW|DIVDU|DIVD)\(({REG})\.\2, ({REG})\.\2\);$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--baseline-build', type=Path, help='generator built without patch 0023')
    ap.add_argument('--baseline-source', type=Path, help='its source tree (ppc_context.h)')
    ap.add_argument('--cxx', default='clang++')
    args = ap.parse_args()
    source, build, out = (p.resolve() for p in (args.source, args.build, args.out))
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be fresh and under build/')
    if bool(args.baseline_build) != bool(args.baseline_source):
        ap.error('--baseline-build and --baseline-source go together')
    out.mkdir(parents=True)
    steps = []

    def record(name, ok, command=(), stdout='', stderr='', actual_exit=0):
        steps.append(dict(name=name, exit_code=0 if ok else 1, actual_exit=actual_exit,
                          command=[str(x) for x in command], stdout=stdout, stderr=stderr))
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            raise SystemExit(1)

    def run(name, command, expected=0, timeout=120, check=True):
        command = [str(x) for x in command]
        try:
            p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            record(name, False, command, stderr='timeout', actual_exit=124)
        if check:
            record(name, p.returncode == expected, command, p.stdout, p.stderr, p.returncode)
        return p

    # 1. The helpers themselves.
    for tag, extra in (('O2', []), ('O2_ubsan_trap', ['-fsanitize=undefined', '-fsanitize-trap=undefined'])):
        binary = out / f'test_ppc_division_{tag}'
        run(f'helpers_compile_{tag}', [args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                                       '-I' + str(ROOT / 'include'), *extra, ROOT / 'cpu/tests/test_ppc_division.cpp',
                                       '-o', binary], timeout=300)
        p = run(f'helpers_run_{tag}', [binary])
        record(f'helpers_verdict_{tag}', 'ppc_division PASS' in p.stdout, stdout=p.stdout)

    def generate(tag, fixture, generator, src):
        work = out / tag
        work.mkdir()
        run(f'{tag}_make_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py', ROOT / f'cpu/tests/{fixture}',
                                '--out', work / 'fixture'])
        meta = json.loads((work / 'fixture.json').read_text())
        gen = work / 'generated'
        gen.mkdir()
        config = work / 'fixture.toml'
        config.write_text('[main]\nfile_path="fixture.xex"\nout_directory_path="generated"\n')
        p = run(f'{tag}_generate', [generator / 'XenonRecomp/XenonRecomp', config, src / 'XenonUtils/ppc_context.h'],
                check=False)
        log = p.stdout + p.stderr
        diagnostics = '\n'.join(line for line in log.splitlines() if not re.fullmatch(HARMLESS, line))
        decls = [f'#define execute_{name[3:]} sub_{address:X}' for name, address in meta['functions'].items()
                 if name.startswith('fn_')]
        (work / 'patch0023_decls.h').write_text('\n'.join(decls) + '\n')
        code = '\n'.join(q.read_text() for q in sorted(gen.glob('ppc_recomp.*.cpp')))
        return work, gen, p.returncode, diagnostics, code, meta

    def compile_and_run(tag, work, gen, src, prelude, ubsan, probe_only=False):
        arm = ('prelude' if prelude else 'context_defaults') + ('_ubsan_trap' if ubsan else '')
        binary = work / f'execution_{arm}'
        command = [args.cxx, '-std=c++20', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
                   '-Wno-unused-label', '-Wno-unused-variable',
                   '-I' + str(src / 'XenonUtils'), '-I' + str(src / 'thirdparty/simde'),
                   '-I' + str(ROOT / 'include'), '-I' + str(gen), '-I' + str(work),
                   f'-DPATCH0023_ARM_NAME="{arm}"']
        if prelude:
            command += ['-include', ROOT / 'include/rcomp/ppc_prelude.h']
        if ubsan:
            command += ['-fsanitize=undefined', '-fsanitize-trap=undefined']
        command += [ROOT / 'cpu/tests/test_patch0023_execution.cpp', *sorted(gen.glob('ppc_recomp.*.cpp')), '-o', binary]
        run(f'{tag}_compile_{arm}', command, timeout=600)
        probe = run(f'{tag}_zero_divisor_probe_{arm}', [binary, 'zero_divisor_probe'], check=False)
        if probe_only:
            return probe
        record(f'{tag}_zero_divisor_survives_{arm}',
               probe.returncode == 0 and 'patch0023/zero_divisor_probe survived r3=0x0' in probe.stdout,
               stdout=probe.stdout, stderr=probe.stderr, actual_exit=probe.returncode)
        p = run(f'{tag}_execute_{arm}', [binary, 'all'])
        record(f'{tag}_verdict_{arm}', 'patch0023/execution PASS' in p.stdout and 'FAIL' not in p.stdout,
               stdout=p.stdout[-4000:])

    def helper_checks(tag, code):
        comments = {m: len(re.findall(r'^\t// ' + m + r'o?\.? ', code, re.M)) for m in HELPER}
        helpers = {h: len(re.findall(r'= PPC_' + h + r'\(', code)) for h in HELPER.values()}
        record(f'{tag}_every_form_emitted', all(comments.values()), stdout=json.dumps(comments))
        record(f'{tag}_one_helper_per_division', all(helpers[HELPER[m]] == comments[m] for m in HELPER),
               stdout=json.dumps({'comments': comments, 'helpers': helpers}))
        raw = [line.strip() for line in code.splitlines() if RAW_INT_DIV.search(line) and '//' not in line]
        record(f'{tag}_no_raw_integer_division', not raw, stdout='\n'.join(raw[:20]))

    # 2. Plain and record forms, then the OE forms.
    work, gen, rc, diag, code, _ = generate('division', 'patch0023_division.s', build, source)
    record('division_generator_exit', rc == 0, actual_exit=rc, stdout=diag)
    record('division_generator_diagnostics', not re.search(BLOCKING, diag), stdout=diag)
    helper_checks('division', code)
    for prelude in (True, False):
        for ubsan in (False, True):
            compile_and_run('division', work, gen, source, prelude, ubsan)

    owork, ogen, rc, odiag, ocode, _ = generate('overflow', 'patch0023_overflow.s', build, source)
    record('overflow_generator_exit', rc == 0, actual_exit=rc, stdout=odiag)
    record('overflow_generator_diagnostics', not re.search(BLOCKING, odiag), stdout=odiag)
    helper_checks('overflow', ocode)
    # XER[OV] from the source operands, then SO, then the quotient (rD may be rA or rB).
    order = re.findall(rf'^\t((?:\w+\.)?xer)\.ov = PPC_(DIVWU|DIVW|DIVDU|DIVD)_OVERFLOW\(({REG})\.(\w+), ({REG})\.\4\);\n'
                       rf'\t\1\.so \|= \1\.ov;\n'
                       rf'\t{REG}\.\4 = PPC_\2\(\3\.\4, \5\.\4\);$', ocode, re.M)
    oe_comments = len(re.findall(r'^\t// div(?:w|wu|d|du)o\.? ', ocode, re.M))
    record('overflow_ov_before_quotient', oe_comments > 0 and len(order) == oe_comments,
           stdout=json.dumps({'oe_instructions': oe_comments, 'ordered_sequences': len(order)}))
    for prelude in (True, False):
        for ubsan in (False, True):
            compile_and_run('overflow', owork, ogen, source, prelude, ubsan)

    # 3. What stays reported.
    _, dgen, rc, ddiag, dcode, dmeta = generate('diagnostics', 'patch0023_diagnostics.s', build, source)
    f = dmeta['functions']
    record('diagnostics_divd_rc_reported',
           f'divd. at {f["fn_divd_rc"]:X} has RC bit enabled but no comparison was generated' in ddiag
           and f'divdo. at {f["fn_divdo_rc"]:X} has RC bit enabled but no comparison was generated' in ddiag,
           stdout=ddiag)
    record('diagnostics_divd_rc_still_through_helper', len(re.findall(r'= PPC_DIVD\(', dcode)) == 2
           and len(re.findall(r'\.ov = PPC_DIVD_OVERFLOW\(', dcode)) == 1, stdout=dcode[-3000:])

    if args.baseline_build:
        bsrc, bbuild = args.baseline_source.resolve(), args.baseline_build.resolve()
        bwork, bgen, rc, bdiag, bcode, _ = generate('baseline_division', 'patch0023_division.s', bbuild, bsrc)
        record('baseline_division_generator_clean', rc == 0 and not re.search(BLOCKING, bdiag), stdout=bdiag)
        new_files = sorted(p.name for p in gen.glob('ppc_recomp.*.cpp'))
        record('baseline_same_files', new_files == sorted(p.name for p in bgen.glob('ppc_recomp.*.cpp')),
               stdout=json.dumps(new_files))
        changed, unexpected = 0, []
        for name in new_files:
            old, new = (bgen / name).read_text().split('\n'), (gen / name).read_text().split('\n')
            if len(old) != len(new):
                unexpected.append(f'{name}: {len(old)} -> {len(new)} lines')
                continue
            for i, (x, y) in enumerate(zip(old, new)):
                if x == y:
                    continue
                mo, mn = QUOTIENT_OLD.match(x), QUOTIENT_NEW.match(y)
                if (mo and mn and mo.group(1, 2, 3, 4) == mn.group(1, 2, 4, 5) and TYPE[mn.group(3)] == mo.group(2)):
                    changed += 1
                else:
                    unexpected.append(f'{name}:{i + 1}: {x.strip()!r} -> {y.strip()!r}')
        divisions = len(re.findall(r'^\t// div(?:w|wu|d|du)\.? ', code, re.M))
        record('baseline_only_quotient_lines_differ', not unexpected and changed == divisions,
               stdout=json.dumps({'changed': changed, 'divisions': divisions, 'unexpected': unexpected[:20]}))
        probe = compile_and_run('baseline_division', bwork, bgen, bsrc, True, False, probe_only=True)
        record('baseline_zero_divisor_does_not_survive', probe.returncode != 0 and 'survived' not in probe.stdout,
               stdout=probe.stdout, stderr=probe.stderr[-2000:], actual_exit=probe.returncode)
        _, _, _, bodiag, _, bometa = generate('baseline_overflow', 'patch0023_overflow.s', bbuild, bsrc)
        record('baseline_oe_forms_unrecognized',
               all(f'Unrecognized instruction at 0x{bometa["functions"][fn]:X}: {name}' in bodiag
                   for fn, name in (('fn_divwo', 'divwo'), ('fn_divwuo', 'divwuo'), ('fn_divdo', 'divdo'),
                                    ('fn_divduo', 'divduo'))), stdout=bodiag)
        _, _, _, bddiag, _, bdmeta = generate('baseline_diagnostics', 'patch0023_diagnostics.s', bbuild, bsrc)
        record('baseline_divd_rc_reported_as_today',
               f'divd. at {bdmeta["functions"]["fn_divd_rc"]:X} has RC bit enabled but no comparison was generated'
               in bddiag, stdout=bddiag)


if __name__ == '__main__':
    main()
