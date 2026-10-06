// Xbox 360 file metadata/query HLE services.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers the supported file metadata/query/mutation exports, including
// NtWriteFile and the real NtSetInformationFile subset documented in
// runtime/docs/IO_METADATA.md.
Status register_xboxkrnl_io_hle();

}  // namespace rcomp::rt
