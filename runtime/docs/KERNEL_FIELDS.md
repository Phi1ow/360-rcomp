# Bounded kernel variables and virtual thread fields

Runtime next-batch work for the GTA IV PS5 boot path, 29 September 2026.
This contract exposes only fields supported by the title metadata or by the
pinned public ABI research. It does not publish a synthetic Xbox 360 kernel
image, firmware version, `KTHREAD` blob, or `OBJECT_TYPE` layout.

## Why the fields are virtual

`build/runtime-objects-nextbatch-20260929/PCR100_REPORT.md` found 13 actual
32-bit big-endian reads of `PCR+0x100`. Six use the resulting thread Body only
as identity; seven dereference it. The established dereference set is
`Body+0x14C` read32 for the stable thread ID and `Body+0x160` read/write32 for
per-thread LastError. Three later helpers read `Body+0x58`, but the pinned
rexglue/Xenia `X_KTHREAD` definition still names those bytes `unk_58[4]`.
A separate comment in its `xboxkrnl_threading.cpp` calls offset `0x058`
"kernel time". The observed game data flow snapshots `+0x58` beside MFTB and
later computes a delta from both. That is consistent with a timing statistic,
but the source conflict leaves its units, update points and exact ownership
undefined. R-comp therefore does not publish it yet.

The existing object arena `0x70000000..0x70FFFFFF` is reserved before
`GuestHeap::init` and intentionally stays uncommitted. Mapping a partial
zero-filled `KTHREAD` page would make every unknown byte look implemented.
Instead the CPU/prelude integration asks `runtime_virtual_read/write` before
the normal committed-page check. Providers return a scalar value or a precise
status; they do not fatal while a registry mutex or object reference is live.
Unknown offsets remain `UnknownField`, so the CPU layer can issue one diagnostic
with effective address, object base/offset, width, direction and LR.

The first 64 KiB of the existing opaque arena is the kernel-token page. Thread
Body tokens start at `0x70010000` and advance by `0x1000`, larger than the
publicly described 0xAB0 KTHREAD extent. This prevents `Body+offset` from
aliasing another object while keeping all tokens inside the already-reserved
range. `ExThreadObjectType` stays an identity token: any descriptor field read
is unknown rather than a zero-filled structure.

## XboxKrnlVersion: title compatibility profile

The local read-only GTA IV import-library evidence in
`build/runtime-objects-20260928/gtaiv-import-versions.json` records both
`xboxkrnl.exe` and `xam.xex` with version and minimum version `0x201A1B00`.
Decoded with the XEX packed-version fields, this is `2.0.6683.0`.

`parse_kernel_compatibility_profile_from_xex` validates the actual XEX optional
header/import-library layout already used by `xex_loader.cpp`. It requires the
kernel version and minimum to agree, and if XAM is present its minimum must
agree with the kernel claim. The registrar applies the version policy below.
The log names the source as `title_import` and records the packed version and
minimum. This value is compatibility metadata requested by the title, not a
claim about the PS5, Xbox hardware firmware, or full kernel behavior.

### Version policy (owner decision and its 3 October 2026 amendment)

Every title sees the kernel version it was built against: the `version` of its
`xboxkrnl.exe` import library (GTA IV 2.0.6683.0 and the Episodes from Liberty
City 2.0.8498.0 declare version == minimum, so nothing changes for them).

A console runs a title when the title's declared **minimum** kernel is not
newer than the console's kernel; the import `version` is the XDK the title was
built with, which can be newer than any retail kernel (NARUTO STORM 3 imports
2.0.21173.0). `presented_kernel_profile()` therefore:

| Title claim | Presented kernel |
| --- | --- |
| version in 2.0.1888 to 2.0.17559 | the version itself |
| version 2.0.x above 17559, minimum in 2.0.1888 to 2.0.17559 | 2.0.17559.0 (`kFinalRetailKernelPacked` 0x20449700), the final retail kernel: the newest a real console could run it on |
| anything else (minimum above 17559, outside 2.0, version < minimum) | `Unsupported` (refused, not guessed) |

The registration log prints `RCOMP-KERNEL profile_capped built=... min=...
presented=...` when the cap applies. `parse_kernel_compatibility_profile_from_xex`
still reports the title's own claim and now also its `minimum_packed`.
`XamGetSystemVersion` reports the same presented profile. PRIME's installer
check should mirror this rule: accept when `minimum` is a 2.0 kernel in
1888..17559 (and `minimum <= version`), whatever the `version`.

`XboxKrnlVersion` ordinal `0x0158` points at virtual address `0x70000100`.
Its eight readable bytes are four big-endian 16-bit fields: major, minor,
build and QFE. Writes are rejected as `ReadOnly`; bytes outside that eight-byte
record remain unknown.

