// Console diagnostic of the save services (app/src/content_selftest.cpp). Called by TitleRuntime::Create in
// RCOMP_M6_CONTENT_SELFTEST builds, after the main module is finalized and before the guest starts.
#pragma once

#include <stdio.h>

namespace rcomp::app {
// Logs one RCOMP-CONTENT-SELFTEST line per step and a final PASS/FAIL line; true when every step passed.
bool run_content_selftest(FILE* log);
}  // namespace rcomp::app
