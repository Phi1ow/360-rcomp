// Optional, bounded render-state diagnostics. All calls belong to the CP
// thread; the candidate patch compiles them out unless explicitly enabled.
#pragma once
#ifndef RCOMP_XENOS_RENDER_DIAGNOSTICS
#define RCOMP_XENOS_RENDER_DIAGNOSTICS 0
#endif

#if RCOMP_XENOS_RENDER_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, rcomp_render_diagnostics);
REXCVAR_DECLARE(int32_t, rcomp_render_diag_start_frame);
REXCVAR_DECLARE(int32_t, rcomp_render_diag_frames);

namespace rcomp::xenos {
enum class RenderDiagnosticKind {
  Draw, Targets, Resolve, Transfer, Dump, SharedBarrier, Clear, StencilClear,
  HostDepthStore, ResolveCopy, DumpDispatch, TransferDraw, Count
};
struct RenderDiagnosticKey {
  std::array<uint32_t, 96> words{};
  uint32_t length = 0;
  void Add(uint32_t word) { if (length < words.size()) words[length++] = word; }
  void Add64(uint64_t word) { Add(uint32_t(word)); Add(uint32_t(word >> 32)); }
  void AddFloat(float value) {
    uint32_t word;
    std::memcpy(&word, &value, sizeof(word));
    Add(word);
  }
  bool operator==(const RenderDiagnosticKey& other) const {
    return length == other.length && words == other.words;
  }
};

// Exact state comparison, no hashes, no unbounded containers or allocations.
class RenderDiagnosticGate {
 public:
  static constexpr uint32_t kCapacity = 128;
  // These cvars require restart and are set before GPU startup. Avoid cvar
  // access and key construction outside the selected window.
  RenderDiagnosticGate()
      : enabled_(REXCVAR_GET(rcomp_render_diagnostics)),
        first_(uint64_t(std::max(REXCVAR_GET(rcomp_render_diag_start_frame), 0))),
        frames_(uint64_t(std::clamp(REXCVAR_GET(rcomp_render_diag_frames), 1, 32))) {}
  bool InWindow(uint64_t frame) const {
    return enabled_ && frame >= first_ && frame - first_ < frames_;
  }
  static constexpr uint32_t Limit(RenderDiagnosticKind kind) {
    return kind == RenderDiagnosticKind::Draw || kind == RenderDiagnosticKind::Transfer ? 128 : 64;
  }
  uint32_t sequence() const { return sequence_; }
  bool Accept(RenderDiagnosticKind kind, uint64_t frame, const RenderDiagnosticKey& key) {
    if (!InWindow(frame)) return false;
    const uint32_t index = uint32_t(kind);
    if (!announced_) {
      announced_ = true;
      std::fprintf(stderr, "RCOMP-RENDER-DIAG begin frame=%llu frames=%llu structural_draw=128 transfer=128 other=64 max_lines=1805\n",
                   (unsigned long long)first_, (unsigned long long)frames_);
    }
    for (uint32_t i = 0; i < counts_[index]; ++i) if (keys_[index][i] == key) return false;
    if (counts_[index] == Limit(kind)) {
      if (!limited_[index]) {
        limited_[index] = true;
        std::fprintf(stderr, "RCOMP-RENDER-DIAG limit kind=%u states=%u\n", index, Limit(kind));
      }
      return false;
    }
    keys_[index][counts_[index]++] = key;
    ++sequence_;
    return true;
  }
 private:
  static constexpr uint32_t kKinds = uint32_t(RenderDiagnosticKind::Count);
  std::array<std::array<RenderDiagnosticKey, kCapacity>, kKinds> keys_{};
  std::array<uint32_t, kKinds> counts_{};
  std::array<bool, kKinds> limited_{};
  bool announced_ = false;
  uint32_t sequence_ = 0;
  const bool enabled_;
  const uint64_t first_;
  const uint64_t frames_;
};
inline RenderDiagnosticGate& RenderDiagnostics() {
  static RenderDiagnosticGate gate;
  return gate;
}
}  // namespace rcomp::xenos
#endif
