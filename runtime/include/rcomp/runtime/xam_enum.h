// XAM enumerator objects (owner: Agent 3, runtime/). See docs/XAM.md.
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {
Status register_xam_enum_hle();
void reset_xam_enum();

// ObDereferenceObject on the "private enumeration structure" returned by
// XamGetPrivateEnumStructureFromHandle. Status::NotFound when `body` is not an
// enumerator body (the caller then tries the other object kinds).
Status dereference_enum_body(uint32_t body);

// Completes a guest XOVERLAPPED at once: InternalLow = result, InternalHigh =
// length, extended error (+0x18) = HRESULT_FROM_WIN32(result), hEvent (+0x0C)
// signalled. False when the overlapped is not writable. A completion routine
// (+0x10) is fatal: no title has needed one and running it is not modelled.
bool xam_complete_overlapped(uint32_t overlapped, uint32_t result, uint32_t length);
}  // namespace rcomp::rt
