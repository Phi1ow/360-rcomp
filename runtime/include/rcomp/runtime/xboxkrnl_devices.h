// xboxkrnl device, share-access, symbolic-link, L2-lock and console-key HLE
// (owner: Agent 3, runtime/). Implementation: src/hle_xboxkrnl_devices.cpp,
// which documents the Xbox 360 contract and the subset of each export.
#pragma once

#include <stdint.h>

#include <string>
#include <string_view>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers IoCheckShareAccess, IoSetShareAccess, IoRemoveShareAccess,
// IoCreateDevice, IoDeleteDevice, IoDismountVolume,
// IoDismountVolumeByFileHandle, KeLockL2, KeUnlockL2, NtDeviceIoControlFile,
// NtReadFileScatter, ObCreateSymbolicLink, ObDeleteSymbolicLink,
// ObIsTitleObject, StfsCreateDevice, StfsControlDevice,
// XeKeysConsolePrivateKeySign and XeKeysConsoleSignatureVerification.
// Called by register_xboxkrnl_hle().
Status register_xboxkrnl_device_hle();

// ---- Device object bodies (IoCreateDevice) for the object manager ---------
// ObReferenceObject / ObDereferenceObject accept the guest DEVICE_OBJECT
// address returned by IoCreateDevice. NotFound: `body` is not a live device
// created by IoCreateDevice in this runtime. Conflict: dereference without a
// matching reference (underflow) or reference-count overflow.
Status reference_device_object(uint32_t body);
Status dereference_device_object(uint32_t body);

// ---- Title symbolic links (ObCreateSymbolicLink) for path resolution ------
// The object manager keeps the links a title creates under "\??\" (the DOS
// device directory). The VFS does not consult them yet: honouring them is a
// change in Vfs path resolution (see the report delivered with this file).
//
// find_title_drive_link("cache:") / ("cache") / ("\??\cache:"): Ok and the
// link target exactly as the title gave it, NotFound when the title created
// no such link.
Status find_title_drive_link(std::string_view drive, std::string* target);
// "x:\a\b" or "\??\x:\a\b" whose drive "x:" is a title link -> target + "\a\b"
// (one level; a target that is itself a link is resolved by calling again).
// NotFound when the drive is not a title link; PathRejected when the path has
// no "drive:" prefix.
Status rewrite_path_through_title_links(std::string_view guest_path, std::string* rewritten);

}  // namespace rcomp::rt
