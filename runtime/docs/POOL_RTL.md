# Pool ownership and RTL byte/string services

The seven exports below execute real guest-memory operations. They do not
delegate to Xenia or rexglue. All pointers are 32-bit guest addresses; host
arithmetic widens before adding offsets. Memory is never executable.

## ABI and supported behavior

| Ordinal | Export / registers starting at r3 | Behavior |
| --- | --- | --- |
| `0xB` | `ExAllocatePoolTypeWithTag(size, tag, pool_selector)` | Selector zero allocates RW guest heap memory, returns its address or NULL. Tag and requested size are host accounting metadata. Size zero returns NULL. Size above `0xFD8` is 4 KiB aligned; other sizes are at least 16-byte aligned. Contents are unspecified. Nonzero selectors terminate as `unimplemented`; their meaning remains unverified. |
| `0xF` | `ExFreePool(base)` | Frees only a currently owned pool base. NULL, interior pointers, double frees and stack/VA/physical/non-pool allocations terminate as `guest_access`. No return value is specified. |
| `0x11B` | `RtlCompareMemoryUlong(source, byte_length, pattern)` | Returns the byte length of the leading matching ULONG sequence. Pattern is interpreted in guest big endian. Invalid ULONG alignment returns zero. Zero length accesses nothing. |
| `0x126` | `RtlFillMemoryUlong(destination, byte_length, pattern)` | Repeats the big-endian pattern. Both address and length must be multiples of four; invalid alignment terminates as `guest_access`. Zero length accesses nothing. The complete destination must be RW before any byte is changed. |
| `0x11D` | `RtlCompareStringN(a, length_a, b, length_b, ignore_case)` | Counted-byte comparison, including embedded NUL. `0xFFFFFFFF` requests a NUL-terminated scan; `0xFFFF` is an ordinary length. Returns first differing byte difference, or length difference for an equal prefix, in the low 32 bits of r3. Nonzero ignore_case uses the fixed Xbox single-byte uppercase mapping, independent of host locale. |
| `0x12C` | `RtlInitAnsiString(destination, source)` | Aliases the source in an 8-byte descriptor `{be16 Length, be16 MaximumLength, be32 Buffer}`; no source copy. Length excludes NUL, maximum includes it. NULL source resets all fields. Length saturates at `0xFFFE`, maximum at `0xFFFF`. |
| `0x12D` | `RtlInitUnicodeString(destination, source)` | Same descriptor, source UTF-16BE code units, lengths in bytes. Saturates at `0xFFFC`/`0xFFFE`. Does not transcode or validate Unicode scalar values. r3 retains the descriptor address. |

The initializer destination is checked RW before scanning; its eight bytes
change only after the source length is known. Scanners validate readable pages
and stop at terminators without probing a following guard page. Comparisons
validate each value actually read, so a mismatch can stop before a later guard.
Guest faults terminate via the existing diagnostic path, not a native SIGSEGV.

For case-insensitive byte comparison, the implemented mapping uppercases ASCII
`a..z` and Latin-1 `E0..F6`/`F8..FE`, leaves the other bytes unchanged except
`FF -> 3F`. This mapping is a byte-comparison rule, not evidence for a Unicode
conversion codepage.

## Ownership, failure and lifetime

Pool, virtual and physical allocation registries belong to `Runtime`. They
disappear at shutdown after managed threads quiesce. Previously the VA/physical
sets survived shutdown: an abandoned VA address could authorize freeing an
unrelated allocation at the same address in a later title. The regression now
requires `STATUS_MEMORY_NOT_ALLOCATED` and preservation of the fresh allocation.

Each family locks its ownership registry, then its heap. Allocation prepares an
ownership node before asking the heap for memory. `GuestHeap` prepares all
free/used nodes before mapping or changing accounting; freeing similarly
prepares its free-list and double-free bookkeeping. Failures preserve live
allocation ownership, counts and output pointers. Freeing a pool block never
invokes a fatal hook while holding its mutex. No raw pointer ABI can distinguish
a stale pointer from a *currently valid allocation in the same family* at an
identical recycled address; this is the normal allocator limitation.

The heap refuses already committed Read/None pages rather than upgrading a page
which may contain another live object. It restores known stack guards before
recycling and checks the platform result. A failed multi-page platform operation
can retain newly committed pages or restored guard rights; the allocation is
still not granted/released. Full platform rollback is not claimed.

`NtAllocateVirtualMemory` and `NtFreeVirtualMemory` now validate both output
words as RW before allocation/free. Their registry check and heap mutation use
one family lock. Physical free removes ownership only after the heap succeeds.

Host C++ builds with exceptions recover `bad_alloc` in these allocator metadata
stages. PS5 builds use `-fno-exceptions`: host allocator exhaustion terminates,
and is **not** claimed to be a recoverable guest OOM. The import registry itself
may allocate its lookup key before entering an HLE function; recovery for that
path is outside this change. Guest heap exhaustion returns NULL/NTSTATUS in both
builds. Callers must serialize mapping/protection against running guest accesses
as required by `GuestMemory`; rights queries do not pin mappings.

