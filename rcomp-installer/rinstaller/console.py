"""The developer's jailbroken PS5: FTP install with read-back, chmod payload, ShadowMount+ registration.

Services (resident payloads loaded at boot): FTP (2120, writes 0666, no SITE CHMOD), ELF loader (9021, runs a
raw .elf un-sandboxed), ps5vkctl (9111, one text command per connection), kernel log (3232).
Writes only under /data/homebrew/<PPSA id>/ and, for registration, /data/shadowmount/manual.lst (after a
backup next to it). Never deletes anything on the console.
"""
import ftplib
import hashlib
import io
import json
import posixpath
import re
import socket
import threading
import time

from .jobs import Blocked, Cancelled, Failed
from .package import title_name

HOMEBREW = '/data/homebrew'
SM_DIR = '/data/shadowmount'
MANUAL_LST = SM_DIR + '/manual.lst'
MANUAL_STATUS = SM_DIR + '/manual.status'
CHMOD_REQUEST_DIR = '/data/rcomp-installer'
CHMOD_REQUEST = CHMOD_REQUEST_DIR + '/chmod.request'
BLOCK = 1 << 20
RETRIES = 4


class CopyMismatch(Exception):
    """A file read back from the console differs from the PC's; the worker re-uploads it."""


class Console:
    def __init__(self, host, ftp_port=2120, loader_port=9021, ctl_port=9111, klog_port=3232):
        self.host = host
        self.ftp_port = int(ftp_port)
        self.loader_port = int(loader_port)
        self.ctl_port = int(ctl_port)
        self.klog_port = int(klog_port)

    # -- FTP -------------------------------------------------------------------------------------------
    def ftp(self, timeout=60):
        f = ftplib.FTP()
        try:
            f.connect(self.host, self.ftp_port, timeout=timeout)
            f.login()
        except OSError as e:
            raise Blocked(f'FTP {self.host}:{self.ftp_port} unreachable ({e}); is the console on with its payloads?')
        f.set_pasv(True)
        return f

    @staticmethod
    def read(f, path):
        buf = io.BytesIO()
        f.retrbinary('RETR ' + path, buf.write, blocksize=BLOCK)
        return buf.getvalue()

    @staticmethod
    def try_read(f, path):
        try:
            return Console.read(f, path)
        except ftplib.error_perm:
            return None

    @staticmethod
    def write(f, path, data):
        f.storbinary('STOR ' + path, io.BytesIO(data), blocksize=BLOCK)

    @staticmethod
    def cwd_exists(f, path):
        """True when CWD into `path` succeeds (restoring the previous directory). Static: listdir and mkdir
        are static too (Console.exists_dir below is the instance form used by the title scan)."""
        try:
            previous = f.pwd()
        except ftplib.all_errors:
            previous = '/'
        try:
            f.cwd(path)
        except ftplib.error_perm:
            return False
        try:
            f.cwd(previous)
        except ftplib.all_errors:
            pass
        return True

    @staticmethod
    def mkdir(f, path):
        """MKD that tolerates an existing directory: FTP servers answer that case with 550 ('File exists'),
        the same code as a real failure, so an error is accepted only when the directory then exists
        (a retried install after an interrupted upload, 4 October 2026)."""
        try:
            f.mkd(path)
        except ftplib.error_perm:
            if not Console.cwd_exists(f, path):
                raise

    @staticmethod
    def remote_sha(f, path, cancel=None, progress=None):
        h = hashlib.sha256()

        def feed(chunk):
            if cancel:
                cancel()
            h.update(chunk)
            if progress:
                progress(len(chunk))
        f.retrbinary('RETR ' + path, feed, blocksize=BLOCK)
        return h.hexdigest()

    @staticmethod
    def size(f, path):
        try:
            f.voidcmd('TYPE I')
            return f.size(path)
        except ftplib.error_perm:
            return None

    @staticmethod
    def listdir(f, path):
        """{name: {'type': 'dir'|'file', 'size': int, 'mode': str}} via MLSD, or None when absent.

        The console's zftpd answers MLSD on a directory that does not exist with an empty listing, not an
        error, so an empty answer is confirmed against the parent's listing."""
        try:
            entries = {}
            for name, facts in f.mlsd(path, facts=['type', 'size', 'unix.mode']):
                if name in ('.', '..') or facts.get('type') in ('cdir', 'pdir'):
                    continue
                entries[name] = {'type': facts.get('type', ''), 'size': int(facts.get('size', 0) or 0),
                                 'mode': facts.get('unix.mode', '')}
        except ftplib.error_perm:
            # Either the directory does not exist or this server has no MLSD (other FTP payloads than the
            # developer console's zftpd): CWD decides existence; the contents are then unknown (empty answer,
            # every file is uploaded and read back).
            return {} if Console.cwd_exists(f, path) else None
        if not entries and path.rstrip('/'):
            parent, name = posixpath.split(path.rstrip('/'))
            siblings = Console.listdir(f, parent or '/')
            if not siblings or siblings.get(name, {}).get('type') != 'dir':
                return None
        return entries

    def exists_dir(self, f, path):
        parent, name = posixpath.split(path.rstrip('/'))
        entries = self.listdir(f, parent)
        if entries == {}:  # the parent exists but its contents are unknown (no MLSD): ask the server directly
            return Console.cwd_exists(f, path)
        return bool(entries and name in entries and entries[name]['type'] == 'dir')

    # -- state -----------------------------------------------------------------------------------------
    def titles(self):
        """What the console knows: homebrew folders, manual.lst lines, manual.status, /system_ex/app ids."""
        with self.ftp(20) as f:
            homebrew = self.listdir(f, HOMEBREW) or {}
            lst = (self.try_read(f, MANUAL_LST) or b'').decode('utf-8', 'replace')
            status = (self.try_read(f, MANUAL_STATUS) or b'').decode('utf-8', 'replace')
            system_ex = self.listdir(f, '/system_ex/app') or {}
            out = {}
            for name, e in homebrew.items():
                if e['type'] != 'dir':
                    continue
                item = out.setdefault(name, {'id': name})
                item['folder'] = f'{HOMEBREW}/{name}'
                raw = self.try_read(f, f'{HOMEBREW}/{name}/sce_sys/param.json')
                if raw:
                    try:
                        p = json.loads(raw.decode('utf-8'))
                        item['name'] = title_name(p)
                        item['param_title_id'] = p.get('titleId')
                    except ValueError:
                        item['name'] = '(param.json unreadable)'
        listed = [line.strip() for line in lst.splitlines() if line.strip()]
        for path in listed:
            name = posixpath.basename(path.rstrip('/'))
            out.setdefault(name, {'id': name})['listed'] = path
        for line in status.splitlines():
            parts = line.split('\t')
            if len(parts) >= 3:
                item = out.setdefault(parts[1], {'id': parts[1]})
                item['status'] = parts[0]
                item['status_path'] = parts[2]
                if len(parts) > 3 and not item.get('name'):
                    item['name'] = parts[3]
        for name, e in system_ex.items():
            if e['type'] != 'dir':
                continue
            if name in out:
                out[name]['registered'] = True
            else:  # a system or otherwise-installed app: listed so its id is never reused, hidden by the UI
                out[name] = {'id': name, 'registered': True, 'system_only': True}
        return sorted(out.values(), key=lambda t: t['id'])

    # -- payloads and control --------------------------------------------------------------------------
    def send_elf(self, data, wait=15):
        try:
            with socket.create_connection((self.host, self.loader_port), timeout=10) as s:
                s.sendall(data)
                s.shutdown(socket.SHUT_WR)
                s.settimeout(wait)
                reply = b''
                try:
                    while chunk := s.recv(4096):
                        reply += chunk
                except socket.timeout:
                    pass
        except OSError as e:
            raise Blocked(f'ELF loader {self.host}:{self.loader_port} unreachable ({e})')
        return reply.decode('utf-8', 'replace').strip()

    def ctl(self, cmd, timeout=30):
        with socket.create_connection((self.host, self.ctl_port), timeout=5) as s:
            s.sendall((cmd + '\n').encode())
            s.settimeout(timeout)
            reply = b''
            try:
                while chunk := s.recv(8192):
                    reply += chunk
            except socket.timeout:
                pass
        return reply.decode('utf-8', 'replace').strip()

    def ctl_or_none(self, cmd, timeout=10):
        try:
            return self.ctl(cmd, timeout)
        except OSError:
            return None

    def klog(self, seconds, line_cb, cancel):
        with socket.create_connection((self.host, self.klog_port), timeout=10) as s:
            s.settimeout(1)
            end, buf = time.time() + seconds, b''
            while time.time() < end:
                cancel()
                try:
                    chunk = s.recv(8192)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                *lines, buf = buf.split(b'\n')
                for line in lines:
                    line_cb(line.decode('utf-8', 'replace').rstrip('\r'))


