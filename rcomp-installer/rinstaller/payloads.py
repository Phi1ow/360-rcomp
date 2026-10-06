"""Console payloads: the chmod payload (built here once with the ps5-payload SDK) and ps5vkctl (loaded from
R-comp's build, pinned by SHA-256)."""
import hashlib
import pathlib
import time

from . import cygwin
from .jobs import Blocked, Failed
from .settings import TOOL_ROOT

CHMOD_SRC = TOOL_ROOT / 'payloads' / 'rcomp_chmod.c'
PAYLOAD_DIR = TOOL_ROOT / 'build' / 'payloads'
CTL_SHA = '6eaa5dafb8f948c6a8a737353bf47577750c7c4d4c4957a02be4e9aef6f53212'


def sdk_root(settings):
    configured = settings.get('payload_sdk')
    cands = [pathlib.Path(configured)] if configured else []
    cands.append(pathlib.Path(settings.get('rcomp_root')) / 'build/vulkan-gta-radv-20260928/ps5-sdk')
    for c in cands:
        if (c / 'bin' / 'prospero-clang').is_file():
            return c
    return None


def chmod_target():
    tag = hashlib.sha256(CHMOD_SRC.read_bytes()).hexdigest()[:12]
    return PAYLOAD_DIR / f'rcomp_chmod-{tag}.elf'


def report(settings):
    target = chmod_target()
    sdk = sdk_root(settings)
    ctl = pathlib.Path(settings.get('rcomp_root')) / 'build/platform-ps5-payloads/ps5vkctl.elf'
    return {'chmod_elf': str(target), 'chmod_built': target.is_file(), 'sdk': str(sdk) if sdk else None,
            'ps5vkctl': str(ctl), 'ps5vkctl_ok': ctl.is_file() and
            hashlib.sha256(ctl.read_bytes()).hexdigest() == CTL_SHA}


def chmod_elf(settings, job, force=False):
    """Bytes of the chmod payload, compiled from payloads/rcomp_chmod.c when its build is missing."""
    log = job.log
    target = chmod_target()
    if target.is_file() and not force:
        log('INFO', f'chmod payload: {target.name}')
        return target.read_bytes()
    sdk = sdk_root(settings)
    if sdk is None:
        raise Blocked('chmod payload not built and ps5-payload SDK not found (bin/prospero-clang); '
                      'set "Payload SDK" in Settings')
    PAYLOAD_DIR.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix('.tmp.elf')
    tmp.unlink(missing_ok=True)

    log('STEP', 'building the chmod payload with the ps5-payload SDK')
    code = cygwin.run(job, settings, [cygwin.to_cyg(sdk / 'bin/prospero-clang'), '-std=c11', '-O2', '-Wall',
                                       '-Wextra', '-Werror', '-o', cygwin.to_cyg(tmp), cygwin.to_cyg(CHMOD_SRC)],
                      cwd=TOOL_ROOT, label='prospero-clang', timeout=300)
    if code != 0 or not tmp.is_file():
        raise Failed(f'chmod payload build failed (exit {code})')
    data = tmp.read_bytes()
    if data[:4] != b'\x7fELF' or b'/data/rcomp-installer/chmod.request' not in data:
        raise Failed('chmod payload output is not the expected ELF')
    tmp.replace(target)
    log('PASS', f'chmod payload built: {target} ({len(data)} bytes)')
    return data


def load_ctl(job, settings, console):
    reply = console.ctl_or_none('status', 5)
    if reply is not None:
        job.log('OUT', 'status: ' + reply)
        return 'PASS', 'ps5vkctl already running', {}
    elf = pathlib.Path(settings.get('rcomp_root')) / 'build/platform-ps5-payloads/ps5vkctl.elf'
    if not elf.is_file():
        raise Blocked(f'{elf} not found')
    data = elf.read_bytes()
    if hashlib.sha256(data).hexdigest() != CTL_SHA:
        raise Blocked(f'{elf} is not the verified ps5vkctl (SHA-256 differs)')
    job.log('INFO', 'loader: ' + (console.send_elf(data, 5) or '(no reply)'))
    for _ in range(20):
        reply = console.ctl_or_none('status', 5)
        if reply is not None:
            job.log('OUT', 'status: ' + reply)
            return 'PASS', 'ps5vkctl loaded and answering', {}
        time.sleep(0.5)
    return 'FAIL', 'ps5vkctl did not answer after loading', {}
