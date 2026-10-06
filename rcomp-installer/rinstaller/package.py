"""Assemble and validate a PS5 app folder for a recompiled title.

Layout (what /data/homebrew/<PPSA id>/ holds once installed):
  eboot.bin, sce_sys/param.json, sce_sys/icon0.png, sce_module/libc.prx, image/plain.xex, game/...,
  manifest.sha256 ("<sha256>  ./<relative path>" for every other file).
"""
import hashlib
import json
import os
import pathlib
import re
import shutil

from . import png, xex
from .jobs import Blocked, Failed

TITLE_ID_RE = re.compile(r'PPSA\d{5}')
CONTENT_ID_RE = re.compile(r'[A-Z]{2}\d{4}-(PPSA\d{5})_00-[A-Z0-9]{16}')
REQUIRED = ('eboot.bin', 'sce_sys/param.json', 'sce_sys/icon0.png', 'sce_module/libc.prx')
MARKER = '.rcomp-installer'  # written in folders this tool assembles; only those may be replaced
CHUNK = 4 << 20


def content_id(title_id):
    return f'UP9000-{title_id}_00-' + ('RCOMP' + title_id[4:]).ljust(16, '0')


def make_param(title_id, name, version='01.000.000'):
    """param.json in the shape of the R-comp titles already running on the console."""
    return {
        'ageLevel': {'default': 0},
        'applicationCategoryType': 0,
        'applicationDrmType': 'free',
        'attribute': 0, 'attribute2': 0, 'attribute3': 0,
        'conceptId': title_id[4:],
        'contentBadgeType': 1,
        'contentId': content_id(title_id),
        'contentVersion': version,
        'downloadDataSize': 0,
        'gameIntent': {'permittedIntents': [{'intentType': 'launchActivity'}]},
        'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': name}},
        'masterVersion': '01.00',
        'requiredSystemSoftwareVersion': '0x0000000000000000',
        'sdkVersion': '0x0000000000000000',
        'titleId': title_id,
        'versionFileUri': '',
    }


def check_param(p):
    """Return a list of problems (empty when param.json is valid for an installable title)."""
    errors = []
    tid = p.get('titleId')
    if not isinstance(tid, str) or not TITLE_ID_RE.fullmatch(tid):
        errors.append(f'titleId must be PPSA + 5 digits (got {tid!r})')
        return errors
    if p.get('conceptId') != tid[4:]:
        errors.append(f'conceptId must be {tid[4:]!r} (got {p.get("conceptId")!r})')
    m = CONTENT_ID_RE.fullmatch(str(p.get('contentId', '')))
    if not m or m.group(1) != tid:
        errors.append(f'contentId must look like {content_id(tid)} (got {p.get("contentId")!r})')
    loc = p.get('localizedParameters') or {}
    lang = loc.get('defaultLanguage')
    if not lang or not isinstance(loc.get(lang), dict) or not loc[lang].get('titleName'):
        errors.append('localizedParameters needs defaultLanguage and that language\'s titleName')
    return errors


def title_name(p):
    loc = p.get('localizedParameters') or {}
    return (loc.get(loc.get('defaultLanguage', ''), {}) or {}).get('titleName', '')


def sha256_file(path, cancel=None):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(CHUNK):
            if cancel:
                cancel()
            h.update(chunk)
    return h.hexdigest()


def list_files(root):
    """Sorted (relative posix path, absolute Path, size) of every regular file; refuses links."""
    root = pathlib.Path(root)
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            p = pathlib.Path(dirpath) / name
            if p.is_symlink():
                raise Blocked(f'symbolic link not allowed in an app folder: {p}')
            out.append((p.relative_to(root).as_posix(), p, p.stat().st_size))
    return out


