# Xbox 360 hard drive (utility partitions)

Owner: Agent 3 (runtime/). Code: `include/rcomp/runtime/hdd.h`, `src/hdd.cpp`,
the object-namespace part of `src/vfs.cpp`, `NtDeviceIoControlFile` and the
`IoCreateDevice`/`IoDeleteDevice` hooks in `src/hle_xboxkrnl_devices.cpp`.
Test: `tests/test_hdd.cpp` (`rt_hdd`).

## What titles do (Halo 3, PPSA88371, title 0x4D5307E6)

Read from the generated code (XenonRecomp comments) of
`inventory-v23/ppc`; addresses are guest addresses. The XDK utility-drive
library is statically linked.

1. Game code `sub_82198780` checks the four pads for a "format cache" chord,
   then calls `sub_821984D8(0)` and, if that fails, `sub_821984D8(1)`
   (format forced). `sub_821984D8` calls
   `XMountUtilityDriveEx(flags = 0x10 | (force ? 0xF : 0), BytesPerCluster =
   0x10000, FileCacheSize = 0xFF000)` (`sub_82582158` -> `sub_82581D18`), then
   `XFlushUtilityDrive` (`sub_82581930`), `GetDiskFreeSpaceEx("cache0:\")`
   (`sub_82582228`: `NtOpenFile` root, options 0x800021, then
   `FileFsSizeInformation`) and `CreateFile("cache0:\test.dat", CREATE_ALWAYS)`.
   Success sets the byte at 0x828A1EAD ("using utility drive"), which about 60
   game functions test; without it the game runs "from DVD" (the HDD-only
   features say they need a hard drive).
