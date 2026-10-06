// R-comp Xenos instrumentation is opt-in. PC sampling has a separate owner.
#pragma once

#ifndef RCOMP_XENOS_DIAGNOSTICS
#define RCOMP_XENOS_DIAGNOSTICS 0
#endif
#ifndef RCOMP_XENOS_PROFILE_TIMINGS
#define RCOMP_XENOS_PROFILE_TIMINGS 0
#endif

#if RCOMP_XENOS_PROFILE_TIMINGS
#include <atomic>
#include <cstdint>
#include <x86intrin.h>
// Slot pairs (count, TSC cycles): 0 draw, 2 pipeline, 4 interrupt, 6 fence
// wait, 8 queue submit, 10 CP idle, 12 CPU-write invalidation, 14 shared
// memory upload, 16 texture load, 20 primary buffer, 22 swap, 24 WAIT_REG_MEM,
// 26 queue acquisition + submit; 18 counts uploaded bytes; 28 and 30 belong to
// the display thread. Stages of a draw, nested inside slot 0: 32 render target
// cache update (transfers included), 34 render target transfers and resolve
// clears, 36 resolve copy (IssueCopy), 38 constants and descriptor bindings,
// 40 texture requests, 42 pipeline lookup, 44 primitive processing, 46 samplers
// and shader modifications, 48 vertex buffers, shared memory, render pass entry and the draw call,
// 50 everything before the primitive loop, 52 host viewport, 54 dynamic state, 56 system constants.
// With the overlapped replay (rcomp_async_submit): 58 the command processor waits for the replay
// thread to have submitted a submission; 60 and 62 are the replay thread's own time replaying
// deferred commands and ending and submitting the command buffer (not command processor time).
// Finer stages of a draw, measured on one draw in eight only (every scope slows a profile build): slots 64 and up, counts and
// cycles are of the sampled draws, multiply by 8: 64 texture cache base request, 66 3D-as-2D views, 68 texture usage transitions,
// 70 constants upload, 72 texture descriptors snapshot, 74 descriptor set write, 76 descriptor set binding, 78 render target
// cache base update, 80 render target post (render pass, framebuffer, transitions), 82 vertex buffer residency, 84 shared memory
// use and barriers, 86 draw command, 88 shader modifications and translation, 90 samplers.
extern "C" std::atomic<uint64_t> rcomp_prof[128];
// For spans that are not a C++ scope: RCOMP_PROF_STAMP(t) at the start, RCOMP_PROF_ADD(slot, t) at the end.
#define RCOMP_PROF_STAMP(var) const uint64_t var = __rdtsc()
#define RCOMP_PROF_ADD(slot, var)                                                  \
  do {                                                                             \
    rcomp_prof[slot].fetch_add(1, std::memory_order_relaxed);                      \
    rcomp_prof[(slot) + 1].fetch_add(__rdtsc() - (var), std::memory_order_relaxed); \
  } while (0)
// A counter pair instead of a timer: one more event and n units. The second element is scaled by 1e6 so that the RCOMP-PROFT dump, which
// divides it by 1e6 for TSC cycles, shows n. Slots from 100: 100/102/104/106 shared memory upload bytes by consumer (vertex buffers, index
// buffers, textures, other: uploads and bytes), 108/110/112/114 the bytes of those whose 4 KiB pages equal the last upload of the same page,
// 116 draws and the draws with no state register written since the previous draw, 118/120 fetch constant writes and the unchanged ones (writes and dwords),
// 122/124 the same for float constants, 126 single register writes and the unchanged ones.
#define RCOMP_PROF_COUNT(slot, n)                                                          \
  do {                                                                                     \
    rcomp_prof[slot].fetch_add(1, std::memory_order_relaxed);                              \
    rcomp_prof[(slot) + 1].fetch_add(uint64_t(n) * 1000000u, std::memory_order_relaxed);   \
  } while (0)
#else
#define RCOMP_PROF_STAMP(var) ((void)0)
#define RCOMP_PROF_ADD(slot, var) ((void)0)
#define RCOMP_PROF_COUNT(slot, n) ((void)0)
#endif

namespace rcomp::xenos {
#if RCOMP_XENOS_PROFILE_TIMINGS
// Both are written and read by the command processor thread only. g_upload_tag: the consumer of the next shared memory upload (0 vertex
// buffers, 1 index buffers, 2 textures, 3 other); g_fine: the draw being processed is one of the finely measured ones (FineScope).
inline int g_upload_tag = 3;
inline bool g_fine = false;
#endif
#if RCOMP_XENOS_PROFILE_TIMINGS
inline bool FineEnabled() { return g_fine; }
#else
inline constexpr bool FineEnabled() { return false; }
#endif
// A scope of the finer stages: it measures only when `enabled` (the draw is one of the sampled ones).
class FineScope {
 public:
#if RCOMP_XENOS_PROFILE_TIMINGS
  FineScope(int slot, bool enabled) : slot_(enabled ? slot : -1), start_(enabled ? __rdtsc() : 0) {}
  ~FineScope() { End(); }
  // Ends the measurement early (the destructor then does nothing).
  void End() {
    if (slot_ >= 0) {
      rcomp_prof[slot_].fetch_add(1, std::memory_order_relaxed);
      rcomp_prof[slot_ + 1].fetch_add(__rdtsc() - start_, std::memory_order_relaxed);
      slot_ = -1;
    }
  }

 private:
  int slot_;
  uint64_t start_;
#else
  FineScope(int, bool) {}
  void End() {}
#endif
};
class ProfScope {
 public:
#if RCOMP_XENOS_PROFILE_TIMINGS
  explicit ProfScope(int slot) : slot_(slot), start_(__rdtsc()) {}
  ~ProfScope() {
    rcomp_prof[slot_].fetch_add(1, std::memory_order_relaxed);
    rcomp_prof[slot_ + 1].fetch_add(__rdtsc() - start_, std::memory_order_relaxed);
  }
 private:
  int slot_;
  uint64_t start_;
#else
  explicit ProfScope(int) {}
#endif
};
}  // namespace rcomp::xenos
