#!/usr/bin/env python3
"""Generator patch 0020 (PPC_HOST_PTR) and RCOMP_PHYSICAL_4K_WINDOW_OFFSET; never edits output.

Builds patch0020_window.s with cpu/tools/mkxex.py, runs the patched XenonRecomp
(setjmp_address/longjmp_address name the fixture's two targets), checks that the
generated C++ forms no raw `base +` guest pointer, then compiles the unmodified
generated C++ with test_patch0020_execution.cpp in every access mode of
include/rcomp/ppc_prelude.h, with the option 0 (default) and 1, and executes it:
with 0 every access must reach host base + ea (the historic flat mapping), with 1
the 0xE0000000 window must reach base + ea + 0x1000 and mark physical page
ea + 0x1000 for the GPU caches. With --baseline-build/--baseline-source (a generator
without patch 0020), its output must still pass with the option 0 and must be
refused at compile time with the option 1.
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
MODES = {  # name: (offset option, extra definitions)
    'off_plain': (0, []),
    'on_plain': (1, []),
    'off_checked': (0, ['RCOMP_CHECKED_GUEST_ACCESS=1']),
    'on_checked': (1, ['RCOMP_CHECKED_GUEST_ACCESS=1']),
    'off_virtual': (0, ['RCOMP_VIRTUAL_GUEST_ACCESS=1']),
    'on_virtual': (1, ['RCOMP_VIRTUAL_GUEST_ACCESS=1']),
    'off_virtual_compare_checked': (0, ['RCOMP_VIRTUAL_GUEST_ACCESS=1', 'RCOMP_STORE_COMPARE=1', 'RCOMP_CHECKED_GUEST_ACCESS=1']),
    'on_virtual_compare_checked': (1, ['RCOMP_VIRTUAL_GUEST_ACCESS=1', 'RCOMP_STORE_COMPARE=1', 'RCOMP_CHECKED_GUEST_ACCESS=1']),
}
# Every emitter site of patch 0020 must appear in the fixture's output.
FORMS = {
    'vector_load': r'\(simde__m128i\*\)\(PPC_HOST_PTR\(',
    'dcbz_dcbzl': r'memset\(PPC_HOST_PTR\(',
    'lwarx': r'\*\(uint32_t\*\)PPC_HOST_PTR\(',
    'ldarx': r'\*\(uint64_t\*\)PPC_HOST_PTR\(',
    'stwcx': r'reinterpret_cast<uint32_t\*>\(PPC_HOST_PTR\(',
    'stdcx': r'reinterpret_cast<uint64_t\*>\(PPC_HOST_PTR\(',
    'setjmp': r'setjmp\(\*reinterpret_cast<jmp_buf\*>\(PPC_HOST_PTR\(',
    'longjmp': r'longjmp\(\*reinterpret_cast<jmp_buf\*>\(PPC_HOST_PTR\(',
    'vector_store': r'PPC_VSTORE128\(',
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--baseline-build', type=Path, help='generator built without patch 0020')
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

    def record(name, ok, command=(), stdout='', stderr='', actual_exit=0, fatal=True):
        steps.append(dict(name=name, exit_code=0 if ok else 1, actual_exit=actual_exit,
                          command=[str(x) for x in command], stdout=stdout, stderr=stderr))
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok and fatal:
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

    def generate(tag, generator, src):
        work = out / tag
        work.mkdir()
        run(f'{tag}_make_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py', ROOT / 'cpu/tests/patch0020_window.s',
                                '--out', work / 'fixture'])
        meta = json.loads((work / 'fixture.json').read_text())
        data = (work / 'fixture.xex').read_bytes()
        header = int.from_bytes(data[8:12], 'big')
        (work / 'image.bin').write_bytes(data[header:])
        functions = meta['functions']
        gen = work / 'generated'
        gen.mkdir()
        config = work / 'fixture.toml'
        config.write_text('[main]\nfile_path="fixture.xex"\nout_directory_path="generated"\n'
                          f'setjmp_address=0x{functions["fn_setjmp_target"]:X}\n'
                          f'longjmp_address=0x{functions["fn_longjmp_target"]:X}\n')
        p = run(f'{tag}_generate', [generator / 'XenonRecomp/XenonRecomp', config, src / 'XenonUtils/ppc_context.h'])
        log = p.stdout + p.stderr
        diagnostics = '\n'.join(line for line in log.splitlines() if not re.fullmatch(HARMLESS, line))
        record(f'{tag}_generator_diagnostics',
               not re.search(r'ERROR:|outside|Unrecognized|unimplemented|Direct call|RC bit|no switch table', diagnostics),
               stdout=diagnostics)
        decls = [f'#define execute_{name[3:]} sub_{address:X}' for name, address in functions.items() if name.startswith('fn_')]
        (work / 'patch0020_decls.h').write_text('\n'.join(decls) + '\n')
        return work, gen

    def compile_mode(tag, work, gen, src, mode, expected=0):
        offset, defines = MODES[mode]
        binary = work / f'execution_{mode}'
        command = [args.cxx, '-std=c++20', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
                   '-Wno-unused-label', '-Wno-unused-variable',
                   '-I' + str(src / 'XenonUtils'), '-I' + str(src / 'thirdparty/simde'),
                   '-I' + str(ROOT / 'include'), '-I' + str(gen), '-I' + str(work),
                   '-include', ROOT / 'include/rcomp/ppc_prelude.h',
                   f'-DRCOMP_PHYSICAL_4K_WINDOW_OFFSET={offset}', *[f'-D{d}' for d in defines],
                   ROOT / 'cpu/tests/test_patch0020_execution.cpp', *sorted(gen.glob('ppc_recomp.*.cpp')), '-o', binary]
        return binary, run(f'{tag}_compile_{mode}', command, expected=expected, timeout=600, check=expected == 0)

    work, gen = generate('patched', build, source)
    code = '\n'.join(p.read_text() for p in sorted(gen.glob('ppc_recomp.*.cpp')))
    raw = [line.strip() for line in code.splitlines() if re.search(r'\bbase \+', line)]
    record('no_raw_base_pointer', not raw, stdout='\n'.join(raw[:20]))
    counts = {form: len(re.findall(pattern, code)) for form, pattern in FORMS.items()}
    record('every_patch0020_form_emitted', all(counts.values()), stdout=json.dumps(counts))
    record('generator_marker', '#define PPC_HOST_PTR_EMITTED 1' in (gen / 'ppc_config.h').read_text())
    for mode in MODES:
        binary, _ = compile_mode('patched', work, gen, source, mode)
        p = run(f'patched_execute_{mode}', [binary, work / 'image.bin'])
        text = p.stdout
        record(f'patched_verdict_{mode}', 'patch0020/execution PASS' in text and 'FAIL' not in text, stdout=text)

    if args.baseline_build:
        bsrc, bbuild = args.baseline_source.resolve(), args.baseline_build.resolve()
        bwork, bgen = generate('baseline', bbuild, bsrc)
        bcode = '\n'.join(p.read_text() for p in sorted(bgen.glob('ppc_recomp.*.cpp')))
        record('baseline_has_raw_base_pointers', re.search(r'\bbase \+', bcode) is not None)
        for mode in ('off_plain', 'off_virtual_compare_checked'):
            binary, _ = compile_mode('baseline', bwork, bgen, bsrc, mode)
            p = run(f'baseline_execute_{mode}', [binary, bwork / 'image.bin'])
            record(f'baseline_verdict_{mode}', 'patch0020/execution PASS' in p.stdout and 'FAIL' not in p.stdout,
                   stdout=p.stdout)
        _, p = compile_mode('baseline', bwork, bgen, bsrc, 'on_virtual', expected=1)
        refused = p.returncode != 0 and 'needs code generated with XenonRecomp patch 0020' in p.stderr
        record('baseline_refused_with_offset', refused, stdout=p.stderr[-4000:], actual_exit=p.returncode)


if __name__ == '__main__':
    main()
