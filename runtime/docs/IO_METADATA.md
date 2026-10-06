# Xbox file IO HLE

Host validation: **PASS** (production rt_io, normal + forced lexical fallback,
and Clang undefined-behavior trap instrumentation).
PS5 strict compile of current vfs.cpp and hle_xboxkrnl_io.cpp: **PASS**.
PS5 execution of this tranche: **NOT TESTED** here.

This file describes the limited file-information contract implemented by
runtime/src/hle_xboxkrnl_io.cpp and runtime/src/vfs.cpp. It does not copy a
kernel implementation. The Xbox layouts and export ordinals are cross-checked
against public references, then the values are produced from R-comp's own VFS.

## Public references

The Xbox-specific enum values, packed layouts and ordinals are pinned to
Xenia commit 95a5c3ee250f80c3b9d139658649d9ffb6db3eec:

- src/xenia/kernel/info/file.h (blob
  9305927bcf3350afb56b1cd32b49edd14685259d): file information classes and
  the 8-byte position/internal and 56-byte network-open layouts.
- src/xenia/kernel/info/volume.h (blob
  bbb6ec3ac43adba01e3feabd02e89ee129729c7d): Xbox volume/size/attribute
  layouts.
- src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc (blob
  1ace11c97ad09e1c9c010087f65b7029a046af89): query dispatch and big-endian
  field semantics.
- src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc (blob
  3e7a700ef750fd481401eeff618eb407dd952157): write/event ABI,
  full-attributes ABI and the NtFlushBuffersFile ordinal context. R-comp does
  **not** adopt Xenia's flush success stub.
- src/xenia/vfs/devices/host_path_entry.cc (blob
  8d1025d4dac7c0dcb3c291c9d69087b610094907): host-path timestamps,
  normal/read-only attributes and 512-byte logical allocation rounding.
- src/xenia/vfs/devices/host_path_device.h (blob
  fa8482ce2d1b99e7123b8b5bff27fa2fb9312bca) is evidence for logical sector
  size, but its fixed capacity/name values are intentionally **not** copied
  into R-comp.

Microsoft's public WDM/NTIFS documentation is the independent structure
reference:

- NtQueryInformationFile:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryinformationfile
- FILE_STANDARD_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_standard_information
- FILE_NETWORK_OPEN_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_network_open_information
- FILE_FS_VOLUME_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_fs_volume_information
- FILE_FS_SIZE_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_fs_size_information
- FILE_FS_ATTRIBUTE_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_fs_attribute_information
- FILE_BASIC_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_basic_information
- FILE_RENAME_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
- FILE_DISPOSITION_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_disposition_information
- FILE_END_OF_FILE_INFORMATION:
  https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_end_of_file_information

The export table used by XenonRecomp is checked at registration time:
NtFlushBuffersFile=0xDB, NtQueryFullAttributesFile=0xE7,
NtQueryInformationFile=0xE8, NtQueryVolumeInformationFile=0xEF,
NtSetInformationFile=0xF7 and NtWriteFile=0xFF.

## Implemented file information

NtQueryInformationFile validates the IOSB, class, minimum structure length,
output range and file handle before publishing output. Only the actual
structure extent is cleared/written; extra caller bytes are preserved.

Supported classes are:

| Xbox class | Value | Output |
| --- | ---: | --- |
| Basic | 4 | four FILETIME values + ULONG attributes + padding (40 bytes) |
| Standard | 5 | allocation, EOF, link count, delete/directory flags (24 bytes) |
| Internal | 6 | host inode identifier for the opened file (8 bytes) |
| Position | 14 | current R-comp file position (8 bytes) |
| Alignment | 17 | byte alignment (0) |
| Allocation | 19 | R-comp logical allocation size, rounded to 512 bytes |
| NetworkOpen | 34 | timestamps, allocation, EOF and attributes (56 bytes) |

Known Xbox information classes outside that subset return
STATUS_NOT_SUPPORTED; values outside the Xbox enum return
STATUS_INVALID_INFO_CLASS. A short supported buffer returns
STATUS_INFO_LENGTH_MISMATCH. Failure paths do not manufacture metadata.

NtQueryFullAttributesFile resolves the supplied Xbox ANSI path through the
same confined VFS and returns the 56-byte network-open layout. Relative
RootDirectory resolution is not implemented and traps rather than pretending
that the root was ignored.

The Basic layout deserves an explicit ABI note: the 40-byte Xbox caller used
by GTA IV stores FileAttributes with a 32-bit stw at offset 32. R-comp
therefore serializes/parses the documented ULONG at +32 and leaves +36 as
padding. Treating that field as a 64-bit integer would reject the real caller.

## Writable mounts, NtWriteFile and create/open

