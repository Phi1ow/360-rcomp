#!/usr/bin/env python3
"""Original instruction oracles; never edits XenonRecomp output."""
import argparse
from decimal import Decimal, localcontext
import hashlib
import json
from pathlib import Path
import random
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--prefix', default='powerpc-linux-gnu-')
    ap.add_argument('--cxx', default='clang++')
    a = ap.parse_args()
    source, build, out = (p.resolve() for p in (a.source, a.build, a.out))
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be fresh and under build/')
    out.mkdir(parents=True)
    steps = []

    def save(step):
        steps.append(step)
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(step['name'] + ': ' + ('PASS' if step['exit_code'] == 0 else 'FAIL'), flush=True)
        if step['exit_code']:
            raise SystemExit(1)

    def run(name, cmd, timeout=60):
        cmd = [str(c) for c in cmd]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            step = dict(name=name, command=cmd, exit_code=p.returncode, stdout=p.stdout, stderr=p.stderr)
        except subprocess.TimeoutExpired:
            step = dict(name=name, command=cmd, exit_code=124, stdout='', stderr='timeout')
        save(step)
        return step['stdout'] + step['stderr']

    def expect(name, ok, evidence):
        save(dict(name=name, command=[], exit_code=0 if ok else 1, stdout=evidence, stderr=''))

    bin_dir, gen_dir = out / 'bin', out / 'gen'
    bin_dir.mkdir(); gen_dir.mkdir()
    obj = bin_dir / 'estimate_select_cache.o'
    run('assemble_original', [a.prefix + 'as', '-a32', '-be', '-mregnames', '-mpower7',
        '-maltivec', '-o', obj, ROOT / 'cpu/tests/estimate_select_cache.s'])
    symbols = run('symbols', [a.prefix + 'nm', '--numeric-sort', obj])
    dis = run('disassemble', [a.prefix + 'objdump', '-d', '-EB', obj])
    (bin_dir / 'estimate_select_cache.dis').write_text(dis)
    relocs = run('relocations', [a.prefix + 'objdump', '-r', obj])
    expect('no_relocations', not re.search(r'^[0-9a-f]{8} ', relocs, re.M), 'Original fixture has no relocated instruction')
    log = run('recompile', [build / 'XenonRecomp/XenonRecomp', bin_dir, gen_dir], 10)
    expect('generator_diagnostics', not re.search(r'unimplemented|Unrecognized|Direct call|ERROR:|RC bit', log), log)
    generated = gen_dir / 'estimate_select_cache.cpp'
    (gen_dir / 'main.cpp').rename(out / 'upstream_main.cpp.txt')
    header = ['#define PPC_CONFIG_H_INCLUDED', '#include <ppc_context.h>',
              '#undef PPC_TRAP', '[[noreturn]] void TESTDOUBLE_guest_trap(uint32_t);',
              '#define PPC_TRAP(address) TESTDOUBLE_guest_trap(address)']
    for address, name in re.findall(r'^([0-9a-f]+) T (\w+)$', symbols, re.M):
        symbol = f'estimate_select_cache_{int(address, 16):X}'
        header += [f'PPC_FUNC({symbol});', f'#define execute_{name} {symbol}']
    (out / 'instruction_decls.h').write_text('\n'.join(header) + '\n')
    binary = out / 'instructions'
    options = [a.cxx, '-std=c++17', '-O2', '-march=x86-64-v3', '-ffp-contract=on',
        '-pthread', '-I' + str(source / 'XenonUtils'), '-I' + str(source / 'thirdparty/simde'),
        '-I' + str(ROOT / 'include'), '-I' + str(out), '-include', ROOT / 'include/rcomp/ppc_prelude.h',
        '-include', out / 'instruction_decls.h']
    run('compile', options + [ROOT / 'cpu/tests/test_estimate_select_cache.cpp', generated, '-o', binary])
    result = run('execute', [binary], 30)
    expect('execution_verdict', result.count(' PASS') == 12 and 'FAIL' not in result, result)

    # Decimal arithmetic is independent of the generated binary64 sqrt/divide.
    # Cover every normal exponent and sparse/extreme subnormals, four modes,
    # plus reproducible random mantissas. This is an estimate bound, not a
    # claim of the actual Xenon microarchitecture's bit pattern.
    rng = random.Random(0x360)
    inputs = {1, 2, 3, (1 << 52) - 1}
    for exp in range(1, 2047):
        inputs.add(exp << 52)
        inputs.add((exp << 52) | ((1 << 52) - 1))
        inputs.add((exp << 52) | rng.getrandbits(52))
    inputs.update(1 << bit for bit in range(52))
    samples = [(bits, mode) for bits in sorted(inputs) for mode in range(4)]
    (out / 'numeric-input.txt').write_text(''.join(f'{bits:016x} {mode}\n' for bits, mode in samples))
    numeric = subprocess.run([str(binary), str(out / 'numeric-input.txt')],
                             capture_output=True, text=True, timeout=60)
    (out / 'numeric-output.txt').write_text(numeric.stdout)
    expect('numeric_process', numeric.returncode == 0, f'rc={numeric.returncode} stderr={numeric.stderr}')
    lines = numeric.stdout.splitlines()
    expect('numeric_count', len(lines) == len(samples), f'{len(lines)} / {len(samples)} samples')
    worst = Decimal(0)
    with localcontext() as context:
        context.prec = 90
        for (bits, mode), line in zip(samples, lines):
            actual_bits, actual_mode, result_bits = line.split()
            if int(actual_bits, 16) != bits or int(actual_mode) != mode:
                raise SystemExit('FAIL: reordered numeric samples')
            value = struct.unpack('>d', bits.to_bytes(8, 'big'))[0]
            result_value = struct.unpack('>d', int(result_bits, 16).to_bytes(8, 'big'))[0]
            oracle = Decimal(1) / Decimal.from_float(value).sqrt()
            relative = abs((Decimal.from_float(result_value) - oracle) / oracle)
            worst = max(worst, relative)
        expect('frsqrte_decimal_error_bound', worst < Decimal(2) ** -50,
               f'{len(samples)} samples; worst relative error={worst}; ISA limit=1/32; tested tighter limit=2^-50')
    # Neighbor remains unsupported: never silence a family wholesale.
    negative_bin, negative_gen = out / 'negative-bin', out / 'negative-gen'
    negative_bin.mkdir(); negative_gen.mkdir()
    neg = out / 'unsupported.s'
    neg.write_text('.text\n.space 4\nfrsqrtes 1,2\nblr\n')
    run('assemble_negative', [a.prefix + 'as', '-a32', '-be', '-mpower7', '-o', negative_bin / 'unsupported.o', neg])
    negative = run('generate_negative', [build / 'XenonRecomp/XenonRecomp', negative_bin, negative_gen], 10)
    expect('unsupported_neighbor_still_fails_generation', 'frsqrtes' in negative and 'unimplemented' in negative, negative)
    (out / 'provenance.json').write_text(json.dumps({
        'generator_sha256': hashlib.sha256((build / 'XenonRecomp/XenonRecomp.exe').read_bytes()).hexdigest(),
        'generated_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(),
        'host_only': True, 'xenon_bit_exact': False}, indent=2))


if __name__ == '__main__':
    main()
