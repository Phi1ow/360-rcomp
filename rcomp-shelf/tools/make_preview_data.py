#!/usr/bin/env python3
"""R-comp shelf: a stand-in for the console's /data, for the PC preview (build/host/shelf_host --data).

Each --title makes one R-comp title folder, installed (homebrew/<PPSA>/) or packaged (rcomp/packages/<name>/):

  --title PPSA88360:installed:xex=<path to a real image/plain.xex>
  --title PPSA88361:package:synth=4D5307E6:0A1B2C3D:Some test title

A real XEX is not copied: only its header and its XDBF resource are, at their offsets, into a file of the same
size whose other bytes are zero (enough for the catalog; nothing of the game's code). Keep such data under
build/ (git-ignored) and never publish it. A synthetic XEX is made from scratch (title ID, media ID, name).
eboot.bin and game/ are small dummies: the preview never runs them.
"""
import argparse
import json
import pathlib
import struct
import sys


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def sparse_xex(src, dst):
    data = pathlib.Path(src).read_bytes()
    if data[:4] != b'XEX2':
        raise SystemExit(f'{src}: not a XEX2 file')
    head = be32(data, 8)
    keys = {be32(data, 24 + 8 * i): be32(data, 28 + 8 * i) for i in range(be32(data, 20))}
    out = bytearray(len(data))
    out[:head] = data[:head]
    base = keys.get(0x10201)
    res = keys.get(0x2FF)
    if base is not None and res is not None:
        size = be32(data, res)
        for p in range(res + 4, res + size, 16):
            addr, n = be32(data, p + 8), be32(data, p + 12)
            off = head + (addr - base)
            out[off:off + n] = data[off:off + n]
    pathlib.Path(dst).write_bytes(bytes(out))


def xdbf(title, png=b''):
    """An XDBF with XSTC (English default), one English XSTR holding string 0x8000, and image 0x8000."""
    xstc = b'XSTC' + struct.pack('>III', 1, 4, 1)
    t = title.encode('utf-8')
    xstr = b'XSTR' + struct.pack('>IIH', 1, 2 + 4 + len(t), 1) + struct.pack('>HH', 0x8000, len(t)) + t
    blobs = [(1, 0x58535443, xstc), (3, 1, xstr)] + ([(2, 0x8000, png)] if png else [])
    entries, body = b'', b''
    for ns, ident, blob in blobs:
        entries += struct.pack('>HQII', ns, ident, len(body), len(blob))
        body += blob
    return b'XDBF' + struct.pack('>IIIII', 0x10000, len(blobs), len(blobs), 0, 0) + entries + body


def synth_xex(title_id, media_id, name):
    base, head = 0x82000000, 0x1000
    x = xdbf(name)
    res_addr = base + 0x10000
    keys = [(0x2FF, 0x200), (0x3FF, 0x220), (0x10201, base), (0x40006, 0x240)]
    b = bytearray(head + 0x10000 + len(x))
    b[0:4] = b'XEX2'
    struct.pack_into('>IIIII', b, 4, 1, head, 0, 0x100, len(keys))
    for i, (k, v) in enumerate(keys):
        struct.pack_into('>II', b, 24 + 8 * i, k, v)
    struct.pack_into('>I', b, 0x200, 4 + 16)
    b[0x204:0x20C] = f'{title_id:08X}'.encode()
    struct.pack_into('>II', b, 0x20C, res_addr, len(x))
    struct.pack_into('>IHH', b, 0x220, 8, 0, 0)  # file format: no encryption, no compression
    struct.pack_into('>IIII', b, 0x240, media_id, 1, 1, title_id)
    b[0x240 + 18] = 1
    b[0x240 + 19] = 1
    off = head + (res_addr - base)
    b[off:off + len(x)] = x
    return bytes(b)


def param(title_id, name):
    return {
        "applicationCategoryType": 0, "attribute": 0, "conceptId": title_id[4:],
        "contentId": f"UP9000-{title_id}_00-RCOMPPREVIEW0000",
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": name}},
        "titleId": title_id,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--title', action='append', default=[])
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    for spec in a.title:
        ps5, state, src = spec.split(':', 2)
        if state == 'installed':
            folder = out / 'homebrew' / ps5
        elif state == 'package':
            folder = out / 'rcomp' / 'packages' / ps5
        else:
            raise SystemExit(f'{spec}: state is installed or package')
        if folder.exists():
            print(f'{folder}: exists, kept')
            continue
        (folder / 'image').mkdir(parents=True)
        (folder / 'sce_sys').mkdir()
        (folder / 'game').mkdir()
        if src.startswith('xex='):
            sparse_xex(src[4:], folder / 'image' / 'plain.xex')
            name = 'R-comp title'
        elif src.startswith('synth='):
            tid, mid, name = src[6:].split(':', 2)
            (folder / 'image' / 'plain.xex').write_bytes(synth_xex(int(tid, 16), int(mid, 16), name))
        else:
            raise SystemExit(f'{spec}: xex=<path> or synth=<title id>:<media id>:<name>')
        (folder / 'sce_sys' / 'param.json').write_text(json.dumps(param(ps5, name), indent=2) + '\n')
        (folder / 'eboot.bin').write_bytes(b'R-comp shelf preview: not an executable\n' + ps5.encode() + bytes(1 << 20))
        (folder / 'game' / 'data.bin').write_bytes(bytes(3 << 20))
        print(f'{folder}: made ({state})')


if __name__ == '__main__':
    sys.exit(main())
