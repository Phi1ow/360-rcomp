"""Run commands inside the R-comp Cygwin (the toolchain the PS5 builds use), streaming their output."""
import os
import pathlib
import re
import subprocess
import threading
import time

from .jobs import Blocked, Failed


def to_cyg(path):
    """C:\\a\\b -> /cygdrive/c/a/b (POSIX paths pass through)."""
    p = str(path)
    m = re.match(r'^([A-Za-z]):[\\/](.*)$', p)
    if m:
        return f'/cygdrive/{m.group(1).lower()}/' + m.group(2).replace('\\', '/')
    return p.replace('\\', '/')


def bash_path(settings):
    configured = pathlib.Path(settings.get('cygwin_bash'))
    if configured.is_file():
        return configured
    rcomp = pathlib.Path(settings.get('rcomp_root'))
    for cand in (rcomp / 'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe',
                 rcomp / 'build/prime-host-tools/cygwin/bin/bash.exe'):
        if cand.is_file():
            return cand
    return None


def _kill_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
    else:
        proc.kill()


def run(job, settings, argv, cwd=None, env=None, tails=(), label=None, timeout=None):
    """Run argv (POSIX words) in Cygwin bash from cwd (Windows path). Stream stdout/stderr and any
    `tails` files (Windows paths of logs the command writes) into the job log. Return the exit code."""
    bash = bash_path(settings)
    if bash is None:
        raise Blocked('Cygwin bash not found: set its path in Settings (R-comp uses '
                      'build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe)')
    script = ('cd "$RCI_CWD" && export PATH=/usr/bin:/bin LLVM_CONFIG=/usr/bin/llvm-config '
              'PYTHONDONTWRITEBYTECODE=1 && exec "$@"')
    full_env = dict(os.environ)
    full_env.update({k: str(v) for k, v in (env or {}).items()})
    full_env['RCI_CWD'] = to_cyg(cwd or settings.get('rcomp_root'))
    full_env['CHERE_INVOKING'] = '1'
    shown = ' '.join(argv)
    job.log('INFO', f'$ {shown}')
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    proc = subprocess.Popen([str(bash), '-c', script, '_', *argv], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=full_env, creationflags=flags)
    stop = threading.Event()

    def pump():
        for raw in iter(proc.stdout.readline, b''):
            job.log('OUT', raw.decode('utf-8', 'replace').rstrip('\r\n'))

    def follow(path):
        path = pathlib.Path(path)
        pos = path.stat().st_size if path.is_file() else 0  # skip what an earlier run left in it
        while not stop.is_set():
            try:
                if path.is_file():
                    with open(path, 'rb') as f:
                        if path.stat().st_size < pos:
                            pos = 0
                        f.seek(pos)
                        data = f.read()
                        if data:
                            cut = data.rfind(b'\n') + 1
                            for line in data[:cut].decode('utf-8', 'replace').splitlines():
                                job.log('OUT', f'[{path.name}] {line}')
                            pos += cut
            except OSError:
                pass
            stop.wait(1.0)

    threads = [threading.Thread(target=pump, daemon=True)]
    threads += [threading.Thread(target=follow, args=(t,), daemon=True) for t in tails]
    for t in threads:
        t.start()
    start = time.time()
    try:
        while proc.poll() is None:
            if job.cancel_event.is_set():
                _kill_tree(proc)
                job.check_cancel()
            if timeout and time.time() - start > timeout:
                _kill_tree(proc)
                raise Failed(f'{label or argv[0]} exceeded {timeout} s')
            time.sleep(0.5)
    finally:
        stop.set()
        threads[0].join(5)
        for t in threads[1:]:
            t.join(3)
    job.log('INFO', f'{label or argv[0]}: exit {proc.returncode} after {time.time() - start:.0f} s')
    return proc.returncode
