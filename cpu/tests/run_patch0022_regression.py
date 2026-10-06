#!/usr/bin/env python3
"""Generator patch 0022 (symbol_prefix, module_import_libraries) on original fixtures; never edits output.

Builds patch0022_provider.s and patch0022_consumer.s with cpu/tools/mkxex.py at two
image bases, generates each with its own symbol_prefix (the consumer also with
module_import_libraries = ["Provider.dll"]), checks the generated names, links the
unmodified C++ of both modules into one program with test_patch0022_execution.cpp
and runs it. Negative evidence: without the prefixes the same two modules do not
link (duplicate symbols); a library without import slot, a malformed prefix and a
module import left out of module_import_libraries stay explicit. With
--baseline-build/--baseline-source (a generator without patch 0022), the output
without the new options is byte-identical to it.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HARMLESS = (r'^ERROR: __(?:restgprlr_14|savegprlr_14|restfpr_14|savefpr_14|restvmx_14|savevmx_14|restvmx_64|savevmx_64)'
            r' address is unspecified$')
BLOCKING = r'ERROR:|outside|Unrecognized|unimplemented|Direct call|RC bit|no switch table|Unable to decode'
BASES = {'provider': 0x8A000000, 'consumer': 0x8B000000}
SHARED = re.compile(r'^(sub_[0-9A-F]+|__imp__(?!rcomp_)\w+)$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('source', 'build', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--baseline-build', type=Path, help='generator built without patch 0022')
    ap.add_argument('--baseline-source', type=Path, help='source tree of --baseline-build')
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

    def run(name, command, expected=0, timeout=120):
        command = [str(x) for x in command]
        try:
            # C locale: the linker's duplicate-symbol messages are matched below.
            p = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                               env=dict(os.environ, LC_ALL='C', LANG='C', LANGUAGE='C'))
        except subprocess.TimeoutExpired:
            record(name, False, command, stderr='timeout', actual_exit=124)
        ok = p.returncode == expected if expected is not None else True
        record(name, ok, command, p.stdout, p.stderr, p.returncode)
        return p.stdout + p.stderr, p.returncode

    fixtures = {}
    for module, base in BASES.items():
        work = out / 'fixtures' / module
        work.mkdir(parents=True)
        run(f'{module}_make_xex', [sys.executable, ROOT / 'cpu/tools/mkxex.py', ROOT / f'cpu/tests/patch0022_{module}.s',
                                   '--out', work / 'fixture', '--base', hex(base)])
        meta = json.loads((work / 'fixture.json').read_text())
        data = (work / 'fixture.xex').read_bytes()
        header = int.from_bytes(data[8:12], 'big')
        (work / 'image.bin').write_bytes(data[header:])
        record(f'{module}_base', meta['base'] == base, stdout=json.dumps(meta['base']))
        fixtures[module] = (work, meta)

    def generate(tag, module, generator, generator_source, extra='', expected=0):
        fixture, _ = fixtures[module]
        work = out / tag
        (work / 'generated').mkdir(parents=True)
        (work / 'fixture.xex').write_bytes((fixture / 'fixture.xex').read_bytes())
        (work / 'fixture.toml').write_text('[main]\nfile_path="fixture.xex"\nout_directory_path="generated"\n' + extra)
        log, _ = run(f'{tag}_generate', [generator / 'XenonRecomp/XenonRecomp', work / 'fixture.toml',
                                        generator_source / 'XenonUtils/ppc_context.h'], expected=expected)
        diagnostics = '\n'.join(line for line in log.splitlines() if not re.fullmatch(HARMLESS, line))
        return work / 'generated', diagnostics

    prefixes = {'provider': 'rcomp_mod_provider_', 'consumer': 'rcomp_mod_consumer_'}
    provider, provider_log = generate('provider', 'provider', build, source, 'symbol_prefix = "rcomp_mod_provider_"\n')
    consumer, consumer_log = generate('consumer', 'consumer', build, source,
                                      'symbol_prefix = "rcomp_mod_consumer_"\nmodule_import_libraries = ["Provider.dll"]\n')
    for name, log in (('provider', provider_log), ('consumer', consumer_log)):
        record(f'{name}_generator_diagnostics', not re.search(BLOCKING, log), stdout=log)
    consumer_meta = fixtures['consumer'][1]
    imports = {i['label']: i['thunk'] for i in consumer_meta['imports']}
    slots = {r['label']: r['slot'] for r in consumer_meta['import_records']}
    mapping = (consumer / 'ppc_func_mapping.cpp').read_text()
    thunk_name = 'rcomp_mod_consumer___imp__rcomp_module_Provider_dll_0001'
    record('consumer_table_prefixed', 'PPCFuncMapping rcomp_mod_consumer_PPCFuncMappings[] = {' in mapping
           and 'PPCFuncMapping PPCFuncMappings[]' not in mapping, stdout=mapping)
    record('module_import_thunk_through_slot',
           f'PPC_FUNC({thunk_name}) {{' in mapping and
           f'PPC_CALL_INDIRECT_FUNC(PPC_LOAD_U32(0x{slots["slot_provider_add"]:X}));' in mapping and
           f'{{ 0x{imports["imp_provider_add"]:X}, {thunk_name} }},' in mapping, stdout=mapping)
    record('unprovided_import_prefixed_and_unresolved',
           'PPC_FUNC(rcomp_mod_consumer___imp__rcomp_unresolved_Missing_dll_0007) {' in mapping and
           'PPC_UNRESOLVED_IMPORT("Missing_dll", 0x0007);' in mapping, stdout=mapping)
    record('kernel_thunk_keeps_host_name',
           f'{{ 0x{imports["imp_kernel_frequency"]:X}, __imp__KeQueryPerformanceFrequency }},' in mapping, stdout=mapping)
    for module, gen in (('provider', provider), ('consumer', consumer)):
        entries = re.findall(r'^\t\{ 0x([0-9A-F]+), (\w+) \},$', (gen / 'ppc_func_mapping.cpp').read_text(), re.M)
        entry = fixtures[module][1]['entry']
        loose = [name for _, name in entries if not SHARED.match(name) and not name.startswith(prefixes[module])]
        record(f'{module}_symbols_prefixed_or_shared', entries and not loose, stdout=json.dumps(entries))
        record(f'{module}_entry_prefixed', (f'{entry:X}', prefixes[module] + '_xstart') in entries, stdout=json.dumps(entries))

    meta = {m: fixtures[m][1]['functions'] for m in fixtures}
    decls = ['#pragma once', '#include <cstdint>',
             'extern PPCFuncMapping rcomp_mod_provider_PPCFuncMappings[];',
             'extern PPCFuncMapping rcomp_mod_consumer_PPCFuncMappings[];',
             'constexpr const PPCFuncMapping* kProviderTable = rcomp_mod_provider_PPCFuncMappings;',
             'constexpr const PPCFuncMapping* kConsumerTable = rcomp_mod_consumer_PPCFuncMappings;']
    values = {'kProviderBase': BASES['provider'], 'kConsumerBase': BASES['consumer'],
              'kProviderBytes': (fixtures['provider'][0] / 'image.bin').stat().st_size,
              'kConsumerBytes': (fixtures['consumer'][0] / 'image.bin').stat().st_size,
              'kProviderEntry': fixtures['provider'][1]['entry'], 'kProviderAdd': meta['provider']['provider_add'],
              'kConsumerEntry': consumer_meta['entry'], 'kConsumerCallAdd': meta['consumer']['consumer_call_add'],
              'kConsumerCallKernel': meta['consumer']['consumer_call_kernel'],
              'kConsumerCallMissing': meta['consumer']['consumer_call_missing'],
              'kThunkProviderAdd': imports['imp_provider_add'], 'kSlotProviderAdd': slots['slot_provider_add']}
    decls += [f'constexpr uint32_t {k} = 0x{v:X}u;' for k, v in values.items()]
    (out / 'patch0022_decls.h').write_text('\n'.join(decls) + '\n')
    compile_flags = [args.cxx, '-std=c++17', '-O2', '-I' + str(source / 'XenonUtils'),
                     '-I' + str(source / 'thirdparty/simde'), '-I' + str(ROOT / 'include'),
                     '-include', ROOT / 'include/rcomp/ppc_prelude.h', '-include', ROOT / 'cpu/tests/patch0022_hooks.h']
    binary = out / 'patch0022_execution'
    run('link_two_modules', [*compile_flags, '-I' + str(consumer), '-I' + str(out),
                             ROOT / 'cpu/tests/test_patch0022_execution.cpp',
                             *sorted(provider.glob('ppc_*.cpp')), *sorted(consumer.glob('ppc_*.cpp')), '-o', binary],
        timeout=300)
    execution, _ = run('execute', [binary, fixtures['provider'][0] / 'image.bin', fixtures['consumer'][0] / 'image.bin'])
    record('execution_verdict', 'patch0022/execution PASS' in execution and ' FAIL' not in execution, stdout=execution)

    # Negative evidence.
    plain_provider, _ = generate('plain_provider', 'provider', build, source)
    plain_consumer, _ = generate('plain_consumer', 'consumer', build, source)
    (out / 'empty_main.cpp').write_text('int main() { return 0; }\n')
    link, rc = run('unprefixed_modules_do_not_link', [*compile_flags, out / 'empty_main.cpp',
                                                      *sorted(plain_provider.glob('ppc_*.cpp')),
                                                      *sorted(plain_consumer.glob('ppc_*.cpp')), '-o', out / 'unprefixed'],
                   expected=None, timeout=300)
    record('unprefixed_link_reports_duplicates', rc != 0 and re.search(r'multiple definition|duplicate symbol', link)
           and 'PPCFuncMappings' in link and '_xstart' in link, stdout=link)
    plain_mapping = (plain_consumer / 'ppc_func_mapping.cpp').read_text()
    record('module_import_needs_the_option', 'PPC_UNRESOLVED_IMPORT("Provider_dll", 0x0001);' in plain_mapping,
           stdout=plain_mapping)
    _, log = generate('slotless', 'consumer', build, source,
                      'symbol_prefix = "rcomp_mod_consumer_"\nmodule_import_libraries = ["Missing.dll"]\n')
    slotless = (out / 'slotless/generated/ppc_func_mapping.cpp').read_text()
    record('library_without_slot_reported', re.search(r'^ERROR: module import Missing\.dll ordinal 0x7 at 0x[0-9A-F]+ '
                                                      r'has no import slot$', log, re.M) is not None
           and 'PPC_UNRESOLVED_IMPORT("Missing_dll", 0x0007);' in slotless, stdout=log)
    _, log = generate('bad_prefix', 'consumer', build, source, 'symbol_prefix = "9bad"\n', expected=255)
    record('malformed_prefix_rejected', 'ERROR: symbol_prefix "9bad" is not a C identifier prefix' in log, stdout=log)

    if args.baseline_build:
        if not args.baseline_source:
            ap.error('--baseline-build needs --baseline-source')
        for module in fixtures:
            base_gen, _ = generate(f'baseline_{module}', module, args.baseline_build.resolve(),
                                   args.baseline_source.resolve())
            new = out / f'plain_{module}/generated'
            names = sorted(p.name for p in new.iterdir())
            same = names == sorted(p.name for p in base_gen.iterdir()) and all(
                (new / n).read_bytes() == (base_gen / n).read_bytes() for n in names)
            record(f'baseline_identical_without_options_{module}', same, stdout=json.dumps(names))


if __name__ == '__main__':
    main()
