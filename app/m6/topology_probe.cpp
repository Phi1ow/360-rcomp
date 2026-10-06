// Diagnostic only (RCOMP_M6_TOPOLOGY_PROBE): which logical CPUs the title may run on and how far apart they are.
// The console exposes no topology to the title, so the probe measures it: a thread pinned to CPU i and one pinned
// to CPU j pass a cache line back and forth; SMT siblings answer in about 20-35 ns, cores that share an L3 slice in
// 40-70 ns and cores of different core complexes in 100 ns or more. The CPUID identification of every CPU is
// printed beside it (the topology fields of an unmodified CPUID, if the console passes them through).
// Output: RCOMP-TOPO lines on stdout. Nothing here runs in a release title.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <atomic>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#if defined(__PROSPERO__)
extern "C" int scePthreadSetaffinity(pthread_t thread, uint64_t mask);
#endif

namespace {
constexpr int kMaxCpus = 16;
constexpr int kRoundTrips = 20000;

int pin_self(int cpu) {
#if defined(__PROSPERO__)
    return scePthreadSetaffinity(pthread_self(), uint64_t(1) << cpu);
#else
    (void)cpu;
    return -1;
#endif
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct CpuInfo {
    int cpu = 0;
    int pin_rc = 0;
    uint32_t apic_id = 0;        // CPUID.1:EBX[31:24]
    uint32_t x2apic_id = 0;      // CPUID.0B.0:EDX
    uint32_t amd_ext_apic = 0;   // CPUID.8000001E:EAX
    uint32_t amd_core_info = 0;  // CPUID.8000001E:EBX (core id, threads per core - 1)
    uint32_t l3_sharing = 0;     // CPUID.8000001D (subleaf 3):EAX[25:14] + 1, logical CPUs sharing the cache
    uint32_t brand_family = 0;   // CPUID.1:EAX
};

void* cpu_info_thread(void* arg) {
    CpuInfo* info = static_cast<CpuInfo*>(arg);
    info->pin_rc = pin_self(info->cpu);
#if defined(__x86_64__)
    uint32_t a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        info->brand_family = a;
        info->apic_id = b >> 24;
    }
    if (__get_cpuid_count(0xB, 0, &a, &b, &c, &d)) info->x2apic_id = d;
    if (__get_cpuid(0x8000001E, &a, &b, &c, &d)) {
        info->amd_ext_apic = a;
        info->amd_core_info = b;
    }
    if (__get_cpuid_count(0x8000001D, 3, &a, &b, &c, &d)) info->l3_sharing = ((a >> 14) & 0xFFF) + 1;
#endif
    return nullptr;
}

struct alignas(128) PingPong {
    std::atomic<uint32_t> turn{0};
    char pad[124];
};

struct PairArg {
    PingPong* line;
    int cpu;
    uint32_t mine;  // the value of `turn` this thread waits for
    int pin_rc = 0;
    uint64_t elapsed_ns = 0;
};

void* pair_thread(void* arg) {
    PairArg* p = static_cast<PairArg*>(arg);
    p->pin_rc = pin_self(p->cpu);
    // both threads must be running before the clock starts: a short rendezvous on the line itself
    const uint64_t start = now_ns();
    for (int i = 0; i < kRoundTrips; ++i) {
        while (p->line->turn.load(std::memory_order_acquire) != p->mine) {
        }
        p->line->turn.store(p->mine ^ 1u, std::memory_order_release);
    }
    p->elapsed_ns = now_ns() - start;
    return nullptr;
}

// Nanoseconds per round trip between two CPUs, or -1 if one of them cannot be used.
double round_trip_ns(int a, int b) {
    static PingPong line;
    line.turn.store(0, std::memory_order_relaxed);
    PairArg first{&line, a, 0}, second{&line, b, 1};
    pthread_t t1, t2;
    if (pthread_create(&t1, nullptr, pair_thread, &first) != 0) return -1;
    if (pthread_create(&t2, nullptr, pair_thread, &second) != 0) {
        line.turn.store(0, std::memory_order_release);
        pthread_join(t1, nullptr);
        return -1;
    }
    pthread_join(t1, nullptr);
    pthread_join(t2, nullptr);
    if (first.pin_rc != 0 || second.pin_rc != 0) return -1;
    return double(first.elapsed_ns + second.elapsed_ns) / 2.0 / kRoundTrips;
}
}  // namespace

extern "C" void rcomp_topology_probe() {
    CpuInfo info[kMaxCpus];
    bool usable[kMaxCpus] = {};
    int count = 0;
    for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
        info[cpu].cpu = cpu;
        pthread_t t;
        if (pthread_create(&t, nullptr, cpu_info_thread, &info[cpu]) != 0) continue;
        pthread_join(t, nullptr);
        usable[cpu] = info[cpu].pin_rc == 0;
        count += usable[cpu] ? 1 : 0;
        printf("RCOMP-TOPO cpu=%d pin_rc=%d apic=%u x2apic=%u amd_ext_apic=%u amd_core=0x%x l3_sharing=%u cpuid1=0x%x\n", cpu,
               info[cpu].pin_rc, info[cpu].apic_id, info[cpu].x2apic_id, info[cpu].amd_ext_apic, info[cpu].amd_core_info,
               info[cpu].l3_sharing, info[cpu].brand_family);
    }
    printf("RCOMP-TOPO usable=%d\n", count);
    // the matrix: round trip in ns, row = first CPU, column = second CPU
    for (int a = 0; a < kMaxCpus; ++a) {
        if (!usable[a]) continue;
        char row[kMaxCpus * 8 + 1];
        int len = 0;
        for (int b = 0; b < kMaxCpus; ++b) {
            if (!usable[b] || b <= a) {
                len += snprintf(row + len, sizeof row - size_t(len), "%7s ", usable[b] && b != a ? "." : "-");
                continue;
            }
            const double ns = round_trip_ns(a, b);
            len += snprintf(row + len, sizeof row - size_t(len), "%7.1f ", ns);
        }
        printf("RCOMP-TOPO row cpu=%d %s\n", a, row);
        fflush(stdout);
    }
}
