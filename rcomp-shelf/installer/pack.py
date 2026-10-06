#!/usr/bin/env python3
"""R-comp installer: read a recompiled title's XEX and assemble a PS5 app folder (/data/homebrew/<id>).

No third-party modules. Used by the local web app (server.py) and usable on its own:

  pack.py info   --xex image/plain.xex
  pack.py build  --xex image/plain.xex --eboot eboot.bin --game <dir> --libc libc.prx
                 --icon-template <512 png> --out <dir> [--title-id PPSAxxxxx] [--name "..."]

An app folder holds eboot.bin, sce_sys/param.json, sce_sys/icon0.png, sce_module/libc.prx,
image/plain.xex and game/. The name and icon come from the XEX's XDBF resource unless given.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import struct
import sys
import zlib


def _be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _be16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def read_xex(path):
    """{title_id, media_id, version, name, icon_png} from a decoded plain XEX; name/icon '' when absent."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b'XEX2':
        raise ValueError('not a XEX2 file (give the decoded image/plain.xex)')
    head = _be32(data, 8)
    keys = {_be32(data, 24 + 8 * i): _be32(data, 28 + 8 * i) for i in range(_be32(data, 20))}
    out = {'title_id': '', 'media_id': '', 'version': 0, 'name': '', 'icon_png': b''}
    exe = keys.get(0x40006)
    if exe:
        out['media_id'] = '%08X' % _be32(data, exe)
        out['version'] = _be32(data, exe + 4)
        out['title_id'] = '%08X' % _be32(data, exe + 12)
    base = keys.get(0x10201)
    res = keys.get(0x2FF)
    fmt = keys.get(0x3FF)
    if res and fmt and base is not None and _be16(data, fmt + 4) == 0 and _be16(data, fmt + 6) == 0:
        size = _be32(data, res)
        want = out['title_id']
        for p in range(res + 4, res + size, 16):
            name = data[p:p + 8].split(b'\0')[0].decode('ascii', 'replace')
            addr, n = _be32(data, p + 8), _be32(data, p + 12)
            if name == want and addr >= base and n:
                off = head + (addr - base)
                if off + n <= len(data):
                    out['name'], out['icon_png'] = _read_xdbf(data[off:off + n])
                break
    return out


def _read_xdbf(x):
    if len(x) < 0x18 or x[:4] != b'XDBF':
        return '', b''
    entry_len, entry_count, free_len = _be32(x, 8), _be32(x, 12), _be32(x, 16)
    data = 0x18 + entry_len * 18 + free_len * 8
    default_lang, title, english, png = 1, '', '', b''
    for i in range(entry_count):
        e = 0x18 + i * 18
        ns = _be16(x, e)
        ident = struct.unpack_from('>Q', x, e + 2)[0]
        off = data + _be32(x, e + 10)
        n = _be32(x, e + 14)
        if off + n > len(x):
            continue
        blob = x[off:off + n]
        if ns == 1 and ident == 0x58535443 and len(blob) >= 16 and blob[:4] == b'XSTC':
            default_lang = _be32(blob, 12)
        elif ns == 2 and ident == 0x8000 and blob[:4] == b'\x89PNG':
            png = blob
        elif ns == 3 and len(blob) >= 14 and blob[:4] == b'XSTR':
            cnt = _be16(blob, 12)
            q = 14
            for _ in range(cnt):
                if q + 4 > len(blob):
                    break
                sid, sl = _be16(blob, q), _be16(blob, q + 2)
                s = blob[q + 4:q + 4 + sl].decode('utf-8', 'replace')
                if sid == 0x8000:
                    if ident == default_lang:
                        title = s
                    if ident == 1:
                        english = s
                q += 4 + sl
    return (title or english), png


def _png_size(b):
    if b[:8] == b'\x89PNG\r\n\x1a\n' and b[12:16] == b'IHDR':
        return _be32(b, 16), _be32(b, 20)
    return 0, 0


def _solid_icon(side, title):
    """A plain 512x512 PNG (dark green) when no better icon is available. stdlib zlib only."""
    rows = bytearray()
    for y in range(side):
        rows.append(0)
        t = y / (side - 1)
        r, g, bl = int(14 + 18 * (1 - t)), int(34 + 60 * (1 - t)), int(18 * (1 - t))
        rows += bytes((r, g, bl)) * side
    raw = zlib.compress(bytes(rows), 6)

    def chunk(typ, data):
        return struct.pack('>I', len(data)) + typ + data + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', side, side, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', raw) + chunk(b'IEND', b'')


