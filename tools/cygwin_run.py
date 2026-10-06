#!/usr/bin/env python3
"""Run a command inside the R-comp Cygwin, from the repository root (Windows host).

The Cygwin is the one tools/bootstrap_cygwin.py extracts (build/prime-host-tools/cygwin), or the developer's full
install (build/vulkan-gta-radv-20260928/host-cygwin-full); RCOMP_CYGWIN_BASH names another bash.exe.
Same environment as the installer's pipeline: PATH=/usr/bin:/bin and LLVM_CONFIG=/usr/bin/llvm-config.

    python tools/cygwin_run.py bash tools/m6_setup.sh
    python tools/cygwin_run.py bash tools/build_extract_xiso.sh
    python tools/cygwin_run.py                      (an interactive shell)
"""
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
CANDIDATES = ('build/prime-host-tools/cygwin/bin/bash.exe', 'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe')
SCRIPT = ('cd "$RCOMP_CWD" && export PATH=/usr/bin:/bin LLVM_CONFIG=/usr/bin/llvm-config PYTHONDONTWRITEBYTECODE=1 '
          '&& exec "$@"')


def to_cygwin_path(path):
    """C:\\a\\b -> /cygdrive/c/a/b (POSIX paths pass through)."""
    text = str(path)
    m = re.match(r'^([A-Za-z]):[\\/](.*)$', text)
    if m:
        return f'/cygdrive/{m.group(1).lower()}/' + m.group(2).replace('\\', '/')
    return text.replace('\\', '/')


def find_bash(root=REPO, environ=None):
    environ = os.environ if environ is None else environ
    named = environ.get('RCOMP_CYGWIN_BASH')
    if named:
        return pathlib.Path(named) if pathlib.Path(named).is_file() else None
    for rel in CANDIDATES:
        if (pathlib.Path(root) / rel).is_file():
            return pathlib.Path(root) / rel
    return None


def command(bash, argv, cwd):
    """(argv, env) that run `argv` (POSIX words) in Cygwin bash from `cwd` (a Windows path)."""
    env = dict(os.environ)
    env['RCOMP_CWD'] = to_cygwin_path(cwd)
    env['CHERE_INVOKING'] = '1'
    return [str(bash), '--noprofile', '--norc', '-c', SCRIPT, '_', *(argv or ['bash', '-i'])], env


def run(argv, root=REPO, cwd=None, stream=None):
    """Run argv in the R-comp Cygwin; return the exit code (127 when there is no Cygwin)."""
    bash = find_bash(root)
    if bash is None:
        print('Cygwin not found: run python tools/bootstrap_cygwin.py first (see docs/INSTALL_WINDOWS.md)', file=sys.stderr)
        return 127
    cmd, env = command(bash, argv, cwd or root)
    return subprocess.call(cmd, env=env, stdout=stream, stderr=subprocess.STDOUT if stream else None)


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
