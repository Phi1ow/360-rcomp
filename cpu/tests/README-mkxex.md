# Original XEX metadata fixtures

`cpu/tools/mkxex.py` assembles original PPC source and wraps the linked bytes
in an unencrypted, uncompressed XEX2. It never consumes commercial content.
The image uses PE32 headers, big-endian PPC instructions/data and little-endian
PE fields. ImageBase, AddressOfEntryPoint and SizeOfImage describe the linked
fixture rather than remaining zero. The image has 0x1000 section alignment,
0x200 file alignment, a 0x1000 header area and padded raw section sizes.

The metadata extension uses the existing whitespace-separated annotation syntax:

```asm
#_ XEX_HEADER_VALUE 0xCAFE0000 0x12345678
#_ XEX_HEADER_VALUE 0xCAFE0100 0
#_ XEX_HEADER_VALUE 0xCAFE0201 0xA1B2C3D4
#_ XEX_HEADER_DATA 0xCAFE0302 header_fixed 8
#_ XEX_HEADER_DATA 0xCAFE04FF header_variable 12
#_ XEX_HEADER_VALUE 0x00030000 0x00200008

.section .rodata
.align 2
header_fixed:
    .long 0x13579BDF,0x2468ACE0
header_variable:
    .long 12,0x10203040,0x50607080
```

`#_ XEX_FUNCTION_END <label> <end label>` stops the `.pdata` length of a
function at `<end label>` (which is then not a function itself), to model
padding or code after a function that `.pdata` does not list.

VALUE accepts only keys ending in 00 or 01 and stores the unsigned 32-bit value
in the optional entry. DATA copies bytes from the linked symbol into the XEX
header; fixed keys 02..FE require exactly `low_byte * 4` bytes. FF requires at
least four bytes and an initial BE32 equal to the full block length, including
that length word. The payload must fit within one image section.

The generator rejects duplicate keys, collisions with its automatic headers,
wrong key/annotation combinations, integer overflow, invalid labels, invalid
sizes and malformed annotations. `XEX_HEADER_VALUE 0x00030000 ...` provides
system flags; there is no special privilege bypass. The JSON manifest reports
the emitted optional entries and basic PE metadata.

Run the original binary-format and rejection tests with a PowerPC binutils
toolchain on PATH:

```sh
python3 cpu/tests/run_mkxex_regression.py --out build/cpu-mkxex-regression-fresh
```

The output directory must be fresh. The suite validates the actual XEX/PE bytes,
not just the generator's JSON: immediate values, inline word location, fixed and
variable payloads, system flags, image/entry/base agreement, section alignment,
non-overlap and 19 malformed-input rejections. It currently reports 34 checks.

Sources are primary public code/specifications at exact revisions:

- [Microsoft PE specification, e103fa4e8810bd8d42c4777e17081e24dbe62dbd](https://github.com/MicrosoftDocs/win32/blob/e103fa4e8810bd8d42c4777e17081e24dbe62dbd/desktop-src/Debug/pe-format.md): PE32 offsets, image-relative addresses, alignment and optional header fields.
- [Xenia XEX definitions, 95a5c3ee250f80c3b9d139658649d9ffb6db3eec](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/util/xex2_info.h): XEX optional keys and SystemFlags.
- [Xenia XEX module, same pin](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/xex_module.cc): optional value/pointer distinction and Xbox PE checks. These observations describe the emulator implementation, not a complete hardware ABI oracle.

No export directory is fabricated by this change. XEX's optional exports-by-name
directory and ordinary PE exports use different relative-address conventions in
the consulted source; their full compatibility requires a separate contract.
Runtime resolution of already imported AOT kernel thunks does not require an
export directory in the fixture.

2026-09-28 validation: 34 host checks PASS in
`build/cpu-mkxex-regression-20260929/report.json`; XenonRecomp pin + patches
0001..0011 accepted the original XEX and emitted its one `_xstart` mapping.
It reported only the eight known missing save/restore helpers absent from this
tiny fixture, not an unsupported instruction or control-flow diagnostic.
Runtime execution and PS5 for this new metadata fixture: NOT TESTED by this lot.

## Versioned import libraries

`#_ XEX_LIBRARY_VERSION <module> <version> <minimum_version>` supplies raw
32-bit words in the real XEX import-library header at offsets `0x1C` and
`0x20`. It requires at least one import from that module. Without the annotation,
the generator keeps its existing `0x20000000` defaults, so old binary fixtures
are unaffected. Duplicate entries, malformed numbers, overflow, missing
arguments and minimum versions greater than the requested version are rejected.

The original kernel-boot fixture uses `0x201A1B00` for both words and both
libraries to exercise the title compatibility profile. This is explicit fixture
metadata, not a claim about the PS5's native firmware. The regression inspects
the binary header bytes independently of the generator's JSON output.
