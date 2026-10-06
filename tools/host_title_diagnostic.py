#!/usr/bin/env python3
"""Build/run an isolated host entry diagnostic; never authorizes an M6/PS5 title.

Known missing function implementations retain the runtime's fatal-on-call
behavior. Every other inventory invariant, variable binding, generated artifact,
instruction warning and source identity must pass. Output stays under build/.
No existing inventory/BOOT_GATE.json is edited and no console is contacted.
"""
import argparse
import copy
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from m6_boot_gate import check_inventory
from m6_inventory import output_allowed
from runtime_source_inventory import source_imports

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_function_only_blockers(report, errors):
    """Only documented missing functions are admissible to this diagnostic."""
    imports = report.get('imports')
    if not isinstance(imports, dict) or not imports:
        raise ValueError('missing imports inventory')
    expected_reasons = []
    permitted = {'inventory did not explicitly permit linking',
                 'inventory blockers are missing or nonempty'}
    for module, values in imports.items():
        if not isinstance(values, dict):
            raise ValueError('invalid import module')
        missing = values.get('functions_missing')
        if not isinstance(missing, list) or any(not isinstance(name, str) or not name for name in missing):
            raise ValueError('missing function list is invalid')
        if values.get('variables_missing') != [] or values.get('unknown_ordinals') != []:
            raise ValueError('variables and unknown ordinals must be resolved before a diagnostic')
        if missing:
            expected_reasons.append(f'unsupported imports in {module}: ' + ', '.join(missing))
            permitted.add(f'{module}: functions_missing is missing or nonempty')
    if report.get('blocking_reasons') != expected_reasons:
        raise ValueError('inventory has blockers other than the listed missing functions')
    if report.get('supported_for_link') is not (not expected_reasons):
        raise ValueError('inventory support flag contradicts its function list')
    forbidden = [error for error in errors if error not in permitted]
    if forbidden:
        raise ValueError('; '.join(forbidden))


def current_source_selection():
    _, _, units = source_imports(ROOT)
    return {name: sha(ROOT / name) for name in sorted(
        {'runtime/CMakeLists.txt', 'app/src/title_runtime.cpp', *units})}


def source_snapshot():
    roots = ('runtime/src', 'runtime/include', 'platform/common', 'platform/posix',
             'include', 'cpu/runtime', 'app/include', 'tests/title_entry_diagnostic')
    paths = {path for folder in roots for path in (ROOT / folder).rglob('*')
             if path.is_file() and path.suffix in ('.c', '.cpp', '.h', '.hpp', '.txt')}
    paths.update(ROOT / name for name in ('runtime/CMakeLists.txt', 'platform/CMakeLists.txt',
                                         'app/src/title_runtime.cpp', 'tests/xex/TESTDOUBLE_cpu_only_gpu.cpp'))
    return {p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(paths)}


