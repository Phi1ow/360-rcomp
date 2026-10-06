# Xbox/NT string helpers

This tranche implements the GTA IV `xboxkrnl.exe` imports
`RtlFreeAnsiString`, `RtlMultiByteToUnicodeN`, `RtlNtStatusToDosError`,
`RtlUnicodeStringToAnsiString`, `RtlUnicodeToMultiByteN`, and
`RtlUpcaseUnicodeChar`. It also implements the bounded one-shot hash helper
`XeCryptSha`; key, signature, RSA, random-number, and verification routines
remain outside this tranche.

## ANSI compatibility profile

The NT routines are locale-sensitive: Microsoft documents
`RtlMultiByteToUnicodeN` and `RtlUnicodeToMultiByteN` as using the ANSI code
page selected at system boot, and `RtlUnicodeStringToAnsiString` as using the
current system locale. R-comp does not currently model the Xbox 360 console
locale. For this Western-European GTA IV tranche it therefore selects one
explicit deterministic profile: **Windows code page 1252**.

This is a compatibility configuration, not a claim that every Xbox 360 or
every title locale uses CP1252. A future locale service must select the proper
table explicitly rather than silently reinterpreting non-ASCII bytes.

The conversion data is generated from the Unicode Consortium copy of
Microsoft's `bestfit1252.txt`. Microsoft Open Specification `[MS-UCODEREF]`
identifies these `bestfitxxxx.txt` files as the supported Windows code-page
data and identifies 1252 as the usual ANSI code page for US English and as a
Western-European ANSI page. The pinned file has SHA-256
`72ea23c939c5b26fae7aded0207b327e2f3902d7d3c168d7087f5cfc38ee76a9`.

Consequences of that table are deliberate and tested:

- byte `0x80` converts to U+20AC (Euro), not U+0080;
- bytes `0x81`, `0x8D`, `0x8F`, `0x90`, and `0x9D` round-trip as their C1
  controls, matching the Microsoft best-fit table and the local Windows NT
  oracle;
- Unicode-to-ANSI uses Microsoft's best-fit entries (for example U+03A9
  Greek capital omega maps to ASCII `O` in CP1252); characters absent from
  the table use its declared default byte `0x3F` (`?`).

## Low-level counted conversions

`RtlMultiByteToUnicodeN` and `RtlUnicodeToMultiByteN` use the Xbox PPC ABI
arguments in `r3..r7`. They are counted conversions: embedded NULs are data,
the output is not implicitly terminated, truncation returns `STATUS_SUCCESS`,
and the optional byte-count result reports only the data actually emitted.
An odd Unicode source byte count ignores the final incomplete byte, matching
the observed NT routine behavior. An odd Unicode destination capacity writes
only complete 16-bit code units.

Microsoft documents the source and destination buffers of both low-level
routines as non-overlapping. R-comp validates both ranges before mutation and
traps a prohibited overlap as a guest-access fault. It also rejects an output
byte-count pointer that overlaps either conversion buffer, preventing a
successful call from corrupting its own result.

Zero effective output length does not require either data pointer to be
mapped. Non-zero accessed ranges must be committed with the required guest
permissions. Validation is based on the bytes actually consumed/emitted, so
a conversion truncated before a protected following page does not probe that
page.

## `RtlUnicodeStringToAnsiString` and ownership

Xbox `ANSI_STRING` / `UNICODE_STRING` descriptors are treated as big-endian
`{ USHORT Length; USHORT MaximumLength; ULONG Buffer; }` structures. Source
`Length` is a byte count. A trailing odd byte is ignored, consistent with the
host NT oracle.

With `AllocateDestinationString == FALSE`, R-comp uses the caller's current
buffer. It reserves one byte for a terminator whenever `MaximumLength > 0`,
sets `Length` to the number of payload bytes written, preserves
`MaximumLength` and `Buffer`, and returns `STATUS_BUFFER_OVERFLOW` when the
payload plus terminator does not fit. A three-byte buffer converting
`A, Euro, Omega, e-acute` therefore receives `41 80 00`, `Length=2`, and
`STATUS_BUFFER_OVERFLOW`, exactly matching the pinned NT oracle.

