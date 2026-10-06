// Synchronisation test (Agent 6): runs the XenonRecomp translation of
// fixtures/ppc/sync/rcomp_sync.s (lwarx/stwcx. increment loop) on T host
// threads at once, each with its own PPCContext over one shared guest memory.
// Oracle: final counter == T * iterations (big-endian in guest memory).
// A lost update means the translated reservation is not atomic.
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <vector>

#include "cpu_harness.h"
#include "rcomp/guest_memory.h"

int main() {
    rcomp::GuestMemory mem;
    if (mem.reserve() != rcomp::MemStatus::Ok ||
        mem.commit(0x10000000ull, 0x10000ull, rcomp::Protect::ReadWrite) != rcomp::MemStatus::Ok)
        return 2;
    const TestCase& t = kUnits[0]->cases[0];
    const uint32_t counter = 0x10001000u;
    int failures = 0;
    for (unsigned threads : {1u, 2u, 4u, 8u}) {
        for (uint64_t iters : {1000ull, 200000ull}) {
            memset(mem.base() + counter, 0, 4);
            std::atomic<bool> go{false};
            std::vector<std::thread> pool;
            std::vector<uint64_t> retries(threads);
            for (unsigned i = 0; i < threads; ++i) {
                pool.emplace_back([&, i] {
                    alignas(64) PPCContext ctx{};
                    ctx.r3.u64 = counter;
                    ctx.r4.u64 = iters;
                    while (!go.load()) {}
                    t.fn(ctx, mem.base());
                    retries[i] = ctx.r5.u64;
                });
            }
            go = true;
            for (auto& th : pool) th.join();
            uint32_t v;
            memcpy(&v, mem.base() + counter, 4);
            v = __builtin_bswap32(v);
            uint64_t expect = threads * iters, total_retries = 0;
            for (auto r : retries) total_retries += r;
            bool ok = v == (uint32_t)expect;
            failures += !ok;
            printf("{\"id\":\"sync/atomic_add/T%u_N%llu\",\"status\":\"%s\",\"stage\":\"execute\","
                   "\"reason\":\"counter=%u expected=%llu stwcx_retries=%llu\"}\n",
                   threads, (unsigned long long)iters, ok ? "PASS" : "FAIL", v, (unsigned long long)expect,
                   (unsigned long long)total_retries);
        }
    }
    return failures ? 1 : 0;
}