Mounts are read-only unless explicitly configured with
MountAccess::ReadWrite. game: and d: are hard-denied as writable devices inside
VFS, regardless of caller access or create disposition.

The full VFS open API supports Xbox dispositions Open, Create, OpenIf,
Overwrite and OverwriteIf, returning Opened/Created/Overwritten actions for
IO_STATUS_BLOCK.Information. Regular-file creation uses real
open/O_CREAT/O_EXCL/O_TRUNC; directory creation uses real mkdir.
FILE_WRITE_THROUGH causes real fsync after mutations.
FILE_NO_INTERMEDIATE_BUFFERING enforces 512-byte offset/length alignment in
GuestFile. The buffer address is not checked: the console reads into
unaligned buffers on such handles (Halo 3 reads the 0x3000-byte header of each
map into an unaligned buffer; the old address rule failed it with
STATUS_INVALID_PARAMETER and the title ended on the dirty-disc screen). The
host file is opened without O_DIRECT, which would require an aligned host
buffer; cache bypass is not observable by the title.

NtWriteFile is synchronous in the current R-comp runtime. It preflights the
IOSB as ReadWrite, the source buffer as Read, and ByteOffset as Read before any
Event state change or host write. It accepts:

- null ByteOffset or -2: current file position;
- -1: append at descriptor EOF under the GuestFile mutex;
- nonnegative explicit offsets.

Writes use real pwrite, update the runtime file position by actual transferred
bytes, and publish the final IOSB before signalling an optional real runtime
Event. File access is validated before Event reset, so a read-only handle
failure cannot consume an existing signal.

An APC routine (ReadFileEx / WriteFileEx) on NtReadFile, NtWriteFile,
NtReadFileScatter or NtWriteFileGather is NT's I/O completion APC: after the
synchronous transfer, `Routine(ApcContext, IoStatusBlock, 0)` is queued as a
user APC to the calling guest thread and runs at its next user-mode alertable
wait. xapi passes its completion thunk with the low bit set (Halo 3:
0x8259F900 | 1); the routine run is the 4-byte-aligned address. A failed
request (NT_ERROR status) queues nothing. On a file associated with a
completion port an APC routine is STATUS_INVALID_PARAMETER.

## Failed transfers are reported

