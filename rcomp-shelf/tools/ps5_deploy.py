#!/usr/bin/env python3
"""R-comp shelf: put the shelf on the console and start it (the same services R-comp's run_title.sh uses).

  tools/ps5_deploy.py push      upload build/ps5/dist/<id>/ to /data/homebrew/<id>/, read every file back (SHA-256),
                                then send the chmod payload (build/ps5/ps5_chmod_title_<id>.elf) to the ELF loader
  tools/ps5_deploy.py launch    start <id> through ps5vkctl (loaded first when it isn't up), refuses when another
                                application runs
  tools/ps5_deploy.py procs     what runs
  tools/ps5_deploy.py kill      close <id> (ps5vkctl refuses any other title)
  tools/ps5_deploy.py log       print /data/rcomp/logs/shelf.log's last lines

Writes only under /data/homebrew/<id>/. Console address and ports: RSHELF_PS5_HOST (required),
RSHELF_FTP_PORT (2120), RSHELF_LOADER_PORT (9021), RSHELF_CTL_PORT (9111). RCOMP_ROOT (../rcomp) holds
ps5vkctl.elf (build/platform-ps5-payloads/), checked against the SHA-256 R-comp's runs use.
"""
import ftplib
import hashlib
import io
import os
import pathlib
import socket
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RCOMP = pathlib.Path(os.environ.get('RCOMP_ROOT', ROOT.parent / 'rcomp')).resolve()
HOST = os.environ.get('RSHELF_PS5_HOST', '')
FTP_PORT = int(os.environ.get('RSHELF_FTP_PORT', '2120'))
LOADER_PORT = int(os.environ.get('RSHELF_LOADER_PORT', '9021'))
CTL_PORT = int(os.environ.get('RSHELF_CTL_PORT', '9111'))
TITLE = os.environ.get('RSHELF_TITLE_ID', 'PPSA88300')
CTL_SHA = '6eaa5dafb8f948c6a8a737353bf47577750c7c4d4c4957a02be4e9aef6f53212'
APP = ROOT / 'build' / 'ps5' / 'dist' / TITLE
REMOTE = '/data/homebrew/' + TITLE


def ftp():
    f = ftplib.FTP()
    f.connect(HOST, FTP_PORT, timeout=20)
    f.login()
    return f


def ensure_dir(f, path):
    try:
        f.mkd(path)
    except ftplib.error_perm:
        pass


def send_elf(path, wait=10):
    with socket.create_connection((HOST, LOADER_PORT), timeout=20) as s:
        s.sendall(pathlib.Path(path).read_bytes())
        s.shutdown(socket.SHUT_WR)
        s.settimeout(wait)
        reply = b''
        try:
            while chunk := s.recv(4096):
                reply += chunk
        except socket.timeout:
            pass
    return reply.decode(errors='replace').strip()


def ctl(cmd, timeout=30):
    with socket.create_connection((HOST, CTL_PORT), timeout=5) as s:
        s.sendall((cmd + '\n').encode())
        s.settimeout(timeout)
        reply = b''
        try:
            while chunk := s.recv(8192):
                reply += chunk
        except socket.timeout:
            pass
    return reply.decode(errors='replace').strip()


def ensure_ctl():
    try:
        return ctl('status', 5)
    except OSError:
        pass
    elf = RCOMP / 'build' / 'platform-ps5-payloads' / 'ps5vkctl.elf'
    if hashlib.sha256(elf.read_bytes()).hexdigest() != CTL_SHA:
        raise SystemExit(f'BLOCKED: {elf} is not the verified ps5vkctl')
    print('loading ps5vkctl:', send_elf(elf, 5) or '(no reply)')
    for _ in range(20):
        try:
            return ctl('status', 5)
        except OSError:
            time.sleep(0.5)
    raise SystemExit('BLOCKED: ps5vkctl did not come up')


def push():
    files = sorted(p for p in APP.rglob('*') if p.is_file())
    if not files:
        raise SystemExit(f'BLOCKED: nothing in {APP}; run tools/build_ps5.sh')
    with ftp() as f:
        ensure_dir(f, REMOTE)
        for p in files:
            rel = p.relative_to(APP).as_posix()
            parts = rel.split('/')[:-1]
            for i in range(len(parts)):
                ensure_dir(f, REMOTE + '/' + '/'.join(parts[:i + 1]))
            with open(p, 'rb') as src:
                f.storbinary(f'STOR {REMOTE}/{rel}', src)
        for p in files:
            rel = p.relative_to(APP).as_posix()
            buf = io.BytesIO()
            f.retrbinary(f'RETR {REMOTE}/{rel}', buf.write)
            if hashlib.sha256(buf.getvalue()).digest() != hashlib.sha256(p.read_bytes()).digest():
                raise SystemExit(f'FAIL: {rel} read back differs')
    print(f'deployed and verified {len(files)} files in {REMOTE}')
    chmod = ROOT / 'build' / 'ps5' / f'ps5_chmod_title_{TITLE}.elf'
    reply = send_elf(chmod)
    print('chmod:', reply)
    if 'failed=0' not in reply:
        raise SystemExit('FAIL: the chmod payload did not report failed=0')


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if not HOST:
        sys.exit('BLOCKED set RSHELF_PS5_HOST to the console\'s address')
    if cmd == 'push':
        push()
    elif cmd == 'procs':
        print(ensure_ctl())
        print(ctl('procs'))
    elif cmd == 'launch':
        ensure_ctl()
        procs = ctl('procs')
        print('before:', procs)
        if 'count=0' not in procs:
            raise SystemExit('BUSY: an application runs; not launching')
        print('launch:', ctl('launch ' + TITLE, 40))
    elif cmd == 'kill':
        ensure_ctl()
        print('kill:', ctl('kill ' + TITLE))
    elif cmd == 'log':
        with ftp() as f:
            buf = io.BytesIO()
            f.retrbinary('RETR /data/rcomp/logs/shelf.log', buf.write)
        print('\n'.join(buf.getvalue().decode(errors='replace').splitlines()[-int(os.environ.get('LINES', '60')):]))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