def validate_inventory(work):
    report = json.loads((work / 'inventory.json').read_text())
    checked = copy.deepcopy(report)
    # The existing report's generated artifacts remain immutable. A diagnostic
    # links the current runtime and binds its current registration sources in
    # its OWN report, rather than rewriting the historical production gate.
    selection = current_source_selection()
    checked['runtime_source_selection'] = {'files': selection}
    errors = check_inventory(checked, work)
    validate_function_only_blockers(checked, errors)
    log = work / 'xenonrecomp.log'
    if not log.is_file() or 'unimplemented' in log.read_text(errors='replace').lower():
        raise ValueError('generator log missing or contains unimplemented instructions')
    return {'scope': 'host diagnostic input integrity only',
            'status': 'PASS', 'production_gate': 'BLOCKED' if errors else 'PASS',
            'original_inventory_sha256': sha(work / 'inventory.json'),
            'guest_artifacts': report['artifacts'],
            'current_runtime_registration_sources': selection,
            'known_missing_functions_from_inventory': {
                module: values['functions_missing'] for module, values in report['imports'].items()},
            'graphics': 'NOT TESTED', 'ps5': 'NOT TESTED', 'gameplay': 'NOT TESTED'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--xenon-source', type=Path, required=True)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--run', action='store_true', help='execute after building; default only builds')
    parser.add_argument('--resume', action='store_true', help='reuse this diagnostic build after source changes')
    parser.add_argument('--timeout', type=int, default=30)
    args = parser.parse_args()
    work = args.inventory.resolve()
    out = args.out.absolute()
    if (not output_allowed(out) or not out.resolve().is_relative_to(ROOT / 'build') or
            out.is_symlink() or (out.exists() and not args.resume)):
        parser.error('output must be a safe new directory under build/, or --resume this diagnostic only')
    if args.resume and not (out / 'DIAGNOSTIC_INPUTS.json').is_file():
        parser.error('--resume requires an existing diagnostic build')
    if not 1 <= args.timeout <= 120:
        parser.error('timeout must be 1..120 seconds')
    if not args.game_root.is_dir() or not (args.xenon_source / 'XenonUtils/ppc_context.h').is_file():
        parser.error('game root and patched XenonRecomp source must exist')
    report = validate_inventory(work)
    out.mkdir(parents=True, exist_ok=True)
    report['sources_before'] = source_snapshot()
    (out / 'DIAGNOSTIC_INPUTS.json').write_text(json.dumps(report, indent=2) + '\n')
    commands = [
        ('configure', ['cmake', '-S', str(ROOT / 'tests/title_entry_diagnostic'),
                       '-B', str(out / 'host'), '-G', 'Ninja',
                       '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
                       '-DRCOMP_DIAGNOSTIC_ONLY=ON', '-DXEX_GEN_DIR=' + str(work / 'ppc'),
                       '-DRCOMP_XENONRECOMP_SRC=' + str(args.xenon_source.resolve())]),
        ('build', ['cmake', '--build', str(out / 'host'), '-j6'])]
    results = []
    for stage, command in commands:
        with (out / (stage + '.log')).open('w') as log:
            run = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        results.append({'stage': stage, 'command': command, 'exit': run.returncode})
        (out / 'COMMANDS.json').write_text(json.dumps(results, indent=2) + '\n')
        print(f'{stage}: exit={run.returncode}; {out / (stage + ".log")}', flush=True)
        if run.returncode:
            return 1
    after = source_snapshot()
    if after != report['sources_before']:
        raise RuntimeError('compiled sources changed during diagnostic build; rebuild before execution')
    validate_inventory(work)
    if not args.run:
        print('Host diagnostic compiled; no game execution, graphics or PS5 claim', flush=True)
        return 0
    command = [str(out / 'host/rcomp_title_entry_diagnostic'), str(work / 'plain.xex'),
               str(args.game_root.resolve())]
    run_directory = out / 'runs' / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    run_directory.mkdir(parents=True, exist_ok=False)
    execution_log = run_directory / 'execution.log'
    with execution_log.open('w') as log:
        try:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
            code, timed_out = result.returncode, False
        except subprocess.TimeoutExpired:
            code, timed_out = 124, True
    text = execution_log.read_text(errors='replace')
    execution = {'scope': 'host-only diagnostic, not gameplay or PS5 validation',
                 'command': command, 'process_exit': code, 'timed_out': timed_out,
                 'entry_started': 'RCOMP-APP stage=entry_begin' in text,
                 'fatal_diagnostics': [line for line in text.splitlines() if line.startswith('RCOMP-FATAL')],
                 'graphics': 'NOT TESTED', 'ps5': 'NOT TESTED', 'gameplay': 'NOT TESTED',
                 'production_gate_unchanged': True, 'execution_log': str(execution_log),
                 'execution_log_sha256': sha(execution_log)}
    (run_directory / 'EXECUTION.json').write_text(json.dumps(execution, indent=2) + '\n')
    (out / 'EXECUTION.json').write_text(json.dumps(execution, indent=2) + '\n')
    print(json.dumps(execution, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