`NtReadFile`, `NtWriteFile`, `NtReadFileScatter` and `NtWriteFileGather`
print one line per failed call (first 64 of the process), except a read at or
beyond the end of the file, which is normal:
`RCOMP-IO <export> failed path=<guest path> offset=<offset>[(current)] length=
status= transferred= size= lr=`. A title that only checks a hash or a byte
count later (Halo 3's localization check) would otherwise fail without a trace.
A host `pread` error additionally prints `rcomp-rt: pread(<path>) failed errno=`.

## NtSetInformationFile

The production subset is intentionally limited to operations with real runtime
or host effects:

| Xbox class | Value | Production effect |
| --- | ---: | --- |
| Basic | 4 | last-access/last-write via futimes; attributes persist in VFS metadata and READONLY changes host write bits via fchmod; nonzero creation/change time is unsupported |
| Rename | 10 | confined same-mount real rename, optional ReplaceIfExists |
| Disposition | 13 | delete-on-close; real unlink/rmdir after the last guest handle closes |
| Position | 14 | updates the mutex-protected GuestFile current position |
| EndOfFile | 20 | real ftruncate on writable regular files |
| Completion | 30 | associates the file object with an I/O completion port and key (`KERNEL_IMPORTS_20261003.md`, "File completion ports") |

FileModeInformation (16) is not advertised as a successful mutation.
FileAllocationInformation (19) returns STATUS_NOT_SUPPORTED: the current PS5
SDK declares posix_fallocate but the linked PS5 libraries do not export a
backed allocation primitive, and allocation is not faked with EOF/truncate.

The GTA IV static callsites motivating this subset directly use Basic, Rename,
Disposition, Allocation and EOF. Its observed Basic callsites set last-write
time or attributes; no observed call requires setting creation/change time.

## Metadata source and path safety

EOF, inode/link count and timestamps come from fstat on an open descriptor.
The path-only query also opens the confined target with O_NOFOLLOW and then
uses fstat, so its final component cannot escape through a symlink. Platforms
with st_birthtime use the real host creation time; hosts without it fall back
to POSIX st_ctime, whose creation-time limitation is explicit. The change field
follows the write timestamp, matching the pinned host-path reference.
Allocation is R-comp's 512-byte logical allocation contract.

Read/write opens, creates, renames and metadata queries remain confined to the
canonical mount root and reject symlink escape. Creation canonicalizes the
existing parent and opens that directory with O_NOFOLLOW|O_DIRECTORY before
creating the leaf. Traversal, host absolute paths, NT device paths, illegal
components and paths over 255 bytes are rejected before host mutation. The
only NT device paths that resolve are the hard drive's, when a title has one
(HDD.md).

Device names are case-insensitive. On host builds where dirent enumeration is
available, existing path components use an exact lookup first and then an
ASCII case-insensitive component walk. An ambiguous case-fold match is
rejected. The PS5 build does not depend on directory enumeration, so mutable
save paths currently require exact host spelling for pre-existing components
when no package/mutable case-fold index is available.

runtime/src/io_next.cpp now contains only the future manifest-backed directory
index. The obsolete IO-next writer and NtWrite adapter were removed after
promotion into VFS/HLE. The retained index synthesizes parent entries, rejects
case-fold collisions, supplies stable per-directory indexes and matches ASCII
wildcards without opendir; it is not yet registered as NtQueryDirectoryFile.

## Volume information

R-comp does not invent an Xbox volume serial, creation time or label for a
plain host-directory mount. FileFsVolumeInformation therefore returns
STATUS_NOT_SUPPORTED until an actual package/volume identity is configured.
FileFsDeviceInformation is likewise not implemented.

For FileFsSizeInformation, supported host builds call fstatvfs on the actual
open descriptor. If the reported allocation unit is a multiple of the modeled
512-byte logical sector, the total/available units and sectors per allocation
unit are returned. The current PS5 SDK exposes the statvfs header but its
shipped libraries do not export fstatvfs, so PS5 deliberately leaves this
class unsupported until R-comp has a package-backed virtual-volume capacity.
Unavailable or unrepresentable capacity returns STATUS_NOT_SUPPORTED rather
than placeholder values.

FileFsAttributeInformation reports R-comp's virtual filesystem name RCOMP, no
unproven filesystem feature flags, and the enforced 255-byte path limit as its
component limit. Its variable-length name is bounds-checked: buffers that
contain the fixed 12-byte header but not the full name get
STATUS_BUFFER_OVERFLOW with Information=12.

NtFlushBuffersFile validates the real file handle. A read-only handle has no
dirty R-comp file data, so completing its empty flush is a real success.
Writable descriptors call fsync and propagate host I/O failures.

The hard-drive utility partitions are the exception to the rules above: their
volumes have an identity (the FATX superblock serial), a FATX capacity
computed from the partition layout, the name "FATX" and a 42-byte component
limit; their raw devices answer the disk IOCTLs. See HDD.md.

## Validation

Focused production validation is in build/runtime-io-integrate-20260929.
rt_test_io covers exact big-endian query bytes, writable dispositions,
case-folded parents, symlink/traversal escape, real bytes on disk, source-page
Read-only acceptance, Event/IOSB ordering, guest-fault preservation, explicit
and append offsets, read-only write rejection, Position/EOF/Basic/Rename/
Disposition semantics, collision/replace behavior and delete-on-close. The
same test passes with RCOMP_VFS_FORCE_LEXICAL=1.

rt_test_vfs and rt_file_completion also pass against the integrated VFS. The
IO/VFS/test objects additionally pass a Clang
-fsanitize=undefined -fsanitize-trap=undefined build in
build/runtime-io-integrate-trap-20260929.

Current sources compile with the PS5 toolchain using
-Wall -Wextra -Werror -fno-exceptions -fno-rtti into
build/runtime-io-integrate-direct-ps5-20260929. Console execution remains
**NOT TESTED** and belongs to PRIME.

## Directory enumeration (NtQueryDirectoryFile)

Found by the actual GTA IV PS5 entry (`NtQueryDirectoryFile` at `0x829A9484`
on an already opened directory handle). `GuestFile::query_directory` snapshots
the host directory on the first call (or `RestartScan`), sorts names, skips `.`
and `..`, applies the DOS wildcard pattern (`*`, `?`, case-insensitive; the
pattern is taken from the first call of a scan) and yields one entry per call.
`NtQueryDirectoryFile` (0xE4) has no class parameter (the eighth argument is the name pattern, the ninth `RestartScan`) and always returns the `FileDirectoryInformation` layout:
64-byte header (`NextEntryOffset`=0, `FileIndex`=0, four FILETIMEs, EndOfFile,
AllocationSize, attributes, name length) followed by the ANSI name. Results:
STATUS_SUCCESS, STATUS_NO_SUCH_FILE (nothing matched), STATUS_NO_MORE_FILES
(exhausted), STATUS_INFO_LENGTH_MISMATCH (buffer under 64 bytes),
STATUS_BUFFER_OVERFLOW (name does not fit; the entry is consumed). Other
APC routines stop with an explicit unimplemented diagnostic.
