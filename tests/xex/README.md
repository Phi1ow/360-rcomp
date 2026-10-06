# Original XEX rehearsals

The default mode keeps `rcomp_title.s` and the 11 historical checks,
including negative imports and four forms of jump tables.

`tools/host_boot_rehearsal.py` selects `rcomp_boot.s` with
`RCOMP_XEX_BOOT_REHEARSAL=ON`: mkxex → M6 decoder → XenonAnalyse →
XenonRecomp → real `app::TitleRuntime`. It tests video/variable import,
static TLS, memory, VFS reads, two guest threads and cleanup across
three lifecycles. The 50 checks include bootstrap and mount errors; every
guest wait is bounded. Expected values are computed in the PPC source,
independently of the generated results.

The temporary files contain only original data. The only substituted
boundary is `TESTDOUBLE_cpu_only_gpu`; a render call aborts. This test
validates no pixel, no PS5 ELF/SELF, no RADV and no commercial file.

The `RCOMP_XEX_PACKAGED_BOOT=ON` mode uses the same `rcomp_boot.s` fixture
and the real `TitleRuntime` from `boot_ps5_runner.cpp`. It exposes
`rcomp_xex_selftest(FILE*, const char*)` for the PS5 `--xex` title and for
its small host launcher. The folder containing the XEX is mounted as `game:`
and `d:`; `boot-data.bin`, exactly the four bytes `01 02 03 04`, must be
packaged next to it. No file, symlink or child process is created on the
console. The pthread watchdog bounds the test to 45 seconds; each guest
wait keeps its five-second limit.

The 27 checks cover the packaged data, three lifecycles, two PPC runs per
lifecycle (static TLS, file read, two workers, termination), the mounts and
the cleanup of allocations/handles/imports. On the host, CTest requires the
final marker `checks=27 pass=27 fail=0` and runs three negative packages
(missing data, wrong data, malformed XEX), all rejected before PPC entry.
The historical 50-check mode stays separate.

PS5 linking of the packaged mode also requires `rcomp_runtime_video`,
`rcomp_platform_input` and the `ScePad`/`SceUserService` system libraries.
The only substituted boundary is still `TESTDOUBLE_cpu_only_gpu`. Building
these archives proves no PS5 execution; only the console log provides that
proof, and it carries `scope=PS5`.

The `RCOMP_XEX_POOL_RTL=ON` mode, mutually exclusive with both boot modes,
uses `fixtures/xex/rcomp_pool_rtl.s` and `pool_rtl_runner.cpp`. It keeps the
archive entry point `rcomp_xex_selftest(FILE*, const char*)` and the same
runtime/platform dependencies as the packaged boot. Only the XEX is needed:
no `boot-data.bin`, mount or commercial file. `tools/host_boot_rehearsal.py
--mode pool-rtl` builds the XEX, checks the imports and generates the AOT CPU.

The PPC oracle contains 22 original steps: pool allocations of 64 and 4096
bytes, alignments and frees; big-endian fill with guards; comparison of a
word prefix in bytes; ANSI and Unicode descriptors (fields, aliasing, null
source); counted strings, length difference, embedded NUL, `0xFFFFFFFF`
sentinel, ASCII and Latin-1 case. It returns `0x6E` after all steps or
`0xE101..0xE116` at the first failure. Expected values are written in the
fixture, without depending on the HLE under test. The string and
CompareMemoryUlong contract follows the public reference
[Xenia Canary c332733](https://github.com/xenia-canary/xenia-canary/blob/c332733afd14ed3aeb08b38d0cabffa58d1c8c2f/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc);
FillMemoryUlong accesses follow the alignments and lengths of the
[Microsoft documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-rtlfillmemoryulong).

The dedicated runner performs three lifecycles and two runs per lifecycle;
it also checks live allocations, allocated bytes, handles and runtime
cleanup. Its marker is `RCOMP-POOL-RTL checks=20 pass=20
fail=0 scope=host` (or `scope=PS5`). The watchdog bounds execution to 45
seconds. Two negative host packages (missing or malformed XEX) must fail
before PPC entry. No HLE import is doubled; the GPU boundary stays
`TESTDOUBLE_cpu_only_gpu`, with no graphics proof.

The exclusive `RCOMP_XEX_MODULES=ON` mode uses `rcomp_modules.s` and
`modules_runner.cpp`, with the same archive entry point and only the
packaged XEX. The 21 PPC steps check the four module/command-line/monitor
variables, the names of the real main module, its UTF-16BE descriptors, the
image and its XEX header. The original optional fields distinguish an
immediate value, the address of the inline word, a fixed block, a
variable-size block, a zero value and an absent field. Privileges are
derived from `system_flags=0x00200008`. Two resolutions of
`RtlCompareStringN`, by ordinal and by name, return the address of the
imported AOT thunk, and the PPC then actually calls it through CTR. The
HMODULE variable is also resolved; the missing module/export and invalid
handle errors check their distinct effect on the output.

The oracle ends with `0x6F` or the first failure `0xE201..0xE215`. The
runner expects 29 checks, three lifecycles, two distinct exact names/command
lines and two runs per lifecycle. Each lifecycle starts with a bootstrap
failure (empty path, duplicated AOT table, truncated XEX), checks that the
state is clean, then creates a valid title in the same process. Allocations
are compared with their state after `Create`, without masking the persistent
module storage. The watchdog is still 45 seconds and the unused GPU is still
the only substituted boundary.

Seven negative host packages check a missing/malformed XEX, a duplicated
key, a block that overlaps the table or runs past the header, an
overflowing variable size and the still unimplemented `XboxKrnlVersion`
import, which must keep its poison value and prevent PPC entry. Function
exports of the main module, DLL loading and enabled monitors are not
validated by this fixture. The limited contract follows the pinned public
sources listed in the [runtime contract](../../runtime/docs/MODULES.md);
no enabled-monitor layout or version profile is invented.

Three positive host variants keep the same program and the same oracle:
a PE size larger than the XEX payload, a logical non-executable section
outside the payload, and a PE raw offset that differs from its memory
address. Each must pass the 29 checks and six `0x6F` returns. The already
decompressed XEX bytes are loaded as a memory image; the PE metadata must
neither move the code to `PointerToRawData` nor cause reads past the real
payload.