def make_param(title_id, name, template=None):
    if template and pathlib.Path(template).is_file():
        p = json.loads(pathlib.Path(template).read_text(encoding='utf-8'))
    else:
        p = {'applicationCategoryType': 0, 'attribute': 0, 'applicationDrmType': 'free',
             'contentVersion': '01.000.000', 'masterVersion': '01.00',
             'requiredSystemSoftwareVersion': '0x0000000000000000', 'sdkVersion': '0x0000000000000000'}
    p['titleId'] = title_id
    p['conceptId'] = title_id[4:]
    p['contentId'] = f'UP9000-{title_id}_00-RCOMP{title_id[4:]}00000'[:48].ljust(36, '0')
    p['contentId'] = f'UP9000-{title_id}_00-' + ('RCOMP' + title_id[4:]).ljust(16, '0')[:16]
    p.setdefault('localizedParameters', {})
    p['localizedParameters'] = {'defaultLanguage': 'en-US', 'en-US': {'titleName': name}}
    return p


def build(xex, eboot, game, libc, out, title_id=None, name=None, icon_template=None, param_template=None, icon_png=None):
    info = read_xex(xex)
    tid = (title_id or info['title_id'] or '').upper()
    if not re.fullmatch(r'PPSA\d{5}', tid or ''):
        raise ValueError(f'need a PS5 title id (PPSAxxxxx); got {tid!r}. Pass --title-id.')
    disp = name or info['name'] or tid
    out = pathlib.Path(out)
    if out.exists():
        raise ValueError(f'{out} exists; use a fresh folder')
    (out / 'image').mkdir(parents=True)
    (out / 'sce_sys').mkdir()
    (out / 'sce_module').mkdir()
    shutil.copyfile(eboot, out / 'eboot.bin')
    shutil.copyfile(xex, out / 'image' / 'plain.xex')
    shutil.copyfile(libc, out / 'sce_module' / 'libc.prx')
    if game:
        shutil.copytree(game, out / 'game')
    else:
        (out / 'game').mkdir()
    # Icon: an explicit PNG, else a 512 template, else the XDBF image upscaled by the console, else a plain tile.
    icon = None
    if icon_png and pathlib.Path(icon_png).is_file():
        icon = pathlib.Path(icon_png).read_bytes()
    elif icon_template and pathlib.Path(icon_template).is_file():
        icon = pathlib.Path(icon_template).read_bytes()
    elif info['icon_png']:
        icon = info['icon_png']
    if not icon:
        icon = _solid_icon(512, disp)
    (out / 'sce_sys' / 'icon0.png').write_bytes(icon)
    (out / 'sce_sys' / 'param.json').write_text(json.dumps(make_param(tid, disp, param_template), indent=2) + '\n', encoding='utf-8')
    files = sorted(p for p in out.rglob('*') if p.is_file() and p.name != 'manifest.sha256')
    lines = []
    for p in files:
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        lines.append(f'{h}  ./{p.relative_to(out).as_posix()}')
    (out / 'manifest.sha256').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    iw, ih = _png_size(icon)
    return {'title_id': tid, 'name': disp, 'media_id': info['media_id'], 'version': info['version'],
            'out': str(out), 'files': len(files) + 1, 'icon': f'{iw}x{ih}', 'icon_from': (
                'given' if icon_png else 'template' if icon_template else 'xex' if info['icon_png'] else 'placeholder')}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('info')
    i.add_argument('--xex', required=True)
    b = sub.add_parser('build')
    for a in ('--xex', '--eboot', '--libc', '--out'):
        b.add_argument(a, required=True)
    b.add_argument('--game')
    b.add_argument('--title-id')
    b.add_argument('--name')
    b.add_argument('--icon-template')
    b.add_argument('--icon-png')
    b.add_argument('--param-template')
    a = ap.parse_args()
    if a.cmd == 'info':
        info = read_xex(a.xex)
        info = dict(info)
        info['icon_png'] = f'{len(info["icon_png"])} bytes' if info['icon_png'] else ''
        print(json.dumps(info, indent=2, ensure_ascii=False))
    else:
        print(json.dumps(build(a.xex, a.eboot, a.game, a.libc, a.out, a.title_id, a.name, a.icon_template,
                               a.param_template, a.icon_png), indent=2, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
