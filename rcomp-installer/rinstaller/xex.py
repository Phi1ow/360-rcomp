"""Read the identity of a decoded Xbox 360 executable (plain XEX2) and its XDBF title metadata.

Only the plaintext headers are read. The decoded image R-comp produces (image/plain.xex) has neither
encryption nor compression, so the XDBF resource (title name, image) can be read straight from the file.
Every offset is bounds-checked: a truncated or foreign file raises XexError, never an IndexError.
"""
import pathlib
import struct

KEY_RESOURCE_INFO = 0x000002FF
KEY_FILE_FORMAT = 0x000003FF
KEY_IMAGE_BASE = 0x00010201
KEY_EXECUTION_INFO = 0x00040006
XSTC_ID = 0x58535443  # 'XSTC'
TITLE_STRING_ID = 0x8000
TITLE_IMAGE_ID = 0x8000
LANGUAGES = {1: 'English', 2: 'Japanese', 3: 'German', 4: 'French', 5: 'Spanish', 6: 'Italian', 7: 'Korean',
             8: 'Chinese (Traditional)', 9: 'Portuguese', 10: 'Chinese (Simplified)', 11: 'Polish', 12: 'Russian'}


class XexError(ValueError):
    pass


def _u32(b, o):
    if o < 0 or o + 4 > len(b):
        raise XexError(f'truncated: u32 at 0x{o:X} beyond 0x{len(b):X}')
    return struct.unpack_from('>I', b, o)[0]


def _u16(b, o):
    if o < 0 or o + 2 > len(b):
        raise XexError(f'truncated: u16 at 0x{o:X} beyond 0x{len(b):X}')
    return struct.unpack_from('>H', b, o)[0]


def _u8(b, o):
    if o < 0 or o >= len(b):
        raise XexError(f'truncated: u8 at 0x{o:X}')
    return b[o]


def read_xex(path_or_bytes):
    """Return a dict describing the XEX: ids, disc, plain flag, XDBF title names and image.

    Keys: title_id ('545407F2'), media_id, version, base_version, disc_number, disc_count, image_base,
    encryption, compression, plain (bool), name (best title name, '' if none), names ({language: name}),
    default_language, icon_png (bytes, b'' if none), xdbf (str status), warnings (list).
    """
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else pathlib.Path(path_or_bytes).read_bytes()
    if data[:4] != b'XEX2':
        raise XexError('not a XEX2 file (expected the decoded image/plain.xex)')
    pe_off = _u32(data, 8)
    count = _u32(data, 20)
    if count > 4096 or 24 + 8 * count > len(data):
        raise XexError(f'implausible optional header count {count}')
    keys = {}
    for i in range(count):
        keys[_u32(data, 24 + 8 * i)] = _u32(data, 28 + 8 * i)

    info = {'title_id': '', 'media_id': '', 'version': 0, 'base_version': 0, 'disc_number': 0, 'disc_count': 0,
            'image_base': None, 'encryption': None, 'compression': None, 'plain': False, 'name': '', 'names': {},
            'default_language': None, 'icon_png': b'', 'xdbf': 'absent', 'warnings': [], 'pe_offset': pe_off,
            'size': len(data)}
    exe = keys.get(KEY_EXECUTION_INFO)
    if exe is None:
        info['warnings'].append('no execution info header (0x40006): title id unknown')
    else:
        info['media_id'] = '%08X' % _u32(data, exe)
        info['version'] = _u32(data, exe + 4)
        info['base_version'] = _u32(data, exe + 8)
        info['title_id'] = '%08X' % _u32(data, exe + 12)
        info['disc_number'] = _u8(data, exe + 18)
        info['disc_count'] = _u8(data, exe + 19)
    if KEY_IMAGE_BASE in keys:
        info['image_base'] = keys[KEY_IMAGE_BASE]
    fmt = keys.get(KEY_FILE_FORMAT)
    if fmt is not None:
        info['encryption'] = _u16(data, fmt + 4)
        info['compression'] = _u16(data, fmt + 6)
        info['plain'] = info['encryption'] == 0 and info['compression'] == 0
    if not info['plain']:
        info['warnings'].append('image is encrypted or compressed: give the decoded image/plain.xex '
                                f'(encryption={info["encryption"]}, compression={info["compression"]})')

    res = keys.get(KEY_RESOURCE_INFO)
    if res is not None and info['plain'] and info['image_base'] is not None and info['title_id']:
        total = _u32(data, res)
        found = False
        for p in range(res + 4, res + total - 15, 16):
            name = bytes(data[p:p + 8]).split(b'\0')[0].decode('ascii', 'replace')
            addr, size = _u32(data, p + 8), _u32(data, p + 12)
            if name.upper() != info['title_id']:
                continue
            found = True
            off = pe_off + (addr - info['image_base'])
            if addr < info['image_base'] or size == 0 or off + size > len(data):
                info['xdbf'] = 'out of bounds'
                info['warnings'].append(f'XDBF resource {name} lies outside the file')
                break
            try:
                _read_xdbf(bytes(data[off:off + size]), info)
                info['xdbf'] = 'read'
            except XexError as e:
                info['xdbf'] = 'invalid'
                info['warnings'].append(f'XDBF resource unreadable: {e}')
            break
        if not found:
            info['warnings'].append('no XDBF resource named after the title id')
    names = info['names']
    lang = info['default_language']
    info['name'] = names.get(LANGUAGES.get(lang, ''), '') or names.get('English', '') or next(iter(names.values()), '')
    return info


def _read_xdbf(x, info):
    if x[:4] != b'XDBF':
        raise XexError('bad XDBF magic')
    entry_len, entry_count, free_len = _u32(x, 8), _u32(x, 12), _u32(x, 16)
    if entry_count > entry_len or entry_len > 100000:
        raise XexError('implausible XDBF entry table')
    base = 0x18 + entry_len * 18 + free_len * 8
    for i in range(entry_count):
        e = 0x18 + i * 18
        ns = _u16(x, e)
        ident = struct.unpack_from('>Q', x, e + 2)[0]
        off, size = base + _u32(x, e + 10), _u32(x, e + 14)
        if off + size > len(x):
            info['warnings'].append(f'XDBF entry ns={ns} id=0x{ident:X} out of bounds')
            continue
        blob = x[off:off + size]
        if ns == 1 and ident == XSTC_ID and blob[:4] == b'XSTC':
            info['default_language'] = _u32(blob, 12)
        elif ns == 2 and ident == TITLE_IMAGE_ID and blob[:8] == b'\x89PNG\r\n\x1a\n':
            info['icon_png'] = blob
        elif ns == 3 and blob[:4] == b'XSTR':
            n = _u16(blob, 12)
            q = 14
            for _ in range(n):
                sid, sl = _u16(blob, q), _u16(blob, q + 2)
                if q + 4 + sl > len(blob):
                    break
                if sid == TITLE_STRING_ID:
                    text = blob[q + 4:q + 4 + sl].decode('utf-8', 'replace').strip('\0').strip()
                    if text:
                        info['names'][LANGUAGES.get(ident, f'language {ident}')] = text
                q += 4 + sl


def summary(info):
    """JSON-friendly view (the PNG replaced by its size)."""
    out = {k: v for k, v in info.items() if k != 'icon_png'}
    out['icon_png_bytes'] = len(info.get('icon_png') or b'')
    if out.get('image_base') is not None:
        out['image_base'] = '0x%08X' % out['image_base']
    return out