2. `sub_82581D18`: rejects a second mount, a file cache above 0xFF000 or a
   cluster size not multiple of 512 (ERROR_INVALID_PARAMETER). Privilege 23
   (TitleBothUtilityPartitions, set in Halo 3's XEX: system flags 0x00800600)
   selects both partitions.
   - `NtCreateFile("\Device\Harddisk0\partition0", 0xC0100000, share READ,
     FILE_OPEN, options 0x22)`; `NtReadFile` 1024 bytes at offset 2048.
   - The block is the utility-drive table: magic "Josh" (0x4A6F7368) at +0,
     an `XE_CONSOLE_SIGNATURE` (0x228 bytes) at +4 over the SHA-1 of the 88
     bytes at +0x22C (two 36-byte partition descriptors, version 1 at +0x274,
     last slot at +0x278, owner title IDs at +0x27C/+0x280), verified with
     `XeKeysConsoleSignatureVerification` (`sub_825810E0`). Invalid table or
     another owner -> the partitions are reformatted.
   - The table is rewritten (`sub_82581160`): SHA-1 + `XeKeysConsolePrivateKeySign`,
     read-modify-write of the 1024 bytes at 2048.
   - Per partition N (`sub_82581610`): when it must be formatted,
     `sub_82582DF8("\Device\Harddisk0\CacheN", 0x10000)` = the FATX formatter:
     `NtOpenFile` (0x100003, share 0, options 0x18),
     `IOCTL_DISK_GET_DRIVE_GEOMETRY` (0x70000, 8 bytes, BytesPerSector at +4),
     `IOCTL_DISK_GET_PARTITION_INFO` (0x74004, 16 bytes, PartitionLength at
     +8), then raw writes: a zero 4 KiB page at 0, the FAT from 0x1000 (FAT16
     below 65520 clusters, first entries 0xFFF8/0xFFFF), the root-directory
     cluster filled with 0xFF, and the superblock page last ("XTAF", serial =
     low word of `KeQuerySystemTime`, sectors per cluster, root cluster 1),
     then `IoDismountVolumeByFileHandle`. Then `NtOpenFile("\Device\Harddisk0\CacheN\",
     0x100001, share 3, options 0x800021)` + `FileFsSizeInformation`: the
     cluster size must be 0x10000, else STATUS_UNRECOGNIZED_VOLUME and format.
   - Privilege 11 (TitleInsecureUtilityDrive) is **not** set, so the mount
     takes the secure path: `XamTaskSchedule(sub_82581AD0)`. The task
     (`sub_82581298`, per partition) opens the raw volume
     `"\Device\Harddisk0\CacheN"` (0xC0000000, share 0, options 0x28),
     queries its `FileFsSizeInformation`, and calls the title's own secure
     file cache driver (`sub_825836A0`, function table 0x82720718 filled by
     `sub_825821E8`): `FileFsSizeInformation` (BytesPerSector <= 4096),
     `FileAlignmentInformation`, `NtCreateEvent`, `IoCreateDevice("\Device\cacheN",
     type 61)`. Then `ObCreateSymbolicLink`: `\??\cache:` -> `\Device\cache<slot>`,
     `\??\cache0:` -> `\Device\cache0`, `\??\cache1:` -> `\Device\cache1`.
     The driver serves the files through IRPs (it imports
     `IoCheckShareAccess`, `IoCompleteRequest`...) and keeps its data on the
     raw volume from offset 0x1000 on.
3. Afterwards the game uses `cache0:\cacheNNN.map`, `cache0:\preferences.dat`,
   `cache1:\autosave`, `cache1:\upload_queue`, `cache1:\webcache\...`, saved
   films (`cache1:\sf_*.blob/.film`) and so on.

The previous run failed at the first step: `partition0` was refused
(STATUS_OBJECT_NAME_INVALID, twice: the normal and the forced attempt), so no
link existed and every `cache0:`/`cache1:` path ended in
STATUS_OBJECT_PATH_NOT_FOUND (`cache1:\`, a drive root, in OBJECT_NAME_INVALID).

The XContent services already present a hard drive (`hle_xam_content.cpp`,
save root: `XamContentGetDeviceData`/`GetDeviceState` answer an HDD device),
so the "requires an Xbox 360 Hard Drive" message most likely follows the
utility-drive state above (the message text is in the map data, not in the
XEX, so the exact test was not traced).

### Other titles

GTA IV (PPSA88360), TBoGT (PPSA88361) and God of War II (PPSA88370) link the
same library ("\Device\Harddisk0\partition0", "Cache%u" strings) and reach it
from game code (call chains to the partition0 user found in their
`inventory-v23/ppc`); their XEX system flags are 0x200 (single partition,
secure path). They would mount a utility drive as soon as one exists, so the
drive is opt-in per title (`TitleConfig::hdd_root`, app/m6 `RCOMP_M6_HDD`,
default OFF): their behaviour is unchanged.

## Layout presented

| Object name | R-comp device | Bytes |
| --- | --- | --- |
| `\Device\Harddisk0\Partition0` | whole disk | 39,091,248 sectors x 512 = 20,014,718,976 |
| `\Device\Harddisk0\Cache0` | utility partition 0 ("System Cache") | offset 0x80000, length 0x80000000 |
| `\Device\Harddisk0\Cache1` | utility partition 1 ("Game Cache") | offset 0x80080000, length 0xA0E30000 |
| `\Device\Harddisk0\CacheN\...` | FATX file system of partition N | host directory |
| `\Device\cacheN\...` | the same file system, while the title device exists | host directory |

Sources and limits:
- Partition offsets/lengths: Free60 "FATX" page, retail partition table
  (System Cache 0x80000/0x80000000, Game Cache 0x80080000/0xA0E30000, both
  marked SFCX = secure file cache, which matches the XDK driver above). That
  the kernel's Cache0/Cache1 are these two, in this order, is inferred (two
  cache partitions, two slots in the table); not otherwise established. The
  table's later SysExt/SysExt2 partitions lie inside the listed Game Cache
  range; R-comp provides neither.
- Disk size: the IDEMA LBA count for a 20 GB drive (97,696,368 + 1,953,504 x
  (20 - 50) sectors). The exact LBA count of Microsoft's 20 GB retail drive is
  not established; only `IOCTL_DISK_GET_DRIVE_GEOMETRY` on Partition0 shows it.
- FATX superblock and limits: Free60 FATX ("XTAF", serial, sectors per
  cluster, root cluster; 42-byte names). FAT size and data start: the XDK
  formatter's own arithmetic (above).

## Host storage

`TitleConfig::hdd_root` (app/m6: `/app0/hdd`, beside `/app0/savedata`, which
R-comp Installer never deletes) holds:

- `raw/system/`, `raw/cache0/`, `raw/cache1/`: the raw bytes, as sparse 64 KiB
  chunk files (`<chunk index as 16 hex digits>.bin`) created on first write.
  Bytes never written read as zero, as on a new drive.
- `cache0/`, `cache1/`: the file systems, real host files.

## Behaviour

- Partition0 is the concatenation of the system area [0, 0x80000) and the two
  utility partitions (the same bytes as their own devices). A transfer
  anywhere else (SysExt, compatibility, data partitions) traps
  (RCOMP_FATAL_UNIMPLEMENTED): no title reads them.
- Raw devices: reads/writes inside the device (a start at the end is
  STATUS_END_OF_FILE, a write across it STATUS_INVALID_PARAMETER),
  FILE_NO_INTERMEDIATE_BUFFERING alignment as for files, `NtFlushBuffersFile`
  fsyncs the chunks written, `FileAlignmentInformation` = 0, the two disk
  IOCTLs (Sectors/BytesPerSector; StartingOffset/PartitionLength); any other
  IOCTL traps. Open/OpenIf only (Create = name collision), never a directory.
- A partition's file system exists while its raw bytes start with a valid
  FATX superblock. Without one, every path on it is STATUS_UNRECOGNIZED_VOLUME
  (files already in the directory stay, as unreachable data).
- Format: a raw write that changes the superblock into a valid FATX superblock
  empties the partition's host directory (a format destroys the previous file
  system; handles already open keep their host descriptors). The FAT and root
  directory the formatter writes stay raw bytes; R-comp's file system does not
  read them. One `RCOMP-HDD format cacheN` line is printed.
- Volume information of the file system and of the raw volume handle:
  FileFsSizeInformation = FATX clusters of the partition (total) and the
  clusters the directory tree does not use (files rounded to clusters, every
  directory's 64-byte entries rounded up, at least one cluster), limited on
  hosts with statvfs to the host free space; FileFsVolumeInformation = the
  superblock serial, no creation time, no label; FileFsAttributeInformation =
  "FATX", 42-byte names (longer names: STATUS_OBJECT_NAME_INVALID).
- The volume root (`\Device\Harddisk0\CacheN\`, `cache1:\` through a link)
  opens as a directory; it is never created, replaced, deleted or renamed.
  The root of a drive that is neither mounted nor linked keeps the old answer
  (STATUS_OBJECT_NAME_INVALID).
- Secure file cache: R-comp dispatches no IRP to a title driver. A device the
  title creates as `\Device\cache0` or `\Device\cache1` (the XDK's names) is
  bound to that partition's file system until `IoDeleteDevice`
  (`RCOMP-HDD title device` line): `cache0:\x` -> link -> `\Device\cache0\x`
  -> `<hdd>/cache0/x`. The title's driver still runs its control path (lock,
  flush, descriptor) and its raw writes are stored; its own on-disk format is
  not used for the files. Other device names keep the old behaviour.
- `IoDismountVolume`/`IoDismountVolumeByFileHandle` stay STATUS_NOT_SUPPORTED
  (the XDK ignores the status); the next open reads the superblock again.
- NtCreateFile/NtOpenFile options added for this path: 0x10
  FILE_SYNCHRONOUS_IO_ALERT (synchronous like 0x20; both together are
  STATUS_INVALID_PARAMETER) and 0x800000 FILE_OPEN_FOR_FREE_SPACE_QUERY.

## Status

- Host: `rt_hdd` PASS (runtime suite 67/67 in `build/runtime-hdd`).
- PS5: NOT TESTED. Expected log lines on the first Halo 3 boot:
  `RCOMP-APP hdd_root=/app0/hdd`, two `RCOMP-HDD format cacheN`, two
  `RCOMP-HDD title device \device\cacheN`.
- Known risk: Halo 3 then copies map caches into `cache0:`/`cache1:` (up to
  the partitions' 2 GiB / 2.5 GiB); the EFLC precedent (install into a
  writable cache at 24 fps) says to watch the frame rate during that copy.