## KeTimeStampBundle

The only title field currently established is `KeTimeStampBundle+0x10`, a
big-endian 32-bit uptime value in milliseconds. Ordinal `0x00AD` points at
virtual address `0x70000200`; `+0x10` is computed when read from the runtime's
monotonic origin. There is no background writer and therefore no 1 ms thread,
guest-memory race, or stale cached counter. Prefix bytes and trailing fields
return `UnknownField`.

The runtime currently has `KeQueryPerformanceFrequency` and a monotonic clock,
but no `KeQueryTickCount` implementation. `xboxkrnl_uptime_millis` centralizes
the title uptime origin so a future, independently verified tick-count path can
share it instead of creating a second epoch.

## Integration still gated by the frozen candidate

The new virtual-field and kernel-variable units are intentionally unintegrated
while the current PS5 services candidate is frozen. After release, the thread
object implementation must mint Body tokens from `kThreadVirtualTokenBase` by
`kThreadVirtualTokenStride`, store the real per-thread LastError atomically, and
register a provider for the thread arena. Guest-thread setup must write that
same Body as BE32 to `PCR+0x100` before guest execution. The explicit single-CPU
policy may publish byte zero at `PCR+0x10C`; `PCR+0x150` remains unresolved and
must not receive a guessed gate value.

TLS KTHREAD fields stay unpublished. `Body+0x58` is described below.

## KTHREAD fields of xapi fibers (3 October 2026)

Halo 3 statically links xapi's fiber routines (ConvertThreadToFiber,
CreateFiber, SwitchToFiber, DeleteFiber, GetCurrentFiber), which read and write
the KTHREAD directly (first trap: `Body+0x164` read, lr 0x823173BC). Layout
source: the pinned rexglue `X_KTHREAD` / `X_KPROCESS` / `X_FIBER_CONTEXT`
headers (BSD-3, read only), cross-checked with the generated code of those
routines. Published, all BE32, width 4 only:

| Field | Access | Value |
| --- | --- | --- |
| `+0x05C` StackBase | read (write: `ReadOnly`) | high end of the running stack: the GuestThread's `stack_base`, then the StackBase of the last `KeSetCurrentStackPointers` |
| `+0x060` StackLimit | read (write: `ReadOnly`) | low end of the running stack (`stack_limit`, same update rule) |
| `+0x084` Process | read (write: `ReadOnly`) | `kTitleProcessVirtualAddress` (`0x70000500`), the title process token |
| `+0x0D0` StackAllocBase | read (write: `ReadOnly`) | the GuestThread's `stack_base`, then the StackAllocBase of the last `KeSetCurrentStackPointers` |
| `+0x164` FiberPtr | read/write | the current xapi fiber block; 0 for a new thread (GetCurrentFiber's "not a fiber" test) |

`+0x0D0`: xapi's CreateFiber stores the value MmCreateKernelStack returned (the
high end) as both the fiber's StackAllocBase and StackBase, and DeleteFiber
hands it back to MmDeleteKernelStack. A thread's own stack follows the same
convention; the value is only copied by ConvertThreadToFiber and given back to
KeSetCurrentStackPointers (deleting another thread's stack through it is a
guest error that MmDeleteKernelStack reports).

The title process token lives in the kernel-token page and is served by the
kernel-variable provider (registered with the title's kernel variables). Only
`KPROCESS+0x1C` (default thread stack size, read by `CreateFiber(0, ...)`) is
published: `kDefaultGuestThreadStackSize` = `0x40000`, the stack R-comp's
ExCreateThread really gives a thread created with stack size 0. On the console
this is the XEX default stack size; R-comp does not read that header yet and
publishes the value it uses, so both stay consistent. Every other byte of the
token is `UnknownField`; a write to `+0x1C` is `ReadOnly`. Unknown KTHREAD
offsets still trap. Switching semantics: `THREAD_OBJECTS.md` ("Guest fibers").

### Scaling of the +0x58 counter (compatibility)

The actual GTA IV D3D9 code uses `Body+0x58` as its GPU-hang watchdog clock
(`sub_829CFE80`: hang report when the counter advanced by 5000 with no GPU
progress). The host's first-use shader translation and pipeline creation are much
slower than a console GPU, which tripped the watchdog ("The GPU is hung!") in long
runs. The published counter therefore runs at 1/20 of the measured thread CPU
milliseconds (`kThreadKernelTimeDivisor` in `thread_object.cpp`). The real unit
remains unverified; revisit when the host GPU path is optimised.