def _running_ids(procs):
    return set(re.findall(r'PPSA\d{5}', procs or ''))


def transfer(job, console, remote, app, remote_sizes, connections):
    """Verify-or-upload every file with a pool of FTP connections; raise Failed on any mismatch."""
    cancel = job.check_cancel
    work = sorted(app['files'], key=lambda x: -x[2])  # largest first: the pool stays busy to the end
    lock = threading.Lock()
    # inflight: per worker thread, [bytes moved for its current file, that file's size]
    state = {'next': 0, 'done': 0, 'moved': 0, 'sent': 0, 'kept': 0, 'error': None, 'current': {},
             'inflight': {}}
    total = app['bytes']

    def progress(n):
        wid = threading.get_ident()
        with lock:
            state['moved'] += n
            if wid in state['inflight']:
                state['inflight'][wid][0] += n

    def one_file(f, rel, path, size, sha):
        """Verify or upload one file on connection f. Returns 'kept' or 'sent'."""
        target = f'{remote}/{rel}'
        if remote_sizes.get(target) == size and console.remote_sha(f, target, cancel, progress) == sha:
            return 'kept'
        h = hashlib.sha256()
        f.voidcmd('TYPE I')
        with open(path, 'rb') as src, f.transfercmd('STOR ' + target) as conn:
            while chunk := src.read(BLOCK):
                cancel()
                h.update(chunk)
                conn.sendall(chunk)
                progress(len(chunk))
        f.voidresp()
        if h.hexdigest() != sha:
            raise Failed(f'{rel} changed on the PC during the upload')
        remote_sizes[target] = size
        back = console.remote_sha(f, target, cancel, progress)
        if back != sha:
            # Some FTP payloads still flush the file when the read-back starts: give them a moment and read once
            # more before calling it a corrupted copy (seen on a collaborator's console, 4 October 2026: eboot.bin,
            # 104 MB, read back differently right after the upload over 8 parallel connections).
            time.sleep(3)
            back = console.remote_sha(f, target, cancel, progress)
        if back != sha:
            remote_sizes.pop(target, None)
            raise CopyMismatch(f'{rel}: console copy differs from the PC (read back {back[:16]}…, expected {sha[:16]}…; '
                               f'console size {console.size(f, target)}, PC size {size})')
        return 'sent'

    def worker(wid):
        f = None
        try:
            while True:
                with lock:
                    if state['error'] or state['next'] >= len(work):
                        return
                    rel, path, size, sha = work[state['next']]
                    state['next'] += 1
                    state['current'][wid] = rel
                    state['inflight'][threading.get_ident()] = [0, size]
                # Transport errors (dropped connection, temporary 4xx, 550 Cannot ...) and a read-back that does
                # not match (the console's copy is re-uploaded) are retried on a fresh connection, RETRIES times.
                for attempt in range(RETRIES + 1):
                    cancel()
                    try:
                        if f is None:
                            f = console.ftp()
                        outcome = one_file(f, rel, path, size, sha)
                        break
                    except (OSError, EOFError, ftplib.error_temp, ftplib.error_reply, ftplib.error_perm, CopyMismatch) as e:
                        perm = isinstance(e, ftplib.error_perm)
                        if perm and 'Cannot' not in str(e) or attempt == RETRIES:
                            raise Failed(f'{rel}: {type(e).__name__}: {e}')
                        job.log('WARN', f'{rel}: {e} (retry {attempt + 1}/{RETRIES})')
                        try:
                            f.close()
                        except (OSError, AttributeError):
                            pass
                        f = None
                        with lock:
                            state['inflight'][threading.get_ident()][0] = 0
                        # A file left open by an interrupted session (e.g. a killed installer) stays locked
                        # by zftpd for a while: back off 10, 20, 40, 60 s (about two minutes in all).
                        time.sleep(min(10 * 2 ** attempt, 60))
                with lock:
                    state[outcome] += 1
                    state['done'] += size
                    state['inflight'].pop(threading.get_ident(), None)
        except BaseException as e:  # first error stops the pool; reported by the caller
            with lock:
                if not state['error']:
                    state['error'] = e
        finally:
            with lock:
                state['current'].pop(wid, None)
                state['inflight'].pop(threading.get_ident(), None)
            if f is not None:
                try:
                    f.close()
                except OSError:
                    pass

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(max(1, connections))]
    t0 = time.time()
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        for t in threads:
            t.join(5 / len(threads))
        with lock:
            rate = state['moved'] / max(time.time() - t0, 0.001) / 2**20
            # finished files plus the bytes in flight on each connection (capped at the file size: an
            # upload then waits there during its read-back)
            seen = state['done'] + sum(min(m, sz) for m, sz in state['inflight'].values())
            line = (f'  {seen * 100 // max(total, 1)}%  {seen / 2**20:.0f}/{total / 2**20:.0f} MiB  '
                    f'({state["done"] / 2**20:.0f} MiB verified, {rate:.1f} MiB/s on the wire, '
                    f'{state["sent"]} sent, {state["kept"]} already '
                    f'identical, {len(state["current"])} connections busy)')
        if time.time() - state.setdefault('last', t0) >= 5:
            state['last'] = time.time()
            job.log('INFO', line)
    err = state['error']
    if err:
        if isinstance(err, (Failed, Blocked, Cancelled)):
            raise err
        raise Failed(f'transfer error: {type(err).__name__}: {err}')
    if state['done'] != total:
        raise Failed(f'transfer incomplete: {state["done"]} of {total} bytes verified')
    return {'sent': state['sent'], 'kept': state['kept'],
            'rate': state['moved'] / max(time.time() - t0, 0.001) / 2**20}


