// Host random source shared by the HLE services that hand random bytes to a title (owner: Agent 3,
// runtime/): NetDll_XNetRandom, NetDll_XNetCreateKey and XeCryptRandom. Not a public interface.
//
// RDRAND first (the PS5's Zen 2 and every supported host have it); a result is rejected when the
// instruction reports a known-broken generator (all-ones words). Hosts fall back to getentropy().
// False when no source can deliver the bytes: callers report that as a platform fatal, never as
// predictable or zero-filled "random" data.
#pragma once

#include <stdint.h>
#include <string.h>

#include <algorithm>

#if !defined(__PROSPERO__)
#include <sys/random.h>
#endif

namespace rcomp::rt {

#if defined(__x86_64__)
inline bool host_cpu_has_rdrand() {
    uint32_t a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1u;
}

inline bool host_rdrand64(uint64_t* out) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint64_t value = 0;
        uint8_t ok = 0;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
        if (ok && value != ~uint64_t(0)) {
            *out = value;
            return true;
        }
    }
    return false;
}
#endif

inline bool host_random_bytes(uint8_t* out, uint32_t size) {
#if defined(__x86_64__)
    static const bool has_rdrand = host_cpu_has_rdrand();
    if (has_rdrand) {
        uint32_t done = 0;
        while (done < size) {
            uint64_t value = 0;
            if (!host_rdrand64(&value)) break;
            const uint32_t n = std::min<uint32_t>(8, size - done);
            memcpy(out + done, &value, n);
            done += n;
        }
        if (done == size) return true;
    }
#endif
#if !defined(__PROSPERO__)
    for (uint32_t done = 0; done < size;) {
        const uint32_t n = std::min<uint32_t>(256, size - done);  // getentropy's per-call limit
        if (getentropy(out + done, n) != 0) return false;
        done += n;
    }
    return true;
#else
    return false;
#endif
}

}  // namespace rcomp::rt
