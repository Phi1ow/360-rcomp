// Supported xboxkrnl object-manager subset for real R-comp runtime objects.
#pragma once

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// Registers NtDuplicateObject, ObReferenceObjectByHandle, ObReferenceObject,
// and ObDereferenceObject. ExThreadObjectType storage is a separate bootstrap
// step via register_thread_object_type_variable(), before XEX import binding.
// See runtime/docs/THREAD_OBJECTS.md for the deliberately bounded ABI.
Status register_xboxkrnl_object_hle();

}  // namespace rcomp::rt
