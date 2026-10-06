# XMA audio hardware model

Found by the actual GTA IV PS5 entry: after startup it read `0x7FEA1800`, which
is register index `0x600` (`ContextArrayAddress`) of the XMA decoder's
memory-mapped register file at `0x7FEA0000`. The title's statically linked
audio code drives the decoder directly through that window and through 64-byte
context records in physical memory, so R-comp models the device state.

## Implemented

* `runtime/src/hle_xboxkrnl_xma.cpp` allocates one 64 KiB block from the
  physical heap holding the 320-entry context array (64 bytes each, zeroed) and
  publishes its physical address (`guest - 0xA0000000`) as
  `ContextArrayAddress`. Register layout follows the public rexglue-sdk
  c94f5eb `register_table.inc`.
* The register window is a virtual-field provider (`virtual_fields.h`), 32-bit
  accesses only. Readable/writable: `CurrentContextIndex` (0x606),
  `NextContextIndex` (0x607), `Kick` (0x650..0x659), `Lock` (0x690..0x699),
  `Clear` (0x6A0..0x6A9). `ContextArrayAddress` is read-only. Every other
  register (for example the store to offset 0x3980 the actual GTA IV entry
  performed) is plain zero-initialised storage that returns what the title
  wrote, like the public reference register file; it has no side effect.
* `XMACreateContext` (0x224) hands out a zeroed context and writes its address;
  exhaustion returns `STATUS_NO_MEMORY` (0xC0000017; the exact failure code of
  the kernel is not independently established). `XMAReleaseContext` (0x226)
  frees only a live context, anything else is a guest-access fatal.

## Optional real frame decoder