def install(job, console, app, chmod_elf, overwrite=False, register=True, wait_seconds=90, connections=8):
    """Upload + read back + chmod + register. `app` is package.inspect_app's result (hashed)."""
    log, cancel = job.log, job.check_cancel
    tid, name = app['title_id'], app['name']
    remote = f'{HOMEBREW}/{tid}'
    result = {'title_id': tid, 'name': name, 'remote': remote, 'upload': 'NOT TESTED', 'readback': 'NOT TESTED',
              'chmod': 'NOT TESTED', 'registration': 'NOT TESTED'}
    job.result = result

    log('STEP', f'checking the console ({console.host})')
    procs = console.ctl_or_none('procs')
    if procs is None:
        log('WARN', f'ps5vkctl ({console.ctl_port}) not reachable: cannot check that {tid} is not running')
    elif tid in _running_ids(procs):
        raise Blocked(f'{tid} is running on the console; close it before installing over it')
    with console.ftp() as f:
        existing = console.listdir(f, remote)
        if existing is not None:
            raw = console.try_read(f, remote + '/sce_sys/param.json')
            other = None
            if raw:
                try:
                    p = json.loads(raw.decode('utf-8'))
                    other = title_name(p)
                except ValueError:
                    other = '(unreadable param.json)'
            if other is not None and other != name and not overwrite:
                raise Blocked(f'{remote} already holds "{other}". Choose another PS5 title id, or tick '
                              f'"overwrite" to replace it')
            log('INFO', f'{remote} exists ({other or "no param.json"}); files are updated in place, '
                        f'files not in this app are left untouched')
        status = (console.try_read(f, MANUAL_STATUS) or b'').decode('utf-8', 'replace')
        for line in status.splitlines():
            parts = line.split('\t')
            if len(parts) >= 3 and parts[1] == tid and parts[2].rstrip('/') != remote:
                raise Blocked(f'{tid} is already registered by ShadowMount+ from {parts[2]}; '
                              f'choose another PS5 title id')

        # Upload with read-back, over several FTP connections in parallel (the console's FTP server delivers
        # about 4 MiB/s per connection and scales with the connection count). A file already on the console
        # with the same size is read back first and kept when its SHA-256 matches, so an interrupted install
        # resumes without re-sending everything.
        log('STEP', f'uploading {len(app["files"])} files ({app["bytes"] / 2**20:.1f} MiB) to {remote} '
                    f'over {connections} connections')
        dirs = sorted({posixpath.dirname(f'{remote}/{rel}') for rel, *_ in app['files']} | {remote},
                      key=lambda d: d.count('/'))
        remote_sizes = {}
        for d in dirs:
            parts = d.strip('/').split('/')
            for i in range(2, len(parts) + 1):
                sub = '/' + '/'.join(parts[:i])
                if sub not in remote_sizes:
                    remote_sizes[sub] = None
                    entries = console.listdir(f, sub)
                    if entries is None:
                        console.mkdir(f, sub)
                        entries = {}
                    for n, e in entries.items():
                        if e['type'] == 'file':
                            remote_sizes[f'{sub}/{n}'] = e['size']
    stats = transfer(job, console, remote, app, remote_sizes, connections)
    result['upload'] = result['readback'] = 'PASS'
    log('PASS', f'{len(app["files"])} files on the console match the PC by SHA-256 '
                f'({stats["sent"]} uploaded and read back, {stats["kept"]} already identical; '
                f'{stats["rate"]:.1f} MiB/s on the wire)')
    with console.ftp() as f:
        # chmod: the loader runs the payload un-sandboxed; it reads the request file written here.
        log('STEP', 'setting mode 0777 under ' + remote + ' (chmod payload through the ELF loader)')
        if console.listdir(f, CHMOD_REQUEST_DIR) is None:
            console.mkdir(f, CHMOD_REQUEST_DIR)
        console.write(f, CHMOD_REQUEST, (tid + '\n').encode())
    reply = console.send_elf(chmod_elf)
    for line in reply.splitlines():
        log('OUT', line)
    m = re.search(r'rcomp_chmod: (\S+) fixed=(\d+) failed=(\d+)', reply)
    if not m:
        result['chmod'] = 'FAIL'
        raise Failed('the chmod payload gave no result line (is the ELF loader on 9021 alive?)')
    if m.group(1) != remote or m.group(3) != '0':
        result['chmod'] = 'FAIL'
        raise Failed(f'chmod payload: {m.group(0)}')
    with console.ftp() as f:
        top = console.listdir(f, remote) or {}
        mode = (top.get('eboot.bin') or {}).get('mode', '')
    if mode and not mode.endswith('777'):
        result['chmod'] = 'FAIL'
        raise Failed(f'eboot.bin mode is {mode} after the payload, expected 0777')
    result['chmod'] = 'PASS'
    log('PASS', f'chmod: {m.group(2)} entries set to 0777' + (f' (eboot.bin mode {mode} over FTP)' if mode else ''))

    if not register:
        result['registration'] = 'NOT TESTED'
        return 'NOT TESTED', f'{tid} installed and verified; ShadowMount+ registration skipped', result
    return register_title(job, console, tid, name, wait_seconds, result)