def read_manifest(path):
    entries = {}
    for n, line in enumerate(pathlib.Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip():
            continue
        m = re.fullmatch(r'([0-9a-fA-F]{64}) [ *]?(.+)', line.strip())
        if not m:
            raise Failed(f'manifest.sha256 line {n} is malformed')
        rel = m.group(2)
        rel = rel[2:] if rel.startswith('./') else rel
        entries[rel] = m.group(1).lower()
    return entries


def inspect_app(folder, log, cancel, verify_hashes=True):
    """Validate an app folder; return {title_id, name, files: [(rel, path, size, sha)], bytes}."""
    folder = pathlib.Path(folder)
    if not folder.is_dir():
        raise Blocked(f'not a folder: {folder}')
    missing = [r for r in REQUIRED if not (folder / r).is_file()]
    if missing:
        raise Blocked('not an app folder, missing: ' + ', '.join(missing))
    try:
        param = json.loads((folder / 'sce_sys/param.json').read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        raise Blocked(f'sce_sys/param.json unreadable: {e}')
    problems = check_param(param)
    if problems:
        raise Blocked('sce_sys/param.json: ' + '; '.join(problems))
    tid, name = param['titleId'], title_name(param)
    w, h = png.size((folder / 'sce_sys/icon0.png').read_bytes()[:32])
    if (w, h) != (512, 512):
        log('WARN', f'sce_sys/icon0.png is {w}x{h}, the home screen expects 512x512')
    if not (folder / 'image/plain.xex').is_file():
        log('WARN', 'image/plain.xex missing: an R-comp title reads it from /app0/image/plain.xex')
    if not (folder / 'game').is_dir():
        log('WARN', 'game/ missing: an R-comp title reads its files from /app0/game')
    files = [f for f in list_files(folder) if f[0] != MARKER]
    total = sum(s for _, _, s in files)
    log('INFO', f'{tid} "{name}": {len(files)} files, {total / 2**20:.1f} MiB')
    manifest = folder / 'manifest.sha256'
    expected = read_manifest(manifest) if manifest.is_file() else None
    if expected is None:
        log('WARN', 'no manifest.sha256: local hashes are computed now, the console copy is checked against them')
    hashed = []
    if verify_hashes:
        for rel, p, size in files:
            cancel()
            sha = sha256_file(p, cancel)
            if expected is not None and rel != 'manifest.sha256':
                if rel not in expected:
                    raise Failed(f'{rel} is not listed in manifest.sha256')
                if expected[rel] != sha:
                    raise Failed(f'{rel} does not match manifest.sha256')
            hashed.append((rel, p, size, sha))
        if expected is not None:
            extra = set(expected) - {r for r, *_ in files}
            if extra:
                raise Failed('manifest.sha256 lists missing files: ' + ', '.join(sorted(extra)[:5]))
            log('PASS', f'local files match manifest.sha256 ({len(expected)} entries)')
    else:
        hashed = [(rel, p, size, None) for rel, p, size in files]
    return {'title_id': tid, 'name': name, 'files': hashed, 'bytes': total, 'param': param}


def _copy_hash(src, dst, cancel):
    h = hashlib.sha256()
    with open(src, 'rb') as fi, open(dst, 'xb') as fo:
        while chunk := fi.read(CHUNK):
            cancel()
            h.update(chunk)
            fo.write(chunk)
    return h.hexdigest()


def choose_icon(info, name, icon_file, log):
    if icon_file:
        data = pathlib.Path(icon_file).read_bytes()
        try:
            out = data if png.size(data) == (512, 512) else png.fit_icon(data)
            log('INFO', f'icon: given PNG {"(512x512 kept)" if out is data else "(fitted to 512x512)"}')
            return out, 'given'
        except png.PngError as e:
            raise Blocked(f'icon file unusable: {e}')
    if info.get('icon_png'):
        try:
            w, h = png.size(info['icon_png'])
            out = png.fit_icon(info['icon_png'])
            log('INFO', f'icon: XEX title image {w}x{h} upscaled onto a 512x512 tile')
            return out, 'xex'
        except png.PngError as e:
            log('WARN', f'XEX title image not decodable ({e}); generating a tile')
    log('INFO', 'icon: generated tile with the title name')
    return png.text_tile(name), 'generated'


def build_app(job, eboot, xex_path, game, libc, out_parent, title_id, name, icon_file=None, replace=False,
              forbidden_roots=(), modules=()):
    """Assemble <out_parent>/<title_id>/; returns the result dict. Raises Blocked/Failed.

    modules: (decoded plain.xex, disc path) of each AOT module (m6_inventory.py modules/<key>/), copied to
    image/modules/<disc path>: the runtime reads an encrypted/compressed disc DLL's image from there
    (runtime_configure_module_images, set by the title to /app0/image/modules)."""
    log, cancel = job.log, job.check_cancel
    for label, p, kind in (('eboot.bin', eboot, 'file'), ('plain.xex', xex_path, 'file'),
                           ('libc.prx', libc, 'file'), ('game folder', game, 'dir')):
        if not p:
            raise Blocked(f'{label} not given')
        if kind == 'file' and not pathlib.Path(p).is_file():
            raise Blocked(f'{label} not found: {p}')
        if kind == 'dir' and not pathlib.Path(p).is_dir():
            raise Blocked(f'{label} not found: {p}')
    for label, p in (('eboot.bin', eboot), ('libc.prx', libc)):
        if pathlib.Path(p).stat().st_size == 0:
            raise Blocked(f'{label} is empty')
    with open(eboot, 'rb') as f:
        magic = f.read(4)
    if magic not in (b'\x7fELF', b'\x4f\x15\x3d\x1d', b'\x54\x14\xf5\xee'):
        log('WARN', 'eboot.bin starts with neither an ELF nor a SELF magic')
    log('STEP', 'reading the XEX')
    info = xex.read_xex(xex_path)
    for w in info['warnings']:
        log('WARN', w)
    if not info['plain']:
        raise Blocked('the XEX is not a decoded plain image; R-comp needs image/plain.xex')
    log('INFO', f'XEX title id {info["title_id"]}, media id {info["media_id"]}, disc {info["disc_number"]}/'
                f'{info["disc_count"]}, name {info["name"]!r}')
    title_id = (title_id or '').strip().upper()
    if not TITLE_ID_RE.fullmatch(title_id):
        raise Blocked(f'PS5 title id must be PPSA + 5 digits (got {title_id!r})')
    name = (name or info['name'] or '').strip()
    if not name:
        raise Blocked('no display name: the XEX has none, enter one')
    out_parent = pathlib.Path(out_parent).resolve()
    out = out_parent / title_id
    for root in forbidden_roots:
        root = pathlib.Path(root).resolve()
        if out == root or root in out.parents:
            raise Blocked(f'output must not be inside {root}')
    for label, p in (('game folder', game), ('eboot.bin', eboot)):
        src = pathlib.Path(p).resolve()
        if out == src or src in out.parents or out in src.parents:
            raise Blocked(f'output {out} overlaps the {label}')
    if out.exists():
        if not replace:
            raise Blocked(f'{out} exists; tick "replace" or choose another output folder')
        if not (out / MARKER).is_file():
            raise Blocked(f'{out} was not assembled by this tool; refusing to delete it')
        log('INFO', f'removing previous assembly {out}')
        shutil.rmtree(out)
    out_parent.mkdir(parents=True, exist_ok=True)
    out.mkdir()
    (out / MARKER).write_text('assembled by R-comp Installer; incomplete until manifest.sha256 exists\n')
    for d in ('sce_sys', 'sce_module', 'image', 'game'):
        (out / d).mkdir()
    records = {}
    log('STEP', 'copying eboot.bin, libc.prx, plain.xex')
    records['eboot.bin'] = _copy_hash(eboot, out / 'eboot.bin', cancel)
    records['sce_module/libc.prx'] = _copy_hash(libc, out / 'sce_module/libc.prx', cancel)
    records['image/plain.xex'] = _copy_hash(xex_path, out / 'image/plain.xex', cancel)
    for module_xex, disc_path in modules:
        parts = pathlib.PurePosixPath(disc_path).parts
        if not parts or any(p in ('', '.', '..') for p in parts) or ':' in disc_path or '\\' in disc_path:
            raise Blocked(f'invalid AOT module disc path {disc_path!r}')
        if not xex.read_xex(module_xex)['plain']:
            raise Blocked(f'AOT module image {module_xex} is not a decoded plain XEX')
        rel = 'image/modules/' + '/'.join(parts)
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        records[rel] = _copy_hash(module_xex, out / rel, cancel)
    if modules:
        log('INFO', f'copied {len(modules)} decoded AOT module image(s) to image/modules/')
    game_files = list_files(game)
    total = sum(s for *_, s in game_files)
    log('STEP', f'copying game/: {len(game_files)} files, {total / 2**20:.1f} MiB')
    seen, done, last = set(), 0, 0
    for rel, p, size in game_files:
        key = rel.casefold()
        if key in seen:
            raise Blocked(f'two game files differ only by case: {rel}')
        seen.add(key)
        dst = out / 'game' / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        records['game/' + rel] = _copy_hash(p, dst, cancel)
        done += size
        if total and (done - last > total / 20 or done == total):
            last = done
            log('INFO', f'  game/ {done * 100 // total}% ({done / 2**20:.0f} MiB)')
    log('STEP', 'writing sce_sys/param.json and sce_sys/icon0.png')
    icon, icon_from = choose_icon(info, name, icon_file, log)
    (out / 'sce_sys/icon0.png').write_bytes(icon)
    param = make_param(title_id, name)
    (out / 'sce_sys/param.json').write_text(json.dumps(param, indent=2) + '\n', encoding='utf-8')
    for rel in ('sce_sys/icon0.png', 'sce_sys/param.json'):
        records[rel] = sha256_file(out / rel)
    problems = check_param(param)
    if problems:
        raise Failed('generated param.json invalid: ' + '; '.join(problems))
    (out / 'manifest.sha256').write_text(''.join(f'{records[r]}  ./{r}\n' for r in sorted(records)),
                                         encoding='utf-8', newline='\n')
    (out / MARKER).write_text('assembled by R-comp Installer\n')
    # The marker is local bookkeeping; it is not part of the app and is never uploaded.
    log('PASS', f'app folder assembled: {out} ({len(records)} files + manifest.sha256)')
    return {'app': str(out), 'title_id': title_id, 'name': name, 'files': len(records) + 1,
            'icon_from': icon_from, 'xex_title_id': info['title_id'], 'media_id': info['media_id']}
