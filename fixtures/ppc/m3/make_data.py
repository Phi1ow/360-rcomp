#!/usr/bin/env python3
"""Writes data/m3.bin and prints the oracle values used in rcomp_m3.s.

Content: 300 bytes, byte i = (i * i + 3) & 0xFF. The program reads 256 bytes
and returns h = fold(h * 31 + b) mod 2^32 over them (h0 = 0). Oracle computed
here, independently of the generated code.
"""
import os
data = bytes((i * i + 3) & 0xFF for i in range(300))
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "data", "m3.bin"), "wb") as f:
    f.write(data)
h = 0
for b in data[:256]:
    h = (h * 31 + b) & 0xFFFFFFFF
print("hash256 = 0x%08X" % h)
print("path bytes =", " ".join("%02x" % b for b in b"game:\\m3.bin"), "len", len(b"game:\\m3.bin"))
