#!/usr/bin/env python3
"""Summarises the stale-page audit of a run (cvar rcomp_stale_audit of the Xenos backend, wave 6).

usage: stale_page_report.py <run directory or rcomp_title.err>...

The title log carries
  RCOMP-STALE-AUDIT calls=N scanned_pages=N suspects=N stale=N        every 64 frames, cumulative
  RCOMP-STALE-PAGE page=0x... frame=N uploaded_hash=... guest_hash=... [head=<32 bytes in hex>]   the first 40 confirmed stale pages
  RCOMP-STALE-VERTEX address=0x... size=0x... invalid_pages=N of N frame=N   the first 20 draws that used a vertex buffer slot without requesting it while pages of its range were invalid
  RCOMP-STALE-VERTEX-COUNT used=N used_invalid=N invalid_pages=N        every 1,048,576 such draws, cumulative
A page is stale when it is valid in the shared memory, was not written by the GPU, carries no pending guest write mark, and its guest bytes are not the bytes the buffer
received, one frame in a row. The first 32 bytes of the page (big-endian guest memory) are classified to tell what kind of data changes behind the tracking's back.
Host analysis of a PS5 log; it proves nothing by itself about the picture.
"""
import collections
import re
import struct
import sys
from pathlib import Path

AUDIT = re.compile(r"RCOMP-STALE-AUDIT calls=(\d+) scanned_pages=(\d+) suspects=(\d+) stale=(\d+)")
PAGE = re.compile(r"RCOMP-STALE-PAGE page=0x([0-9a-f]+) frame=(\d+)(?: uploaded_hash=[0-9a-f]+ guest_hash=[0-9a-f]+)?(?: head=([0-9a-f]+))?")
VERTEX = re.compile(r"RCOMP-STALE-VERTEX address=0x([0-9a-f]+) size=0x([0-9a-f]+) invalid_pages=(\d+) of (\d+) frame=(\d+)")
VERTEX_COUNT = re.compile(r"RCOMP-STALE-VERTEX-COUNT used=(\d+) used_invalid=(\d+) invalid_pages=(\d+)")


def classify(head_hex):
    """A guess at what 32 bytes of big-endian guest memory are: nothing more than a hint."""
    if not head_hex:
        return "no head logged"
    raw = bytes.fromhex(head_hex)
    if not any(raw):
        return "all zero"
    floats = struct.unpack(">8f", raw)
    plausible = sum(1 for f in floats if f == 0.0 or (1e-6 < abs(f) < 1e6))
    shorts = struct.unpack(">16h", raw)
    small = sum(1 for s in shorts if abs(s) < 4096)
    printable = sum(1 for b in raw if 32 <= b < 127)
    words = struct.unpack(">8I", raw)
    pointers = sum(1 for w in words if 0x40000000 <= w < 0x100000000 or w == 0)
    if printable >= 24:
        return "text-like"
    if plausible == 8 and any(f != 0.0 for f in floats):
        return "float data"
    if pointers >= 7:
        return "pointers / small structure"
    if small >= 14:
        return "small 16-bit values (audio-like)"
    return "other"


def report(path):
    err = Path(path)
    if err.is_dir():
        err = err / "rcomp_title.err"
    text = err.read_text(encoding="utf-8", errors="replace")
    print(f"== {err.parent.name}")
    audits = AUDIT.findall(text)
    if audits:
        calls, scanned, suspects, stale = map(int, audits[-1])
        print(f"audit: {calls} calls, {scanned} pages scanned ({scanned / max(calls, 1):.1f} a frame), {suspects} suspects, {stale} confirmed stale sightings")
    else:
        print("audit: no RCOMP-STALE-AUDIT line (cvar off, or the run is too short)")
    sightings = PAGE.findall(text)
    pages = collections.Counter(int(m[0], 16) for m in sightings)
    frames = collections.defaultdict(list)
    heads = {}
    for page, frame, head in sightings:
        frames[int(page, 16)].append(int(frame))
        if head:
            heads[int(page, 16)] = head
    if pages:
        print(f"{len(pages)} distinct pages among the {sum(pages.values())} logged sightings (the log keeps the first 40):")
        kinds = collections.Counter()
        for page, count in sorted(pages.items()):
            fs = frames[page]
            kind = classify(heads.get(page))
            kinds[kind] += 1
            print(f"  page 0x{page:x} (physical 0x{page << 12:08x}, {page * 4096 / 2**20:.1f} MiB): {count} sightings, frames {min(fs)}..{max(fs)}; {kind}"
                  + (f"; head {heads[page][:32]}" if page in heads else ""))
        print("  kinds: " + ", ".join(f"{k}: {n}" for k, n in kinds.most_common()))
    vertex_counts = VERTEX_COUNT.findall(text)
    if vertex_counts:
        used, used_invalid, invalid_pages = map(int, vertex_counts[-1])
        print(f"vertex buffer slots used without a request: {used} draws, {used_invalid} with invalid pages in their range ({invalid_pages} pages)")
    vertex = VERTEX.findall(text)
    for address, size, invalid, total, frame in vertex[:20]:
        print(f"  vertex 0x{address} size 0x{size}: {invalid} of {total} pages invalid at frame {frame}")
    if not vertex_counts and not vertex:
        print("vertex shortcut audit: nothing logged")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for argument in sys.argv[1:]:
        report(argument)
