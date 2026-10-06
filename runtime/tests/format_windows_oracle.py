"""Create original formatting vectors using the installed Windows legacy CRT.

The observations characterize Windows msvcrt, not an Xbox firmware dump.
Outputs are fresh files under build/; no runtime source is generated or changed.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('native Windows Python is required')
    root = Path(__file__).resolve().parents[2]
    output = args.out.resolve()
    if not output.is_relative_to(root / 'build') or output.exists():
        parser.error('choose a new directory inside repository build/')
    output.mkdir(parents=True)
    dll_path = Path(os.environ['SystemRoot']) / 'System32/msvcrt.dll'
    crt = c.CDLL(str(dll_path))
    crt.setlocale.argtypes = [c.c_int, c.c_char_p]
    crt.setlocale.restype = c.c_char_p
    locale = crt.setlocale(2, b'.1252')  # LC_CTYPE only; numeric punctuation stays C.
    if not locale:
        raise RuntimeError('CP1252 locale is unavailable')
    numeric_locale = crt.setlocale(4, b'C')
    if numeric_locale != b'C':
        raise RuntimeError('C numeric locale is unavailable')
    function = crt._snprintf
    function.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p]
    function.restype = c.c_int
    randomizer = random.Random(20260929)
    cases = []

    def add(format_string, arguments, count=96):
        converted = []
        encoded = []
        for kind, value in arguments:
            if kind == 's':
                data = value.encode('cp1252')
                converted.append(c.c_char_p(data))
                encoded.append('s:' + data.hex())
            elif kind == 'w':
                converted.append(c.c_wchar_p(value))
                encoded.append('w:' + value.encode('utf-16-be').hex())
            elif kind == 'f':
                converted.append(c.c_double(value))
                encoded.append('u:' + struct.pack('>d', value).hex())
            elif kind == 'i':
                converted.append(c.c_int32(value))
                encoded.append(f'u:{value & 0xFFFFFFFF:016x}')
            elif kind == 'q':
                converted.append(c.c_int64(value))
                encoded.append(f'u:{value & ((1 << 64) - 1):016x}')
            else:
                raise ValueError(kind)
        buffer = (c.c_ubyte * (count + 16))(*([0xA5] * (count + 16)))
        result = function(buffer, count, format_string.encode('ascii'), *converted)
        cases.append((count, format_string.encode('ascii').hex(), ','.join(encoded) or '-',
                      result, bytes(buffer).hex()))

    for count in (0, 1, 4, 5, 6, 10):
        add('ABCDE', [], count)
    add('%+08d|%#08x|%.0d|%#.0o', [('i', -42), ('i', 42), ('i', 0), ('i', 0)])
    add('%*.*s', [('i', 8), ('i', 3), ('s', 'abcdef')])
    add('%*.*s', [('i', -8), ('i', -1), ('s', 'abc')])
    add('%I64d %I64X', [('q', -(1 << 63)), ('q', 0xFEDCBA9876543210)])
    add('%hd|%hhu|%ld', [('i', 0xFFFF), ('i', 0x1FF), ('i', -17)])
    add('%S|%ls|%C', [('w', 'ABC'), ('w', 'é'), ('i', 0x20AC)])
    add('%hs|%.2ls', [('s', 'Hello'), ('w', '€éZ')])
    for value in (-2.5, -1.25, -0.0, 0.0, 1.25, 2.5, 2.35, 9.5, 9.95, 1234.5):
        for format_string in ('%.0f', '%.1f', '%+.2f', '%010.1f', '%e', '%.3E', '%.6g'):
            add(format_string, [('f', value)])
    integer_formats = ('%08X', '%#010x', '%+012d', '% 10d', '%-8.4u', '%.0u', '%#.0o', '%ld')
    for _ in range(400):
        value = randomizer.randrange(-(1 << 31), 1 << 31)
        add(randomizer.choice(integer_formats), [('i', value)], randomizer.randrange(1, 40))
    for _ in range(200):
        value = randomizer.randrange(-(1 << 63), 1 << 63)
        add(randomizer.choice(('%I64d', '%I64u', '%016I64X')), [('q', value)], randomizer.randrange(1, 40))
    for _ in range(300):
        value = randomizer.randrange(-1_000_000, 1_000_000) / (1 << randomizer.randrange(16))
        add(randomizer.choice(('%.0f', '%+.1f', '%10.2f', '%.6f', '%e', '%.3E', '%.6g')),
            [('f', value)], randomizer.randrange(1, 40))
    text = '\n'.join('|'.join(map(str, row)) for row in cases) + '\n'
    (output / 'vectors.txt').write_text(text, encoding='ascii', newline='\n')
    metadata = {'scope': 'Windows legacy msvcrt formatting observations; not Xbox or PS5 proof',
                'dll': str(dll_path), 'dll_sha256': hashlib.sha256(dll_path.read_bytes()).hexdigest(),
                'ctype_locale': locale.decode('ascii', errors='replace'),
                'numeric_locale': numeric_locale.decode('ascii'), 'seed': 20260929,
                'cases': len(cases), 'vector_sha256': hashlib.sha256(text.encode('ascii')).hexdigest(),
                'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    (output / 'METADATA.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
