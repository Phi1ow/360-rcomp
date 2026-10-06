// Pins the PPCContext layout the runtime was compiled against (XenonRecomp
// @ddd128b ppc_context.h, no PPC_CONFIG_* flag). If the generated code's
// ppc_config.h enables a PPC_CONFIG_* option that changes the layout, the
// runtime must be rebuilt with the same header, and these values updated
// deliberately. A mismatch between translation units is an ODR/ABI break
// that no test would otherwise catch.
#include <stddef.h>

#include "rcomp/runtime/guest_context.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(alignof(PPCContext) == 64, "PPCContext alignment changed");
static_assert(sizeof(PPCContext) == 2688, "PPCContext size changed (PPC_CONFIG_* mismatch?)");
static_assert(offsetof(PPCContext, r3) == 0, "PPCContext layout changed");
static_assert(offsetof(PPCContext, r1) == 16, "PPCContext layout changed");
static_assert(offsetof(PPCContext, r13) == 104, "PPCContext layout changed");
static_assert(offsetof(PPCContext, lr) == 256, "PPCContext layout changed");
static_assert(offsetof(PPCContext, ctr) == 264, "PPCContext layout changed");
static_assert(offsetof(PPCContext, reserved) == 280, "PPCContext layout changed");
static_assert(offsetof(PPCContext, fpscr) == 324, "PPCContext layout changed");
static_assert(offsetof(PPCContext, f1) == 336, "PPCContext layout changed");
static_assert(offsetof(PPCContext, v0) == 592, "PPCContext layout changed");
#pragma clang diagnostic pop

namespace rcomp::rt {
// Lets an integration test compare against sizeof(PPCContext) as seen by
// generated code.
size_t ppc_context_size_seen_by_runtime() { return sizeof(PPCContext); }
}  // namespace rcomp::rt
