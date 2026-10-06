#!/usr/bin/env python3
"""Build original metadata XEX and reject malformed fixture annotations."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'cpu/tests/xex_metadata_original.s'
GENERATOR = ROOT / 'cpu/tools/mkxex.py'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to((ROOT / 'build').resolve()):
        ap.error('--out must be fresh and under build/')
    out.mkdir(parents=True)
    steps = []

    def check(name, ok, **details):
        steps.append(dict(name=name, status='PASS' if ok else 'FAIL', **details))
        (out / 'report.json').write_text(json.dumps(steps, indent=2))
        print(name + ': ' + steps[-1]['status'], flush=True)
        if not ok:
            raise SystemExit(1)

    def generate(name, source, error=None):
        target = out / name
        cmd = [sys.executable, str(GENERATOR), str(source), '--out', str(target)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        ok = p.returncode == 0 if error is None else (
            p.returncode != 0 and error in p.stderr and not target.with_suffix('.xex').exists())
        check(name, ok, command=cmd, exit_code=p.returncode, stdout=p.stdout, stderr=p.stderr)
        return target

    target = generate('original', SOURCE)
    xex = target.with_suffix('.xex').read_bytes()
    meta = json.loads(target.with_suffix('.json').read_text())
    be32 = lambda offset: struct.unpack_from('>I', xex, offset)[0]
    count, header_size = be32(20), be32(8)
    opts = {be32(24 + i * 8): (be32(28 + i * 8), 28 + i * 8) for i in range(count)}
    check('three_inline_values', opts[0xCAFE0000][0] == 0x12345678 and
          opts[0xCAFE0100][0] == 0 and opts[0xCAFE0201][0] == 0xA1B2C3D4)
    check('inline01_address_is_value_word', xex[opts[0xCAFE0201][1]:opts[0xCAFE0201][1] + 4]
          == bytes.fromhex('a1b2c3d4'))
    check('fixed_payload_bytes', xex[opts[0xCAFE0302][0]:opts[0xCAFE0302][0] + 8]
          == bytes.fromhex('13579bdf2468ace0'))
    check('variable_payload_size_and_bytes', xex[opts[0xCAFE04FF][0]:opts[0xCAFE04FF][0] + 12]
          == bytes.fromhex('0000000c1020304050607080'))
    check('system_flags_are_actual_inline_value', opts[0x00030000][0] == 0x00200008)
    check('all_optional_entries_reported', len(meta['optional_headers']) == count and
          {v['key']: v['value'] for v in meta['optional_headers']} == {k: v[0] for k, v in opts.items()})
    image = xex[header_size:]
    le16 = lambda offset: struct.unpack_from('<H', image, offset)[0]
    le32 = lambda offset: struct.unpack_from('<I', image, offset)[0]
    pe = le32(0x3C)
    optional = pe + 24
    check('real_pe32_identity', image[:2] == b'MZ' and image[pe:pe + 4] == b'PE\0\0'
          and le16(pe + 4) == 0x1F2 and le16(pe + 20) == 224 and le16(optional) == 0x10B)
    check('pe_matches_xex_metadata', le32(optional + 28) == meta['base'] and
          le32(optional + 16) + meta['base'] == meta['entry'] and
          le32(optional + 56) == len(image) == meta['image_size'])
    check('pe_alignment_and_directories', le32(optional + 32) == 0x1000 and
          le32(optional + 36) == 0x200 and le32(optional + 60) == 0x1000 and
          le16(optional + 68) == 14 and le32(optional + 92) == 16)
    sec_count = le16(pe + 6)
    sec_table = optional + 224
    section_ranges = []
    for i in range(sec_count):
        pos = sec_table + i * 40
        name = image[pos:pos + 8].rstrip(b'\0').decode()
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from('<IIII', image, pos + 8)
        section_ranges.append((raw_offset, raw_offset + raw_size))
        check('pe_section_' + name, rva == raw_offset and rva % 0x1000 == 0 and
              raw_size % 0x200 == 0 and raw_size >= virtual_size and
              raw_offset + raw_size <= len(image) and
              meta['sections'][name]['size'] == virtual_size)
    section_ranges.sort()
    check('pe_sections_do_not_overlap', all(a[1] <= b[0] for a, b in zip(section_ranges, section_ranges[1:])))

    original = SOURCE.read_text()
    versioned_source = (original + '''
#_ XEX_IMPORT version_kernel_thunk xboxkrnl.exe 0x0084
#_ XEX_IMPORT version_xam_thunk xam.xex 0x0191
#_ XEX_LIBRARY_VERSION xboxkrnl.exe 0x201A1B00 0x201A1B00
#_ XEX_LIBRARY_VERSION xam.xex 0x201A1B00 0x201A1B00
    .text
    .align 2
    .globl version_kernel_thunk
version_kernel_thunk: .space 16
    .globl version_xam_thunk
version_xam_thunk: .space 16
''')
    version_input = out / 'version_input.s'
    version_input.write_text(versioned_source)
    version_output = generate('versioned', version_input)
    version_bytes = version_output.with_suffix('.xex').read_bytes()
    version_meta = json.loads(version_output.with_suffix('.json').read_text())
    read_u32 = lambda offset: struct.unpack_from('>I', version_bytes, offset)[0]
    header = next(read_u32(28 + 8 * i) for i in range(read_u32(20))
                  if read_u32(24 + 8 * i) == 0x103FF)
    cursor = header + 12 + read_u32(header + 4)
    for module in ('xboxkrnl.exe', 'xam.xex'):
        check('library_version_bytes_' + module,
              struct.unpack_from('>II', version_bytes, cursor + 0x1C) == (0x201A1B00, 0x201A1B00)
              and version_meta['library_versions'][module] ==
                  {'version': 0x201A1B00, 'minimum_version': 0x201A1B00})
        cursor += read_u32(cursor)
    check('library_version_header_bounds', cursor == header + read_u32(header))
    bad = {
        'duplicate_library_version': (versioned_source + '\n#_ XEX_LIBRARY_VERSION xam.xex 1 1\n',
                                      'duplicate XEX_LIBRARY_VERSION'),
        'missing_library_version_arg': (versioned_source.replace('xam.xex 0x201A1B00 0x201A1B00',
                                                                 'xam.xex 0x201A1B00'),
                                        'malformed XEX_LIBRARY_VERSION'),
        'negative_library_version': (versioned_source.replace('xam.xex 0x201A1B00 0x201A1B00',
                                                              'xam.xex -1 0'),
                                     'outside 32-bit unsigned'),
        'overflow_library_version': (versioned_source.replace('xam.xex 0x201A1B00 0x201A1B00',
                                                              'xam.xex 0x100000000 0'),
                                     'outside 32-bit unsigned'),
        'inverted_library_versions': (versioned_source.replace('xam.xex 0x201A1B00 0x201A1B00',
                                                               'xam.xex 1 2'),
                                      'minimum library version exceeds version'),
        'unused_library_version': (original + '\n#_ XEX_LIBRARY_VERSION unused.xex 1 1\n',
                                    'XEX_LIBRARY_VERSION has no imports'),
        'duplicate_key': (original + '\n#_ XEX_HEADER_VALUE 0xCAFE0000 1\n', 'duplicate optional header key'),
        'automatic_key_collision': (original + '\n#_ XEX_HEADER_VALUE 0x00010100 1\n', 'collides with automatic key'),
        'inline_data_key': (original.replace('VALUE 0xCAFE0000', 'VALUE 0xCAFE0002'), 'VALUE requires'),
        'data_inline_key': (original.replace('DATA 0xCAFE0302', 'DATA 0xCAFE0301'), 'DATA size does not match'),
        'fixed_wrong_size': (original.replace('header_fixed 8', 'header_fixed 4'), 'DATA size does not match'),
        'ff_too_short': (original.replace('header_variable 12', 'header_variable 0'), 'DATA size does not match'),
        'ff_wrong_prefix': (original.replace('.long 12,', '.long 8,'), 'total BE32 size'),
        'unknown_label': (original.replace('header_fixed 8', 'unknown_header 8'), 'unknown label'),
        'section_overrun': (original.replace('header_variable 12', 'header_variable 4096'), 'fit one image section'),
        'negative_value': (original.replace('0x12345678', '-1'), 'outside 32-bit unsigned'),
        'overflow_value': (original.replace('0x12345678', '0x100000000'), 'outside 32-bit unsigned'),
        'overflow_key': (original.replace('0xCAFE0000', '0x1CAFE0000'), 'outside 32-bit unsigned'),
        'invalid_integer': (original.replace('0x12345678', 'not_an_integer'), 'expected a 32-bit'),
        'extra_argument': (original.replace('0x12345678', '0x12345678 extra'), 'wrong number of arguments'),
        'missing_argument': (original.replace('header_fixed 8', 'header_fixed'), 'wrong number of arguments'),
        'unknown_annotation': (original.replace('XEX_HEADER_VALUE', 'XEX_HEADER_MAGIC', 1), 'malformed XEX_HEADER'),
        'duplicate_entry': (original + '\n#_ XEX_ENTRY start\n', 'duplicate #_ XEX_ENTRY'),
        'data_entry': (original.replace('XEX_ENTRY start', 'XEX_ENTRY metadata_data'), 'aligned instruction in .text'),
        'unknown_entry': (original.replace('XEX_ENTRY start', 'XEX_ENTRY unknown_entry'), 'aligned instruction in .text'),
    }
    for name, (content, error) in bad.items():
        source = out / (name + '.s')
        source.write_text(content)
        generate(name, source, error)
    print(f'PASS {len(steps)} checks; production runtime and console NOT TESTED', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
