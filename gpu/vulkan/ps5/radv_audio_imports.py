#!/usr/bin/env python3
"""Check converted native audio imports against read-only firmware metadata."""
import json
import pathlib
import struct
import sys

FUNCTIONS = {
    'libSceAudioOut.sprx': ('sceAudioOutInit', 'sceAudioOutOpen', 'sceAudioOutOutput', 'sceAudioOutClose'),
    'libSceUserService.sprx': ('sceUserServiceInitialize', 'sceUserServiceGetInitialUser'),
}


def elf_imports(path):
    data = pathlib.Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('ELF64 little-endian required')
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    phsize, phnum, shsize, shnum = struct.unpack_from('<HHHH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + n * phsize) for n in range(phnum)]

    def offset(address, size):
        for typ, flags, pos, va, pa, file_size, mem_size, align in segments:
            if typ == 1 and va <= address and address + size <= va + file_size:
                result = pos + address - va
                if result + size <= len(data):
                    return result
        raise ValueError('unmapped ELF dynamic address')

    dynamic = []
    for typ, flags, pos, va, pa, file_size, mem_size, align in segments:
        if typ == 2:
            for at in range(pos, pos + file_size, 16):
                tag, value = struct.unpack_from('<qQ', data, at)
                if not tag:
                    break
                dynamic.append((tag, value))
    tags = dict(dynamic)
    strings = offset(tags[5], tags[10])
    strings_size = tags[10]

    def string(index):
        if index >= strings_size:
            raise ValueError('ELF string out of bounds')
        end = data.index(b'\0', strings + index, strings + strings_size)
        return data[strings + index:end].decode('ascii')

    count = None
    if 0x6100003F in tags:
        count = tags[0x6100003F] // tags[11]
    else:
        for n in range(shnum):
            section = struct.unpack_from('<IIQQQQIIQQ', data, shoff + n * shsize)
            if section[1] == 11:  # SHT_DYNSYM
                count = section[5] // section[9]
                break
    if count is None or tags[11] != 24:
        raise ValueError('missing ELF symbol count or unsupported entry size')
    symbols = offset(tags[6], count * 24)
    imports = set()
    for n in range(count):
        name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', data, symbols + n * 24)
        if name and not section and info >> 4 in (1, 2):
            imports.add(string(name))
    needed = {string(value) for tag, value in dynamic if tag == 1}
    return imports, needed


def audit(before, after, exports_path):
    original, _ = elf_imports(before)
    converted, needed = elf_imports(after)
    wanted = {name for names in FUNCTIONS.values() for name in names} & original
    if not wanted:
        return {'status': 'NOT TESTED', 'reason': 'no native audio import in linked title', 'imports': []}
    proof = {row['module']: row for row in json.loads(pathlib.Path(exports_path).read_text())}
    result = []
    for module, functions in FUNCTIONS.items():
        required = set(functions) & wanted
        if not required:
            continue
        row = proof[module]
        converted_module = module.replace('.sprx', '.prx')
        if converted_module not in needed:
            raise ValueError('converted audio module absent: ' + converted_module)
        for name in sorted(required):
            export = row['imports'][name]
            if export['present'] is not True:
                raise ValueError('firmware export absent: ' + name)
            matches = sorted(text for text in converted if text.split('#')[0] == export['nid'])
            if len(matches) != 1:
                raise ValueError('converted NID missing or ambiguous: ' + name)
            result.append({'function': name, 'module': module, 'nid': export['nid'],
                           'converted_symbol': matches[0], 'firmware_sha256': row['sha256']})
    return {'status': 'PASS', 'imports': result, 'console': 'NOT TESTED'}


def main():
    if len(sys.argv) != 5:
        raise ValueError('usage: radv_audio_imports.py before.elf converted.elf exports.json result.json')
    result = audit(*sys.argv[1:4])
    pathlib.Path(sys.argv[4]).write_text(json.dumps(result, indent=2) + '\n')
    print(result['status'] + ' converted audio imports: ' + str(len(result['imports'])))


if __name__ == '__main__':
    try:
        main()
    except (KeyError, OSError, ValueError, struct.error) as error:
        print('FAIL audio import audit: ' + str(error), file=sys.stderr)
        sys.exit(1)