Pool addresses and host-only accounting differ from console placement and its
undocumented allocation header. No guest-visible header is invented. Nonzero
pool-selector values remain an explicit compatibility limit; two static GTA IV
call sites use computed values, so selector-zero compatibility with the game has
not been established.

## Research and limitations

Public Xbox ABI research is pinned to [Xenia
Canary c332733](https://github.com/xenia-canary/xenia-canary/tree/c332733afd14ed3aeb08b38d0cabffa58d1c8c2f).
Its [RTL implementation](https://github.com/xenia-canary/xenia-canary/blob/c332733afd14ed3aeb08b38d0cabffa58d1c8c2f/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc)
establishes StringN's sentinel, counted comparison and fixed case table, and
corroborates the equal-prefix byte count. Its [pool research](https://github.com/xenia-canary/xenia-canary/blob/c332733afd14ed3aeb08b38d0cabffa58d1c8c2f/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc)
establishes argument order and the small/large threshold, but ignores its pool
selector. Only contracts were consulted; no implementation from either file was
copied. See [reference hashes](POOL_RTL_REFERENCES.json).

The [Microsoft comparison contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-rtlcomparememoryulong)
defines bytes and invalid alignment; the [fill contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-rtlfillmemoryulong)
defines alignment and repeated ULONGs. The [ANSI initializer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlinitansistring)
defines aliasing and saturation, and the [Unicode initializer contract](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-rtlinitunicodestring)
defines a counted string. The original local NT oracle below independently checks
both saturation limits and comparison examples. These are NT semantics adopted
for the supported Xbox ABI, not Xbox hardware measurements of overlong strings
or invalid alignment. Historical Xenia master counted matching words and had an
unused StringN length calculation; it is not used as the oracle for those cases.

Unicode/ANSI conversion imports and `RtlFreeAnsiString` stay unregistered. The
public references disagree between UTF-8 and one-byte replacement conversions,
and do not establish the console codepage. Registering an ASCII-only conversion
would conceal that missing functionality. No such conversion is advertised.

## Validation

Host results and exact commands are recorded in the ignored local directory
`build/runtime-pool-rtl-20260929`; production guest source remains under runtime.
No commercial game content is included in these tests.

```sh
export PATH=/usr/bin:/bin
cmake -S runtime -B build/runtime-pool-rtl-20260929/host -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DRCOMP_XENONRECOMP="$PWD/build/cpu-xenonrecomp-cfg-src" \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fsanitize-trap=undefined"
cmake --build build/runtime-pool-rtl-20260929/host -j 4
ctest --test-dir build/runtime-pool-rtl-20260929/host --output-on-failure
ctest --test-dir build/runtime-pool-rtl-20260929/host \
  -R 'rt_(pool_rtl|heap_failures|heap|physical|lifetime)$' \
  --repeat until-fail:10 --output-on-failure
```

The native `nt_oracle.py`/`nt_oracle.json` in that directory used public ntdll
exports on Windows 10.0.26200. The observed long ANSI/Unicode descriptor pairs
were `FFFE/FFFF` and `FFFC/FFFE`; four equal words compared as 16 bytes, and a
second-word mismatch as 4. This is a Windows oracle only.

Tests call the production import thunks and check bounds, endian, neighboring
canaries, readonly/guard pages, 4 GiB overflow, equality/prefix/sentinel/embedded
NUL/high-byte comparisons, allocation exhaustion, cross-family ownership,
shutdown/recreation, and four concurrent pool workers. The fault-injection suite
arms a TESTDOUBLE `operator new` *after* resolving the actual implementation
pointer to target only metadata staging. It covers 14 failure points across heap
allocate/free, pool, VA and physical allocations and checks recovery afterwards.
Final host result: **PASS 19/19 suites**, UBSan enabled, all command exit codes
zero (`build-verified.log`, `test-verified.log`). The five selected suites then
completed **50/50 repeated executions**, exit code zero (`repeat.log`).

PRIME separately verified the original PPC fixture on PS5 at
2026-09-28 18:43:51 UTC: platform **PASS 13/13**, packaged pool/RTL runner
**PASS 20/20**, six guest returns `0x6E` (22 positive guest checks per run),
allocation/handle baselines restored. The SELF SHA-256 was
`a01c2c1874b24eb051a5129082956acba859fd0cf7240906e40be8c742f2d7ae`,
build ID `927ec4fc5b16b3ea`. Logs are at
`build/prime-pool-rtl-20260928/console/pool-rtl/PPSA88360-20260928T184351Z`.
PRIME stopped the title and closed the temporary controller. That console proof
covers the positive fixture; fault cases and host-OOM injection remain host-only.