`RCOMP_XMA_FFMPEG_ROOT` selects an explicit install prefix containing
`include/libavcodec/avcodec.h`, `lib/libavcodec.a` and `lib/libavutil.a`.
The independent FFmpeg fork is pinned by PRIME to
`wmarti/FFmpeg` commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c`.
It supplies `AV_CODEC_ID_XMAFRAMES`, which accepts individual compressed
frames without a container/extradata or a second packet scheduler. Standard
FFmpeg XMA2 is a different interface. The minimal dependency is LGPL-2.1-or-later;
no rexglue/Xenia audio, CPU, kernel or system source is imported.

`runtime/src/xma_decoder.cpp` is R-comp's packet/context adapter. It assembles
frames across 2048-byte packets and both input buffers, honors stream packet
skip counts and bit cursors, and sends each frame to the actual FFmpeg codec.
Only successful decoding advances the input cursor or retires an input valid
flag. Planar float samples are saturated and interleaved as signed 16-bit
big-endian PCM in the physical output ring. One ring block is reserved to
distinguish full from empty. PCM that cannot yet fit remains in the decoder;
a later kick resumes it. Invalid/incomplete input or a full ring does not
fabricate input consumption. Finite/infinite loop cursors and subframe skip/end
fields are modeled; one worker visit is bounded to 32 decoded frames.

A real worker handles queued kicks. A kick requests one processing visit;
the guest submits another kick after replenishing input or draining output.
LOCK synchronously fences decoding and disables pending work. CLEAR fences
and resets valid flags/cursors plus the actual codec state. RELEASE fences and
retires the decoder. Shutdown stops and joins the worker before Runtime memory
is destroyed; subsequent generations receive fresh decoder state. Hardware
publication preserves guest-owned fields, writes input valid flags last,
and diagnoses an inaccessible context instead of reporting completion.

With no dependency selected, the existing register/context model remains:
a kick is recorded and reports `XMA2 decoder not implemented, input not consumed`;
all input/cursor/output fields remain unchanged. Audio output through the PS5
`XAudio*` render backend is still BLOCKED. Decoding to guest PCM does not establish
audible output or gameplay.

The codec logger is installed before decoder creation. It keeps FFmpeg's
configured level filter and forwards actual messages and numeric levels to
`stderr` using `std::vfprintf`. It never probes a terminal or stubs `isatty`.
This avoids the verified PS5 null import in FFmpeg's default log callback.

## Register byte order (found 2026-09-29)

The register file is little-endian device memory: GTA IV accesses it with
`lwbrx`/`stwbrx`, so the value a plain big-endian guest word access sees is
the byte-swapped register. The provider now swaps on both paths. Before the
fix the title read `ContextArrayAddress` byte-swapped, computed context
indices from it and wrote its kicks and locks to registers `0xE10`/`0xE50`
instead of `0x650`/`0x690`; after it, the kicks land on `Kick` (0x650+) and
`Lock` (0x690+).

With the decoder enabled, `CurrentContextIndex` and `NextContextIndex` reflect
the worker's scheduling candidate. LOCK selects a candidate outside its locked
mask after fencing. Guest writes to either cursor are bounded to 320 contexts.
Without the dependency, the previous one-context-per-10-us diagnostic sweep
remains; that sweep performs no decoding and is not evidence of completion.

## Kick completion (2026-09-29)

The exploratory input-consumption shortcut has been removed. Valid bits are
never cleared merely because a kick was written. With the optional decoder,
actual decoding and durable PCM govern consumption; without it, input flags,
packet cursors and output fields remain unchanged.

`runtime/tests/test_xma.cpp` exercises the production register provider and
real physical context array. It verifies little-endian register values and
unchanged context records after kicks across three register groups, including
contexts 0 and 319. `test_xma_packets.cpp` covers packet continuations/skips,
incomplete input, malformed physical pointers, loop cursors and PCM conversion.
`test_xma_decode.cpp` exercises the production worker/provider and real FFmpeg
with independently encoded synthetic mono/stereo silence: PCM bytes and ring
cursors, full/wrapped buffers, guest drain, invalid input, lock/clear/release,
shutdown with pending work and a fresh Runtime generation. No decoder double
or title asset is used. The decoder-enabled UBSan suite passed 36/36 tests.

PS5 decoder execution, non-silent sample/timing oracles and MDCT delay alignment:
NOT TESTED. Host physical aliases: NOT TESTED (`vm_alias` returns ENOSYS).
Host verification is never PS5 gameplay/audio proof.

For crash localization, `RCOMP_XMA_DECODER_DIAGNOSTICS=ON` traces the first
find/allocate/open/send/receive calls before and after each stage per context,
including genuine return codes. This is compile-time opt-in and OFF removes
all trace call sites. There are no timing calls or atomic counters per frame.

## Kernel context helpers

`runtime/src/hle_xboxkrnl_xma_api.cpp` (Skate 3) implements the XMA* helpers on
the same 64-byte records and registers, through `src/xma_internal.h`: every record
access holds the device mutex, and Enable / Disable / Initialize write the
context's bit to Kick / Lock / Clear exactly as a title store does (shared code
path `apply_register_locked` in `hle_xboxkrnl_xma.cpp`).

| Export | Effect on the record (w = big-endian word) |
| --- | --- |
| `XMAInitializeContext` (0x225) | Clear, then the record from `XMA_CONTEXT_INIT` (56 bytes): w0 packet count 0 / loop count / block count, w1 packet count 1 / loop subframe end and skip / subframe decode count / sample rate / stereo, w2 read offset, w3 loop start, w4 loop end, w5..w7 physical buffer addresses (NULL input buffers stay 0). Work buffer not stored. |
| `XMAEnableContext` (0x227) | Kick (one decoder visit). |
| `XMADisableContext` (0x228) | Lock; the lock is fenced synchronously, so the optional wait is already satisfied. |
| `XMASetInputBuffer0/1` (0x232/0x233) | w5/w6 physical address, packet count. |
| `XMASetInputBuffer0Valid/1Valid`, `XMAIsInputBuffer0Valid/1Valid` (0x22E..0x231) | w0 bit 20 / 21. |
| `XMASetInputBufferReadOffset` (0x237) | w2 bits 0..25. |
| `XMASetOutputBufferValid`, `XMAIsOutputBufferValid` (0x22C/0x22D) | w1 bit 31. |
| `XMASetOutputBufferReadOffset`, `XMAGetOutputBufferReadOffset` (0x22A/0x22B) | w9 bits 0..4. |
| `XMAGetOutputBufferWriteOffset` (0x229) | w0 bits 27..31 (published by the decoder). |

The Clear is written before the record (the reference writes it after): with the
decoder, Clear resets the read offset to 32 and the flags, which would otherwise
overwrite the title's initial values. `channel_count` 0 is mono and 1 stereo (the
reference's `>= 1`); 2 and above, values that do not fit their field, a buffer
outside the physical windows and a NULL output buffer have no established
contract and are explicit fatals. A context address that is not a live
`XMACreateContext` context is a guest-access fatal, as for `XMAReleaseContext`.
Entry points and `XMA_CONTEXT_INIT`: rexglue-sdk c94f5eb
`xboxkrnl_audio_xma.cpp` (BSD-3, read only). Host evidence:
`tests/test_xma_api.cpp` (record bits, register effects, rejections). PS5 and the
decoder-enabled build with these helpers: NOT TESTED.
