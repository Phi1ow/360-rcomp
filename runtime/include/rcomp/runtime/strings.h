// Xbox/NT string and character HLE (owner: runtime strings tranche).
#pragma once

#include <stdint.h>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

// R-comp's deterministic ANSI compatibility profile for this title tranche.
// This is Windows-1252 (Western European), not a claim that every Xbox 360
// locale uses CP1252. See runtime/docs/STRINGS.md for the pinned mapping data.
constexpr uint32_t kAnsiCodePage = 1252;

// Deterministic single-code-unit helpers for other runtime services (for
// example guest formatting). Both use the same CP1252/best-fit tables as the
// Rtl* HLE functions in hle_xboxkrnl_strings.cpp.
uint8_t ansi_from_unicode(uint16_t value);
uint16_t unicode_from_ansi(uint8_t value);

Status register_xboxkrnl_strings_hle();

}  // namespace rcomp::rt
