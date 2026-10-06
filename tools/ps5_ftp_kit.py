#!/usr/bin/env python3
"""Copies one PS5 test kit folder to the console over FTP and fetches the log.

Runs on the PC that sees the console (standard library only, Windows/Linux/macOS).
Nothing is guessed: host and FTP port are arguments. It only ever writes or
deletes under /data/homebrew/<TITLE_ID>/.

  python3 ps5_ftp_kit.py push  --host H --port P <kit>/8-xex-title/PPSA88360
  (launch the title PPSA88360 on the console, wait for it to finish)
  python3 ps5_ftp_kit.py pull  --host H --port P --out logs/8-xex-title

push: deletes /data/homebrew/<ID>/ (whole folder, previous test), uploads the
folder, then checks every file size on the console and every sha256 of the
folder's manifest.sha256 locally. pull: downloads rcomp_title.log (from
/data/homebrew/<ID>/, else /app0/) and prints the verdict line.
"""
import argparse
import ftplib
import hashlib
import os
import posixpath
import re
import sys

REMOTE_ROOT = "/data/homebrew"


def connect(a):
    ftp = ftplib.FTP()
    ftp.connect(a.host, a.port, timeout=30)
    ftp.login(a.user, a.password)
    ftp.set_pasv(not a.active)
    return ftp


def is_dir(ftp, path):
    cur = ftp.pwd()
    try:
        ftp.cwd(path)
        return True
    except ftplib.error_perm:
        return False
    finally:
        ftp.cwd(cur)


def remove_tree(ftp, path):
    assert path.startswith(REMOTE_ROOT + "/PPSA") and ".." not in path, path
    names = []
    ftp.retrlines("NLST " + path, names.append)
    for n in names:
        n = posixpath.basename(n.rstrip("/"))
        if n in (".", ".."):
            continue
        p = posixpath.join(path, n)
        if is_dir(ftp, p):
            remove_tree(ftp, p)
        else:
            ftp.delete(p)
    ftp.rmd(path)


def check_manifest(local):
    man = os.path.join(local, "manifest.sha256")
    if not os.path.exists(man):
        return
    bad = []
    for line in open(man):
        digest, name = line.split(None, 1)
        name = name.strip().lstrip("*")
        with open(os.path.join(local, name), "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != digest:
                bad.append(name)
    if bad:
        sys.exit("local folder does not match its manifest.sha256: " + ", ".join(bad))


def push(a):
    local = os.path.abspath(a.folder)
    title = os.path.basename(local)
    if not re.fullmatch(r"PPSA\d{5}", title):
        sys.exit(f"{local}: the folder must be named after the title id (PPSAnnnnn)")
    if not os.path.exists(os.path.join(local, "eboot.bin")):
        sys.exit(f"{local}: no eboot.bin")
    check_manifest(local)
    remote = f"{REMOTE_ROOT}/{title}"
    ftp = connect(a)
    if is_dir(ftp, remote):
        print(f"deleting previous {remote}")
        remove_tree(ftp, remote)
    files = []
    for dirpath, _, filenames in os.walk(local):
        rel = os.path.relpath(dirpath, local).replace(os.sep, "/")
        rdir = remote if rel == "." else f"{remote}/{rel}"
        try:
            ftp.mkd(rdir)
        except ftplib.error_perm:
            pass
        for fn in sorted(filenames):
            lp, rp = os.path.join(dirpath, fn), f"{rdir}/{fn}"
            with open(lp, "rb") as f:
                ftp.storbinary("STOR " + rp, f)
            files.append((lp, rp))
            print(f"  {rp} ({os.path.getsize(lp)} bytes)")
    ftp.voidcmd("TYPE I")
    bad = [rp for lp, rp in files if ftp.size(rp) != os.path.getsize(lp)]
    # The system refuses to exec an eboot.bin without the execute bit
    # (processSpawn 0x80aa001a, errno 13). zftpd writes 0666 and answers
    # "200 CHMOD command successful" without changing anything, so the mode is
    # read back from LIST instead of trusting the reply.
    for _, rp in files:
        try:
            ftp.voidcmd(f"SITE CHMOD 777 {rp}")
        except (ftplib.error_perm, ftplib.error_reply, ftplib.error_temp):
            break
    listing = []
    ftp.retrlines("LIST " + remote, listing.append)
    eboot_mode = next((l.split()[0] for l in listing if l.split()[-1:] == ["eboot.bin"]), "")
    if eboot_mode[3:4] == "x":
        chmod = f"eboot.bin is {eboot_mode}"
    else:
        chmod = (f"WARNING: eboot.bin is {eboot_mode or '?'} (no execute bit, the FTP server "
                 "cannot chmod); send tools/ps5_chmod_title to the ELF loader before launching "
                 "(see docs/PS5_TEST_KIT.md)")
    ftp.quit()
    if bad:
        sys.exit("size mismatch after upload: " + ", ".join(bad))
    print(f"OK: {len(files)} files in {remote}, sizes checked; {chmod}. Launch {title} on the console now.")


def pull(a):
    os.makedirs(a.out, exist_ok=True)
    ftp = connect(a)
    dest = os.path.join(a.out, "rcomp_title.log")
    for src in (f"{REMOTE_ROOT}/{a.title}/rcomp_title.log", "/app0/rcomp_title.log"):
        try:
            with open(dest, "wb") as f:
                ftp.retrbinary("RETR " + src, f.write)
            print(f"downloaded {src} -> {dest}")
            break
        except ftplib.error_perm:
            continue
    else:
        ftp.quit()
        os.remove(dest)
        sys.exit("no rcomp_title.log on the console (title not started, or it crashed before opening its log)")
    ftp.quit()
    text = open(dest, errors="replace").read()
    last = text[text.rfind("RCOMP-TITLE begin"):] if "RCOMP-TITLE begin" in text else text
    ends = re.findall(r"RCOMP-TITLE end status=(\d+)", last)
    print("verdict: " + (f"end status={ends[-1]}" if ends else "NO END LINE (the title stopped before the end)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["push", "pull"])
    ap.add_argument("folder", nargs="?", help="push: kit folder N-name/PPSAnnnnn")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--user", default="anonymous")
    ap.add_argument("--password", default="")
    ap.add_argument("--active", action="store_true", help="active FTP mode instead of passive")
    ap.add_argument("--title", default="PPSA88360", help="pull: title id")
    ap.add_argument("--out", default="ps5-logs", help="pull: local directory")
    a = ap.parse_intermixed_args()
    if a.command == "push":
        if not a.folder:
            ap.error("push needs the kit folder")
        push(a)
    else:
        pull(a)


if __name__ == "__main__":
    main()