def register_title(job, console, tid, name, wait_seconds=90, result=None):
    log, cancel = job.log, job.check_cancel
    result = result if result is not None else {'title_id': tid, 'registration': 'NOT TESTED'}
    remote = f'{HOMEBREW}/{tid}'
    log('STEP', 'registering with ShadowMount+ (' + MANUAL_LST + ')')
    with console.ftp() as f:
        if not console.exists_dir(f, remote):
            raise Blocked(f'{remote} does not exist on the console; install the app first')
        raw = console.try_read(f, MANUAL_LST)
        if raw is None:
            raise Blocked(f'{MANUAL_LST} not found: is ShadowMount+ installed on this console?')
        text = raw.decode('utf-8', 'replace')
        lines = [line.strip() for line in text.splitlines()]
        if remote in lines:
            log('INFO', f'{MANUAL_LST} already lists {remote}')
        else:
            backup = f'{MANUAL_LST}.before-rcomp-installer-{time.strftime("%Y%m%d-%H%M%S")}'
            console.write(f, backup, raw)
            if console.read(f, backup) != raw:
                raise Failed(f'backup {backup} did not read back identical; manual.lst left unchanged')
            log('INFO', f'backup: {backup}')
            new = text if text.endswith('\n') or not text else text + '\n'
            new = (new + remote + '\n').encode('utf-8')
            console.write(f, MANUAL_LST, new)
            if console.read(f, MANUAL_LST) != new:
                raise Failed(f'{MANUAL_LST} did not read back as written (backup kept at {backup})')
            log('PASS', f'added "{remote}" to {MANUAL_LST}')
    log('INFO', f'waiting up to {wait_seconds} s for ShadowMount+ to scan')
    end = time.time() + wait_seconds
    status_line, on_system = None, False
    while True:
        cancel()
        with console.ftp(20) as f:
            status = (console.try_read(f, MANUAL_STATUS) or b'').decode('utf-8', 'replace')
            status_line = next((line for line in status.splitlines()
                                if line.split('\t')[1:2] == [tid]), None)
            on_system = console.exists_dir(f, f'/system_ex/app/{tid}')
        if status_line and status_line.startswith('installed\t') and on_system:
            break
        if time.time() > end:
            result['registration'] = 'FAIL'
            raise Failed(f'ShadowMount+ did not confirm {tid} within {wait_seconds} s '
                         f'(manual.status: {status_line!r}, /system_ex/app/{tid}: '
                         f'{"present" if on_system else "absent"})')
        time.sleep(5)
    result['registration'] = 'PASS'
    log('OUT', 'manual.status: ' + status_line.replace('\t', '  '))
    log('PASS', f'/system_ex/app/{tid} exists: "{name}" is on the home screen list')
    return 'PASS', f'{tid} "{name}" installed, verified, chmod 0777 and registered by ShadowMount+', result
