#!/usr/bin/env python3
"""Prepare a Windows PC for R-comp: everything the installer's "Check toolchain" asks for. Python standard library only.

    python tools/setup_windows.py            run every step that is not done yet
    python tools/setup_windows.py --list     show the steps and which ones are done

Needs Git for Windows (git on PATH) and Python 3.9 or newer, and downloads about 430 MB in total. Each step is skipped
when its result is already in place, so the command can simply be run again after a failure.

  sources     git submodule update --init --recursive third_party/XenonRecomp            (~65 MB, pinned commits)
  zstandard   pip install --target build/prime-host-tools/pylibs zstandard==0.25.0       (0.5 MB, needed by the next step)
  cygwin      tools/bootstrap_cygwin.py: the locked Cygwin, 146 packages with SHA-512     (~270 MB, a few minutes)
  kit         tools/install_kit.py: the prebuilt PS5 toolchain kit, SHA-256 checked       (~95 MB)
  extract-xiso  tools/build_extract_xiso.sh: the pinned source, built with the Cygwin's gcc
  xenonrecomp   tools/m6_setup.sh: patched XenonRecomp and XenonAnalyse, and rcomp_xex_decode

Nothing outside the checkout is touched: no registry, no PATH, no system Python package.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import cygwin_run  # noqa: E402
import install_kit  # noqa: E402

PYLIBS = 'build/prime-host-tools/pylibs'


class Step:
    def __init__(self, name, what, done, run):
        self.name, self.what, self.done, self.run = name, what, done, run


def exists_any(root, *rels):
    return any((root / rel).exists() for rel in rels)


def kit_in_place(root):
    lock_path = root / 'deps/kit.lock.json'
    if not lock_path.is_file():
        return False
    return not install_kit.missing_files(json.loads(lock_path.read_text(encoding='utf-8')), root)


def cygwin_complete(root):
    """A usable Cygwin: the bootstrap's one needs its completion marker (written last, so an interrupted run has none);
    another one (the developer's full install) needs llvm-config, which the SDK compiler wrapper asks for."""
    bash = cygwin_run.find_bash(root)
    if bash is None:
        return False
    cygwin_root = bash.parent.parent
    if cygwin_root == root / 'build/prime-host-tools/cygwin':
        return (cygwin_root / 'etc/rcomp-bootstrap-complete').is_file()
    return any((bash.parent / name).exists() for name in ('llvm-config', 'llvm-config.exe'))


def steps(root=REPO):
    root = pathlib.Path(root)

    def sources():
        if not (root / '.git').exists():
            print('this folder is not a git clone (a downloaded ZIP has no XenonRecomp submodule): '
                  'git clone https://github.com/Phi1ow/360-rcomp.git', file=sys.stderr)
            return 1
        return subprocess.call(['git', 'submodule', 'update', '--init', '--recursive', 'third_party/XenonRecomp'], cwd=root)

    def zstandard():
        return subprocess.call([sys.executable, '-m', 'pip', 'install', '--quiet', '--target', str(root / PYLIBS),
                                'zstandard==0.25.0'])

    def script(name, *args):
        return lambda: subprocess.call([sys.executable, str(root / 'tools' / name), *args])

    def bootstrap():
        target = root / 'build/prime-host-tools/cygwin'
        if target.exists():
            print(f'{target} exists but is incomplete (an interrupted bootstrap?): delete that folder and run again',
                  file=sys.stderr)
            return 1
        return subprocess.call([sys.executable, str(root / 'tools/bootstrap_cygwin.py')])

    def in_cygwin(*argv):
        return lambda: cygwin_run.run(list(argv), root=root)

    return [
        Step('sources', 'XenonRecomp sources (git submodules)',
             lambda: exists_any(root, 'third_party/XenonRecomp/thirdparty/fmt/CMakeLists.txt'), sources),
        Step('zstandard', 'zstandard 0.25.0 for the Cygwin bootstrap',
             lambda: cygwin_complete(root) or exists_any(root, PYLIBS + '/zstandard'), zstandard),
        Step('cygwin', 'the locked Cygwin (build/prime-host-tools/cygwin)',
             lambda: cygwin_complete(root), bootstrap),
        Step('kit', 'the prebuilt PS5 toolchain kit', lambda: kit_in_place(root), script('install_kit.py')),
        Step('extract-xiso', 'the disc image extractor',
             lambda: exists_any(root, 'build/catalog-tools/extract-xiso/extract-xiso.exe'),
             in_cygwin('bash', 'tools/build_extract_xiso.sh')),
        Step('xenonrecomp', 'patched XenonRecomp, XenonAnalyse and the XEX decoder',
             lambda: all(exists_any(root, *alts) for alts in (
                 ('build/cpu-xenonrecomp/XenonRecomp/XenonRecomp.exe', 'build/catalog-tools/xenonrecomp-v23/XenonRecomp/XenonRecomp.exe'),
                 ('build/cpu-xenonrecomp/XenonAnalyse/XenonAnalyse.exe', 'build/catalog-tools/xenonrecomp-v23/XenonAnalyse/XenonAnalyse.exe'),
                 ('build/cpu-xex-decode.exe', 'build/prime-xex-decode-v18/rcomp_xex_decode.exe'))),
             in_cygwin('bash', 'tools/m6_setup.sh')),
    ]


def say(text):
    print(text, flush=True)  # in order with the output of the steps, also when redirected to a file


def run_steps(plan, only=(), skip=(), log=say):
    """Run the steps in order, stop at the first failure. Returns the process exit code."""
    for step in plan:
        if (only and step.name not in only) or step.name in skip:
            log(f'SKIPPED  {step.name}: not selected')
            continue
        if step.done():
            log(f'PASS     {step.name}: already done ({step.what})')
            continue
        log(f'RUNNING  {step.name}: {step.what}')
        start = time.time()
        try:
            code = step.run()
        except OSError as e:  # git or pip not found, for example
            print(f'{step.name}: {e}', file=sys.stderr)
            code = 127
        if code != 0 or not step.done():
            log(f'FAIL     {step.name}: exit {code} after {time.time() - start:.0f} s, result not in place')
            return 1
        log(f'PASS     {step.name}: {time.time() - start:.0f} s')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', action='store_true', help='show the steps and which ones are done')
    ap.add_argument('--only', action='append', default=[], metavar='STEP', help='run only this step (repeatable)')
    ap.add_argument('--skip', action='append', default=[], metavar='STEP', help='leave this step out (repeatable)')
    args = ap.parse_args(argv)
    plan = steps()
    names = [s.name for s in plan]
    for name in args.only + args.skip:
        if name not in names:
            ap.error(f'unknown step {name!r}; steps: {", ".join(names)}')
    if sys.platform != 'win32':
        print('This script prepares a Windows PC. On Linux, WSL or macOS install git, cmake, ninja and clang, then run '
              'tools/m6_setup.sh.', file=sys.stderr)
        return 2
    if args.list:
        for s in plan:
            print(('done     ' if s.done() else 'missing  ') + f'{s.name}: {s.what}')
        return 0
    code = run_steps(plan, args.only, args.skip)
    if code == 0:
        say('PASS     the Windows toolchain is ready: python rcomp-installer/server.py, then "Check toolchain"')
    return code


if __name__ == '__main__':
    sys.exit(main())
