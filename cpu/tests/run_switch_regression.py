#!/usr/bin/env python3
"""Original XEX switch discovery/execution and invalid-config rejection."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--cxx', default='clang++')
    args = ap.parse_args()
    source, build, out = (p.resolve() for p in (args.source, args.build, args.out))
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be fresh and under build/')
    out.mkdir(parents=True)
    steps = []

    def record(name, ok, command, stdout='', stderr='', actual_exit=0):
        steps.append(dict(name=name, exit_code=0 if ok else 1, actual_exit=actual_exit,
                          command=[str(x) for x in command], stdout=stdout, stderr=stderr))
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(f'{name}: {"PASS" if ok else "FAIL"}', flush=True)
        if not ok:
            raise SystemExit(1)

    def run(name, command, expected=0, timeout=30):
        command = [str(x) for x in command]
        try:
            p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
            record(name, p.returncode == expected, command, p.stdout, p.stderr, p.returncode)
            return p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            record(name, False, command, stderr='timeout', actual_exit=124)

    run('make_original_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py',
                             ROOT / 'cpu/tests/switch_original.s', '--out', out / 'original'])
    meta = json.loads((out / 'original.json').read_text())
    data = bytearray((out / 'original.xex').read_bytes())
    header = struct.unpack_from('>I', data, 8)[0]
    pe = header + struct.unpack_from('<I', data, header + 0x3c)[0]
    nsections = struct.unpack_from('<H', data, pe + 6)[0]
    sections = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
    omit = {meta['functions'][name] for name in ('fn_switch', 'fn_duplicate')}
    kept = []
    for i in range(nsections):
        section = sections + 40 * i
        if data[section:section + 8].rstrip(b'\0') != b'.pdata':
            continue
        size, va = struct.unpack_from('<II', data, section + 8)
        pos = header + va
        kept = [bytes(data[z:z + 8]) for z in range(pos, pos + size, 8)
                if struct.unpack_from('>I', data, z)[0] not in omit]
        data[pos:pos + size] = b''.join(kept) + b'\0' * (size - 8 * len(kept))
        struct.pack_into('<I', data, section + 8, 8 * len(kept))
        struct.pack_into('<I', data, section + 16, 8 * len(kept))
    record('remove_only_dispatcher_pdata', len(kept) == 2, [],
           'Retain original entry and boundary metadata; dispatcher discovery must use code edges')
    xex = out / 'heuristic.xex'
    xex.write_bytes(data)
    (out / 'image.bin').write_bytes(data[header:])
    tables = out / 'switch_tables.toml'
    run('analyse_switches', [build / 'XenonAnalyse/XenonAnalyse', xex, tables])
    switches = tables.read_text()
    record('two_detected_switches', switches.count('[[switch]]') == 2, [], switches)
    config_template = '[main]\nfile_path="heuristic.xex"\nout_directory_path="{gen}"\nswitch_table_file_path="{tables}"\n'
    config = out / 'fixture.toml'
    gen = out / 'generated'
    gen.mkdir()
    config.write_text(config_template.format(gen=gen.name, tables=tables.name))
    log = run('generate_heuristic_switches', [build / 'XenonRecomp/XenonRecomp', config,
                                           source / 'XenonUtils/ppc_context.h'])
    # This import-free fixture has none of the eight ABI save/restore helpers.
    # Only those exact eight upstream diagnostics are allowed, never a generic
    # ERROR suppression or an unsupported/call/switch warning.
    harmless = r'^ERROR: __(?:restgprlr_14|savegprlr_14|restfpr_14|savefpr_14|restvmx_14|savevmx_14|restvmx_64|savevmx_64) address is unspecified$'
    errors = '\n'.join(line for line in log.splitlines() if not re.fullmatch(harmless, line))
    record('generator_diagnostics', not re.search(r'ERROR:|outside|Unrecognized|unimplemented|Direct call|RC bit', errors), [], log)
    shared = (gen / 'ppc_recomp_shared.h').read_text()
    record('four_functions_without_duplicates', shared.count('PPC_EXTERN_FUNC(') == 4, [], shared)
    boundary = meta['functions']['fn_boundary']
    (out / 'switch_decls.h').write_text(f'#define execute_boundary sub_{boundary:X}\n')
    binary = out / 'switch_execution'
    cpp = sorted(gen.glob('ppc_recomp.*.cpp'))
    run('compile_execution', [args.cxx, '-std=c++17', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
        '-I' + str(source / 'XenonUtils'), '-I' + str(source / 'thirdparty/simde'),
        '-I' + str(ROOT / 'include'), '-I' + str(gen), '-I' + str(out),
        '-include', ROOT / 'include/rcomp/ppc_prelude.h',
        ROOT / 'cpu/tests/test_switch_execution.cpp', *cpp, '-o', binary])
    execution = run('execute_switches', [binary, out / 'image.bin'])
    record('execution_verdict', execution.count(' PASS') == 19 and 'FAIL' not in execution, [], execution)

    first_base = int(re.search(r'base = (0x[0-9A-Fa-f]+)', switches)[1], 16)
    first_label = int(re.search(r'labels = \[\s*(0x[0-9A-Fa-f]+)', switches)[1], 16)
    text_section = meta['sections']['.text']
    bad = {
        'unmapped_base': re.sub(r'base = 0x[0-9A-Fa-f]+', 'base = 0x91000000', switches, count=1),
        'unaligned_base': switches.replace(f'base = 0x{first_base:X}', f'base = 0x{first_base + 1:X}', 1),
        'noncode_base': re.sub(r'base = 0x[0-9A-Fa-f]+', f'base = {meta["sections"][".rdata"]["va"]}', switches, count=1),
        'bad_register': switches.replace('r = 3', 'r = 32', 1),
        'empty_labels': re.sub(r'labels = \[[^]]*\]', 'labels = []', switches, count=1),
        'unaligned_label': switches.replace(f'0x{first_label:X},', f'0x{first_label + 1:X},', 1),
        'outside_section_label': switches.replace(f'0x{first_label:X},', f'{text_section["va"] + text_section["size"]},', 1),
        'noncode_label': switches.replace(f'0x{first_label:X},', f'{meta["sections"][".rdata"]["va"]},', 1),
        'missing_bctr_at_section_end': re.sub(r'base = 0x[0-9A-Fa-f]+', f'base = {text_section["va"] + text_section["size"] - 4}', switches, count=1),
        'duplicate_bctr_mapping': switches + f'\n[[switch]]\nbase = {first_base + 4}\nr = 3\nlabels = [{first_label}]\n',
    }
    for name, content in bad.items():
        badtables = out / (name + '.toml')
        badtables.write_text(content)
        badgen = out / (name + '-generated')
        badgen.mkdir()
        config.write_text(config_template.format(gen=badgen.name, tables=badtables.name))
        log = run('reject_' + name, [build / 'XenonRecomp/XenonRecomp', config,
                  source / 'XenonUtils/ppc_context.h'], expected=1, timeout=5)
        record('diagnostic_' + name, 'ERROR: invalid switch table' in log and not list(badgen.iterdir()), [], log)
    config.write_text(config_template.format(gen=gen.name, tables=tables.name))


if __name__ == '__main__':
    main()
