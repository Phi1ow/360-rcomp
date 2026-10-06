"""Minimal PNG decode / resize / encode with zlib only, for the 512x512 home-screen icon.

decode() supports non-interlaced 8-bit grayscale, RGB, palette, gray+alpha and RGBA (16-bit samples are
reduced to their high byte). Anything else raises PngError, and the caller falls back to a generated tile.
"""
import struct
import zlib

SIG = b'\x89PNG\r\n\x1a\n'


class PngError(ValueError):
    pass


def size(png):
    if png[:8] == SIG and png[12:16] == b'IHDR':
        return struct.unpack('>II', png[16:24])
    return 0, 0


def decode(png):
    """Return (width, height, bytearray RGBA)."""
    if png[:8] != SIG:
        raise PngError('not a PNG')
    pos, ihdr, plte, trns, idat = 8, None, None, None, bytearray()
    while pos + 8 <= len(png):
        n, typ = struct.unpack('>I4s', png[pos:pos + 8])
        body = png[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
    if not ihdr:
        raise PngError('no IHDR')
    w, h, depth, ctype, _, _, interlace = ihdr
    if interlace or depth not in (8, 16) or ctype not in (0, 2, 3, 4, 6) or (ctype == 3 and depth != 8):
        raise PngError(f'unsupported PNG (depth {depth}, color type {ctype}, interlace {interlace})')
    if w * h > 4096 * 4096:
        raise PngError('PNG too large')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = channels * depth // 8
    stride = w * bpp
    raw = zlib.decompress(bytes(idat))
    if len(raw) < h * (stride + 1):
        raise PngError('truncated image data')
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif f != 0:
            raise PngError(f'bad filter {f}')
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    step = depth // 8
    o = 0
    for line in rows:
        for x in range(w):
            s = [line[(x * channels + c) * step] for c in range(channels)]
            if ctype == 0:
                px = (s[0], s[0], s[0], 255)
                if trns and len(trns) >= 2 and struct.unpack('>H', trns[:2])[0] >> (8 if depth == 16 else 0) == s[0]:
                    px = (s[0], s[0], s[0], 0)
            elif ctype == 2:
                px = (s[0], s[1], s[2], 255)
            elif ctype == 3:
                i = s[0]
                if plte is None or 3 * i + 3 > len(plte):
                    raise PngError('palette index out of range')
                px = (plte[3 * i], plte[3 * i + 1], plte[3 * i + 2], trns[i] if trns and i < len(trns) else 255)
            elif ctype == 4:
                px = (s[0], s[0], s[0], s[1])
            else:
                px = tuple(s)
            out[o:o + 4] = bytes(px)
            o += 4
    return w, h, out


def encode_rgb(w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(typ, body):
        return struct.pack('>I', len(body)) + typ + body + struct.pack('>I', zlib.crc32(typ + body) & 0xffffffff)

    return SIG + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')


def _background(side, top=(28, 52, 40), bottom=(8, 14, 12)):
    rgb = bytearray(side * side * 3)
    for y in range(side):
        t = y / (side - 1)
        px = bytes(int(top[c] + (bottom[c] - top[c]) * t) for c in range(3))
        rgb[y * side * 3:(y + 1) * side * 3] = px * side
    return rgb


def fit_icon(png, side=512, margin=40):
    """Scale an image (bilinear, aspect kept) onto a side x side tile; alpha composed on the tile."""
    w, h, rgba = decode(png)
    rgb = _background(side)
    inner = side - 2 * margin
    scale = min(inner / w, inner / h)
    dw, dh = max(1, round(w * scale)), max(1, round(h * scale))
    ox, oy = (side - dw) // 2, (side - dh) // 2
    for y in range(dh):
        sy = min(max((y + 0.5) / scale - 0.5, 0), h - 1)
        y0 = int(sy)
        y1 = min(y0 + 1, h - 1)
        fy = sy - y0
        for x in range(dw):
            sx = min(max((x + 0.5) / scale - 0.5, 0), w - 1)
            x0 = int(sx)
            x1 = min(x0 + 1, w - 1)
            fx = sx - x0
            i00, i01, i10, i11 = (y0 * w + x0) * 4, (y0 * w + x1) * 4, (y1 * w + x0) * 4, (y1 * w + x1) * 4
            px = []
            for c in range(4):
                top = rgba[i00 + c] * (1 - fx) + rgba[i01 + c] * fx
                bot = rgba[i10 + c] * (1 - fx) + rgba[i11 + c] * fx
                px.append(top * (1 - fy) + bot * fy)
            a = px[3] / 255
            d = ((oy + y) * side + ox + x) * 3
            for c in range(3):
                rgb[d + c] = int(px[c] * a + rgb[d + c] * (1 - a) + 0.5)
    return encode_rgb(side, side, rgb)


# 5x7 bitmap font for the generated tile (upper case, digits, a few signs).
_FONT = {
    'A': '01110100011000111111100011000110001', 'B': '11110100011000111110100011000111110',
    'C': '01110100011000010000100001000101110', 'D': '11100100101000110001100011001011100',
    'E': '11111100001000011110100001000011111', 'F': '11111100001000011110100001000010000',
    'G': '01110100011000010111100011000101111', 'H': '10001100011000111111100011000110001',
    'I': '01110001000010000100001000010001110', 'J': '00111000100001000010000101001001100',
    'K': '10001100101010011000101001001010001', 'L': '10000100001000010000100001000011111',
    'M': '10001110111010110101100011000110001', 'N': '10001100011100110101100111000110001',
    'O': '01110100011000110001100011000101110', 'P': '11110100011000111110100001000010000',
    'Q': '01110100011000110001101011001001101', 'R': '11110100011000111110101001001010001',
    'S': '01111100001000001110000010000111110', 'T': '11111001000010000100001000010000100',
    'U': '10001100011000110001100011000101110', 'V': '10001100011000110001100010101000100',
    'W': '10001100011000110101101011010101010', 'X': '10001100010101000100010101000110001',
    'Y': '10001100010101000100001000010000100', 'Z': '11111000010001000100010001000011111',
    '0': '01110100011001110101110011000101110', '1': '00100011000010000100001000010001110',
    '2': '01110100010000100010001000100011111', '3': '11111000100010000010000011000101110',
    '4': '00010001100101010010111110001000010', '5': '11111100001111000001000011000101110',
    '6': '00110010001000011110100011000101110', '7': '11111000010001000100010000100001000',
    '8': '01110100011000101110100011000101110', '9': '01110100011000101111000010001001100',
    ':': '00000011000110000000011000110000000', '-': '00000000000000011111000000000000000',
    '.': '00000000000000000000000000110001100', "'": '00100001000100000000000000000000000',
    '&': '01100100101010001000101011001001101', '!': '00100001000010000100001000000000100',
    '?': '01110100010000100010001000000000100', ' ': '0' * 35,
}


def _wrap(text, width):
    width = max(1, width)
    lines, cur = [], ''
    for word in text.split():
        while len(word) > width:
            if cur:
                lines.append(cur)
                cur = ''
            lines.append(word[:width])
            word = word[width:]
        if not cur:
            cur = word
        elif len(cur) + 1 + len(word) <= width:
            cur += ' ' + word
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def text_tile(title, footer='R-COMP', side=512):
    """A generated icon: dark green gradient with the title in a block font."""
    rgb = _background(side)

    def draw(text, cx, y, scale, color):
        x = cx - (len(text) * 6 * scale - scale) // 2
        for ch in text:
            bits = _FONT.get(ch, _FONT['?'])
            for r in range(7):
                for c in range(5):
                    if bits[r * 5 + c] == '1':
                        for dy in range(scale):
                            row = (y + r * scale + dy) * side
                            for dx in range(scale):
                                px = x + c * scale + dx
                                if 0 <= px < side and 0 <= y + r * scale + dy < side:
                                    rgb[(row + px) * 3:(row + px) * 3 + 3] = bytes(color)
            x += 6 * scale

    clean = ''.join(ch if ch.upper() in _FONT else ' ' for ch in title.upper()).strip() or 'UNTITLED'
    for scale in (8, 7, 6, 5, 4, 3, 2, 1):
        lines = _wrap(clean, (side - 48) // (6 * scale))
        if len(lines) * 9 * scale <= side - 140:
            break
    lines = lines[:max(1, (side - 140) // (9 * scale))]
    y = (side - 60 - len(lines) * 9 * scale) // 2
    for line in lines:
        draw(line, side // 2, y, scale, (236, 244, 238))
        y += 9 * scale
    draw(footer, side // 2, side - 56, 3, (110, 200, 140))
    return encode_rgb(side, side, rgb)
