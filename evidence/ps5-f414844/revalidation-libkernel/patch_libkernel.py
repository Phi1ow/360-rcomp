#!/usr/bin/env python3
"""Diagnostic only: copies a kit title folder and renames its import module
libkernel_web -> libkernel in eboot.bin (same string-table offsets, same
module version). This is byte-for-byte what linking libkernel.so instead of
libkernel_web.so changes in the import tables; the kit itself is untouched.
The fake-SELF segment digests of the kit are all zero, so nothing else changes.

  python patch_libkernel.py <kit>/1-platform <out>/1-platform
"""
import hashlib, os, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
shutil.copytree(src, dst)
root = os.path.join(dst, "PPSA88360")
old = b"libkernel_web.prx\x00libkernel_web\x00"
new = b"libkernel.prx".ljust(18, b"\x00") + b"libkernel".ljust(14, b"\x00")
p = os.path.join(root, "eboot.bin")
d = open(p, "rb").read()
assert d.count(old) == 1, d.count(old)
open(p, "wb").write(d.replace(old, new))
lines = []
for line in open(os.path.join(root, "manifest.sha256")):
    _, name = line.split(None, 1)
    name = name.strip()
    data = open(os.path.join(root, name.lstrip("*")), "rb").read()
    lines.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
open(os.path.join(root, "manifest.sha256"), "w", newline="\n").write("".join(lines))
print(dst, hashlib.sha256(open(p, "rb").read()).hexdigest())
