// The Xbox 360 hard drive the runtime presents to a title (owner: Agent 3, runtime/).
// Design, layout and sources: runtime/docs/HDD.md.
//
// A title that probes the drive (the XDK utility-drive library linked into Halo 3, GTA IV,
// Episodes from Liberty City, God of War II...) opens "\Device\Harddisk0\Partition0" and the two
// utility ("cache") partitions "\Device\Harddisk0\Cache0" / "Cache1" as raw devices, formats them
// with the FATX formatter, then uses them as file systems: "\Device\Harddisk0\CacheN\..." (FATX) or
// a device "\Device\cacheN" it creates itself (the XDK's secure file cache driver).
//
// All of it is backed by one writable host directory per title:
//   <root>/raw/system/   disk bytes [0, 0x80000) (system area before the first partition)
//   <root>/raw/cache0/   raw bytes of utility partition 0 (sparse 64 KiB chunk files)
//   <root>/raw/cache1/   raw bytes of utility partition 1
//   <root>/cache0/       the file system of utility partition 0 (real host files)
//   <root>/cache1/       the file system of utility partition 1
// Raw bytes never written read as zero (a new drive).
#pragma once

#include <stdint.h>

#include <string>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Retail layout (runtime/docs/HDD.md): a 20 GB drive (IDEMA LBA count for 20 GB) whose first
// partitions are the System Cache and the Game Cache of the Free60 partition table.
constexpr uint64_t kHddSectorBytes = 512;
constexpr uint64_t kHddDiskBytes = 39091248ull * kHddSectorBytes;
constexpr uint64_t kHddSystemAreaBytes = 0x80000;
constexpr uint64_t kHddCache0Offset = 0x80000, kHddCache0Bytes = 0x80000000ull;
constexpr uint64_t kHddCache1Offset = 0x80080000ull, kHddCache1Bytes = 0xA0E30000ull;
// FATX: longest file name (Free60 FATX directory entry: 42 name bytes).
constexpr uint32_t kFatxMaxName = 42;

// Presents the drive for the current runtime lifetime, backed by `host_dir` (created with its
// layout when missing). Before guest execution, after runtime_init. AlreadyExists when this
// runtime already has a drive; NotInitialized without a runtime; NotFound when the directory
// cannot be created.
Status runtime_configure_hdd(const std::string& host_dir);

// The configured host directory of the current runtime, "" when the title has no drive.
std::string hdd_root();

// Kernel object hooks (src/hle_xboxkrnl_devices.cpp). A device the title names "\Device\cache0"
// or "\Device\cache1" with IoCreateDevice is the XDK secure file cache of utility partition 0/1:
// R-comp dispatches no IRP to the title's driver, so while the device exists the paths under it
// reach the partition's file system (the same host directory as "\Device\Harddisk0\CacheN\").
// `canonical` is the lower-case object name. Nothing happens without a drive or for other names.
void hdd_title_device_created(const std::string& canonical);
void hdd_title_device_deleted(const std::string& canonical);

}  // namespace rcomp::rt