With allocation enabled, R-comp allocates `payload + 1` bytes from the real
guest heap, writes the terminator, updates all three descriptor fields, and
records that exact allocation as `RtlUnicodeStringToAnsiString` ownership.
`RtlFreeAnsiString` frees only one of those owned bases and then resets the
descriptor to zero. It cannot free pool, virtual-memory, stack, TLS, or other
GuestHeap allocations merely because an address aliases them. A NULL buffer
is reset without touching the heap. Allocation failure leaves the destination
descriptor unchanged and does not leave ownership metadata behind.

## Uppercase and NTSTATUS mapping

Microsoft documents `RtlUpcaseUnicodeChar` as returning the uppercase form of
one Unicode character. The public documentation does not publish the NT
uppercase table itself. To avoid host-libc locale dependence on PS5, the
generated table records every changed UTF-16 code unit observed from
`ntdll!RtlUpcaseUnicodeChar` in the pinned Windows NT binary below; all other
16-bit values are unchanged. This also preserves NT's single-WCHAR behavior
for values such as U+00DF.

`RtlNtStatusToDosError` is similarly table-driven. The generator parses every
unique `NTSTATUS` constant in Microsoft Windows SDK 10.0.26100.0
`shared/ntstatus.h` (2969 unique values) and records the result returned by
the pinned `ntdll!RtlNtStatusToDosError`. A numeric status not defined by that
SDK table returns `ERROR_MR_MID_NOT_FOUND` (317), the documented fallback.
This avoids a hand-written partial mapping; for example the oracle establishes
`STATUS_TIMEOUT (0x00000102) -> ERROR_TIMEOUT (1460)` and
`STATUS_BUFFER_OVERFLOW (0x80000005) -> ERROR_MORE_DATA (234)`.

The oracle binary is `C:\Windows\System32\ntdll.dll`, file version
`10.0.26100.9278`, SHA-256
`a74f7482085eab125ccc09152ab7e0b5994bcb13e1a7b29880bdbb24179ecb8b`.
The SDK `ntstatus.h` used to enumerate status values has SHA-256
`50bfd57f92dd01c93c1960292aa428e3323043dc947cf15c0dfa6cfec0778ae6`.

These tables are an NT compatibility oracle, not PS5 execution evidence and
not proof of the exact historical Xbox 360 Unicode-table revision. The
functions exercised by GTA IV use the same public NT contracts and Xbox
ordinals, while the locale-sensitive conversion is deliberately pinned to
the CP1252 profile described above.

## `XeCryptSha`

GTA IV also imports ordinal `0x192`, `XeCryptSha`. Public Xbox-oriented
research agrees on the eight-argument one-shot ABI:

`(Input1, Input1Size, Input2, Input2Size, Input3, Input3Size, Output, OutputSize)`.

All eight scalar arguments fit the Xbox PPC integer argument registers
`r3..r10`; no varargs or stack decoding is involved. R-comp hashes the three
byte ranges in order with SHA-1 and writes the first `min(OutputSize, 20)`
digest bytes. Zero-sized ranges may have NULL pointers. A non-zero size
requires a readable guest range; a non-zero output requires a writable guest
range. The digest is finalized before output is written, so output may overlap
an input range without changing the result.

The SHA-1 compression/finalization code is an independent implementation of
NIST FIPS PUB 180-4, rather than imported emulator code. Tests include the
FIPS `abc` vector and the 56-byte
`abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq` vector, plus the
empty digest, three-segment feeding, truncation, NULL/zero handling, and exact
input/output overlap.

## Ordinals and independent source boundary

The ordinals are cross-checked at runtime against the vendored XenonRecomp
export table (commit `ddd128bcca99fe8bfbb99bea583c972351fa6ace`,
`xboxkrnl_table.inc` SHA-256
`99bbdad7faaec18ce627eca28f0900b8e4a47c372000e43f85b447f07a2683ba`).
That file supplies names/ordinals only. R-comp's implementation and behavioral
tables come from the primary NT/code-page references and oracle described
here; no Xenia/rexglue kernel implementation is imported. For the public
`XeCryptSha` parameter shape only, `xboxkrnl_crypt.cc` from Xenia Canary commit
`c332733afd14ed3aeb08b38d0cabffa58d1c8c2f` was used as an ABI cross-check;
the pinned file SHA-256 is
`b233267b9a8182229a7ca5bbded45d32e9a22ba4fc2a067db7dc98d837497355`.

Primary references and pinned hashes are machine-readable in
`runtime/docs/STRINGS_REFERENCES.json`.
