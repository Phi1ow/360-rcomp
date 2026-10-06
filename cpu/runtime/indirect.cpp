// Checked indirect dispatch for XenonRecomp output (see rcomp/ppc_prelude.h).
#define PPC_CONFIG_H_INCLUDED
#include "rcomp/ppc_prelude.h"
#include <ppc_context.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <string>
#include "profiler_registry.h"
#include "profiler_pc.h"

#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime_state.h"

#ifndef RCOMP_CPU_DIAGNOSTICS
#define RCOMP_CPU_DIAGNOSTICS 0
#endif

void rcomp_call_indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    PPCFunc* fn = rcomp::lookup_function(target);
    if (!fn) {
        rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET,
                    "target=0x%08X lr=0x%08llX ctr=0x%08X r3=0x%016llX reason=%s", target,
                    (unsigned long long)ctx.lr, ctx.ctr.u32, (unsigned long long)ctx.r3.u64,
                    (target & 3) ? "misaligned" : "no_function_at_address");
    }
    rcomp::indirect_cache_fill(target, reinterpret_cast<const void*>(fn));
    fn(ctx, base);
}

void rcomp_switch_out_of_range(PPCContext& ctx, uint32_t guest_pc, uint32_t index) {
    rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "jump table at 0x%08X index=%u outside the analysed table lr=0x%08llX",
                guest_pc, index, (unsigned long long)ctx.lr);
}

void rcomp_guest_trap(PPCContext& ctx, uint32_t guest_pc) {
    // `twi 31,r0,20` is the Xbox 360 debug-print service: r3 = guest pointer to
    // the text, r4 = its length in bytes; execution continues after the trap.
    // It is the one trap with a defined non-fatal meaning (public rexglue-sdk
    // c94f5eb / Xenia treat the immediate as the service code).
    rcomp::GuestMemory* mem = rcomp::active_guest_memory();
    uint32_t word = 0;
    if (mem && mem->is_accessible(guest_pc, 4, rcomp::Protect::Read)) {
        const uint8_t* p = mem->translate(guest_pc, 4);
        if (p) word = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    if ((word & 0xFFFF0000u) == 0x0FE00000u && (word & 0xFFFFu) == 20u) {
        const uint32_t text = ctx.r3.u32, length = ctx.r4.u32;
        if (mem && length <= 4096 && (length == 0 || mem->is_accessible(text, length, rcomp::Protect::Read))) {
            const uint8_t* bytes = length ? mem->translate(text, length) : nullptr;
            if (length == 0 || bytes) {
                char line[4097];
                uint32_t n = 0;
                for (uint32_t i = 0; i < length; ++i) {
                    const uint8_t ch = bytes[i];
                    if (ch == '\n' || ch == '\r') { if (n && line[n - 1] != ' ') line[n++] = ' '; continue; }
                    line[n++] = (ch >= 0x20 && ch < 0x7F) ? char(ch) : '.';
                }
                line[n] = 0;
                if (n == 0) return;  // newline-only prints carry no text
                std::fprintf(stdout, "RCOMP-GUEST-DEBUG pc=0x%08X %s\n", guest_pc, line);
                return;  // stdout is flushed by the title's log flusher, not per line
            }
        }
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "debug-print trap pc=0x%08X text=0x%08X length=%u not readable",
                    guest_pc, text, length);
    }
    // On the console any other taken trap raises a program exception; titles use it
    // for asserts. There is no handler to resume into, so stop with the site.
    rcomp_fatal(RCOMP_FATAL_GUEST_TRAP, "pc=0x%08X lr=0x%08llX r3=0x%016llX", guest_pc,
                (unsigned long long)ctx.lr, (unsigned long long)ctx.r3.u64);
}

void rcomp_unresolved_import(PPCContext& ctx, const char* module, uint32_t ordinal) {
    // Same line shape as rcomp::hle_missing_import (runtime/): module, ordinal,
    // caller and the first argument registers.
    rcomp_fatal(RCOMP_FATAL_MISSING_IMPORT,
                "module=%s ordinal=0x%04X name=(not in any export table) lr=0x%08llX r3=0x%08X r4=0x%08X "
                "r5=0x%08X r6=0x%08X",
                module, ordinal, (unsigned long long)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

void rcomp_call_unknown(PPCContext& ctx, uint32_t target) {
    rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET,
                "direct call target=0x%08X lr=0x%08llX reason=no_function_recompiled_at_address", target,
                (unsigned long long)ctx.lr);
}

extern "C" void rcomp_trace_fpscr(uint32_t old_csr, uint32_t value, uint32_t fields) {
#if RCOMP_CPU_DIAGNOSTICS
    // Bring-up tracing: report FPSCR stores that would enable exceptions (bits 3..7).
    static std::atomic<int> printed{0};
    const uint32_t mask = ((fields & 1u) ? 0x0000000Fu : 0u) | ((fields & 2u) ? 0x000000F0u : 0u);
    if (((value & mask) & 0xF8u) && printed.fetch_add(1) < 60)
        std::fprintf(stderr, "RCOMP-FPSCR store old=0x%08X value=0x%08X fields=0x%02X\n", old_csr, value, fields);
#else
    (void)old_csr;
    (void)value;
    (void)fields;
#endif
}

// ---- sampling profiler (diagnostic titles): which recompiled function each guest thread is in ----
extern "C" {
thread_local const char* rcomp_t_fn = nullptr;
thread_local const char* rcomp_t_ring[256];
thread_local unsigned rcomp_t_idx = 0;
// Keep the registry alive through process teardown; other pthreads and the
// sampler may still be running when C++ static destructors begin.
alignas(RcompProfilerRegistry) static unsigned char g_profile_storage[sizeof(RcompProfilerRegistry)];
static RcompProfilerRegistry& g_profile_threads =
    *::new (static_cast<void*>(g_profile_storage)) RcompProfilerRegistry;
extern "C" uint32_t rcomp_debug_read_guest_u32(uint32_t address);
static thread_local std::atomic<void*> t_ctx{nullptr};
static thread_local uint32_t t_ctx_id = 0;
static thread_local volatile sig_atomic_t t_pc_group = -1;
static_assert(std::atomic<void*>::is_always_lock_free);
// The guest thread whose samples are reported as the "main" group: thread 1 (the title's main thread) unless
// RCOMP_PC_SAMPLE_TID names another (4 October 2026: the render-side worker is the pacer of Episodes from
// Liberty City, 97 % of a core, and "others" averages it with twenty sleeping workers).
static uint32_t sampled_group0_thread() {
    static const uint32_t tid = [] {
        const char* v = std::getenv("RCOMP_PC_SAMPLE_TID");
        const long n = v && *v ? std::strtol(v, nullptr, 10) : 1;
        return uint32_t(n > 0 ? n : 1);
    }();
    return tid;
}
// RCOMP_PC_SAMPLE_TID2 (optional): the "others" group holds this one guest thread only, instead of every other
// guest thread (0 = the default, all of them): two threads profiled apart in one run.
static uint32_t sampled_group1_thread() {
    static const uint32_t tid = [] {
        const char* v = std::getenv("RCOMP_PC_SAMPLE_TID2");
        const long n = v && *v ? std::strtol(v, nullptr, 10) : 0;
        return uint32_t(n > 0 ? n : 0);
    }();
    return tid;
}
void rcomp_register_thread_ctx(uint32_t thread_id, void* ctx) {
    // Host GPU threads retain their group while executing guest interrupts.
    if (t_pc_group != 2) {
        const uint32_t second = sampled_group1_thread();
        t_pc_group = thread_id == sampled_group0_thread() ? 0 : (!second || thread_id == second) ? 1 : -1;
    }
    t_ctx_id = thread_id;
    t_ctx.store(ctx, std::memory_order_relaxed);
    if (!g_profile_threads.register_guest(thread_id, ctx))
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "PC sampler guest registration failed tid=%u", thread_id);
}
void rcomp_unregister_thread_ctx(uint32_t thread_id, void* ctx) {
    // Remove published metadata even if another pthread-key destructor has
    // already released emulated TLS. Ownership is checked by the registry.
    g_profile_threads.unregister_guest(thread_id, ctx);
    if (t_ctx_id != thread_id || t_ctx.load(std::memory_order_relaxed) != ctx) return;
    t_ctx.store(nullptr, std::memory_order_relaxed);
    t_ctx_id = 0;
    if (t_pc_group != 2) t_pc_group = -1;
}
// Crash-report helper (diagnostic titles): guest base and the faulting thread registers.
void rcomp_debug_crash_context(void) {
    rcomp::GuestMemory* mem = rcomp::active_guest_memory();
    std::fprintf(stderr, "RCOMP-CRASH guest_base=%p\n", mem ? (void*)mem->base() : nullptr);
    void* const context = t_ctx.load(std::memory_order_relaxed);
    if (!context) return;
    const PPCContext& c = *static_cast<PPCContext*>(context);
    std::fprintf(stderr, "RCOMP-CRASH lr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r9=%08X r10=%08X r11=%08X r31=%08X\n", (uint32_t)c.lr, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r9.u32, c.r10.u32, c.r11.u32, c.r31.u32);
    uint32_t sp = c.r1.u32; std::string chain;
    for (int frame = 0; frame < 16 && sp; ++frame) { const uint32_t back = rcomp_debug_read_guest_u32(sp); if (!back || back == 0xDEADBEEFu) break; char b[16]; std::snprintf(b, sizeof b, " %08X", rcomp_debug_read_guest_u32(back - 8)); chain += b; sp = back; }
    std::fprintf(stderr, "RCOMP-CRASH chain:%s\n", chain.c_str());
    // The first 128 bytes of the faulting frame's local area as printable text (a path or a name the function was working on); a non-printable byte is '.', the text ends at the first NUL after a printable one.
    {
        char text[129];
        size_t length = 0;
        for (uint32_t offset = 96; offset < 96 + 128 && c.r1.u32; offset += 4) {
            const uint32_t word = rcomp_debug_read_guest_u32(c.r1.u32 + offset);
            for (int shift = 24; shift >= 0; shift -= 8) {
                const unsigned byte = (word >> shift) & 0xFFu;
                text[length++] = (byte >= 0x20 && byte < 0x7F) ? char(byte) : '.';
            }
        }
        text[length] = 0;
        std::fprintf(stderr, "RCOMP-CRASH frame+96: %s\n", text);
    }
}
void rcomp_register_thread_slot(uint32_t thread_id) {
    (void)thread_id;
    if (!g_profile_threads.register_slot(&rcomp_t_fn, rcomp_t_ring, &rcomp_t_idx))
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "PC sampler TLS registration failed");
}
}

#include <pthread.h>
#include <time.h>
#include <map>
#include <string>

// ---- host PC sampling (diagnostic titles): SIGURG interrupts a guest thread, its handler
// records the interrupted host instruction pointer. Symbolised offline with the link map.
// Weak, relocated linker symbols. The platform owner supplies these from
// ADDR(.text)/SIZEOF(.text); an absent contract disables candidate scanning.
extern "C" {
extern const unsigned char __rcomp_text_begin[]
    __attribute__((weak, visibility("hidden")));
extern const unsigned char __rcomp_text_end[]
    __attribute__((weak, visibility("hidden")));
}
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static std::atomic<uint64_t> g_pc_rx_begin{0}, g_pc_rx_end{0};
static RcompPcSamples<> g_pc_samples[3];
// Wall time by what the thread is in, without any timing of its own: a sample whose instruction pointer is in a host system library (mapped at 0x8_xxxx_xxxx on the
// console) counts the number of the system call being (re)started (rax), and every sample counts the HLE import the thread is executing
// (rcomp_sampler_current_import, runtime/src/import_registry.cpp: module << 12 | ordinal, 0 = none). Both are drained with the PCs.
extern "C" uint32_t rcomp_sampler_current_import(void) __attribute__((weak));
constexpr unsigned kSysSlots = 512, kImportSlots = 8192;
static std::atomic<uint32_t> g_sys_samples[3][kSysSlots];
static std::atomic<uint32_t> g_import_samples[3][kImportSlots];
static void pc_sample_handler(int, siginfo_t*, void* context) {
    const int group = t_pc_group;  // existing TLS, warmed before registration
    if (group < 0 || group >= 3) return;
    const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    const RcompPcRxBounds rx{
        g_pc_rx_begin.load(std::memory_order_relaxed),
        g_pc_rx_end.load(std::memory_order_relaxed)};
    g_pc_samples[group].record(uint64_t(mc.mc_rip), uint64_t(mc.mc_rsp), rx);
    if ((uint64_t(mc.mc_rip) >> 32) == 8) {
        const uint64_t number = uint64_t(mc.mc_rax);
        g_sys_samples[group][number < kSysSlots - 1 ? number : kSysSlots - 1].fetch_add(1, std::memory_order_relaxed);
    }
    if (rcomp_sampler_current_import) {
        const uint32_t id = rcomp_sampler_current_import();
        g_import_samples[group][id < kImportSlots ? id : 0].fetch_add(1, std::memory_order_relaxed);
    }
}
extern "C" void rcomp_profile_register_host_thread() {
    t_pc_group = 2;
    if (!g_profile_threads.register_host()) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true))
            std::fprintf(stderr, "RCOMP-PC LIMIT host registration unavailable (maximum four targets)\n");
    }
}
extern "C" void rcomp_profile_unregister_host_thread() {
    // Retire before leaving the host entry point, while emulated TLS is live.
    // A pending SIGURG is delivered after the group becomes -1 and this
    // thread is no longer a sampling target. The handler never takes a lock.
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGURG);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "PC sampler signal blocking failed");
    t_ctx.store(nullptr, std::memory_order_relaxed);
    t_ctx_id = 0;
    t_pc_group = -1;
    if (!g_profile_threads.unregister_current())
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "PC sampler thread unregister failed");
    if (pthread_sigmask(SIG_SETMASK, &previous, nullptr) != 0)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "PC sampler signal restore failed");
}

static std::string pc_sample_rows(
        std::vector<std::pair<uint32_t, uint64_t>>& rows) {
    std::sort(rows.rbegin(), rows.rend());
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 600; ++i) {
        char b[40];
        std::snprintf(b, sizeof b, " %llx:%u",
                      (unsigned long long)rows[i].second, rows[i].first);
        line += b;
    }
    return line;
}
static void pc_sample_dump() {
    for (int group = 0; group < 3; ++group) {
        const char* const name = group == 0 ? "main" : group == 1 ? "others" : "gpu";
        std::vector<std::pair<uint32_t, uint64_t>> raw_rows, candidate_rows;
        const auto raw = g_pc_samples[group].raw.drain(
            [&](uint64_t pc, uint32_t n) { raw_rows.emplace_back(n, pc); });
        const auto candidates = g_pc_samples[group].candidates.drain(
            [&](uint64_t pc, uint32_t n) { candidate_rows.emplace_back(n, pc); });
        const std::string raw_line = pc_sample_rows(raw_rows);
        const std::string candidate_line = pc_sample_rows(candidate_rows);
        // Existing RCOMP-PC parsers see only actual interrupted RIPs. The
        // heuristic family has its own counts; never add them to raw total.
        std::fprintf(stderr, "RCOMP-PC %s total=%llu recorded=%llu dropped=%llu%s\n",
                     name, (unsigned long long)raw.total,
                     (unsigned long long)raw.recorded, (unsigned long long)raw.dropped,
                     raw_line.c_str());
        std::fprintf(stderr, "RCOMP-PC-CANDIDATE %s kind=stack-scan-heuristic total=%llu raw_total=%llu recorded=%llu dropped=%llu%s\n",
                     name, (unsigned long long)candidates.total,
                     (unsigned long long)raw.total, (unsigned long long)candidates.recorded,
                     (unsigned long long)candidates.dropped, candidate_line.c_str());
        // Syscall numbers (decimal) and imports (id = module << 12 | ordinal: module 0 xboxkrnl, 1 xam) of the interval, most frequent first.
        std::vector<std::pair<uint32_t, uint32_t>> system_rows, import_rows;
        uint64_t system_total = 0, import_total = 0, import_none = 0;
        for (uint32_t i = 0; i < kSysSlots; ++i) {
            const uint32_t n = g_sys_samples[group][i].exchange(0, std::memory_order_relaxed);
            if (n) { system_rows.emplace_back(n, i); system_total += n; }
        }
        for (uint32_t i = 0; i < kImportSlots; ++i) {
            const uint32_t n = g_import_samples[group][i].exchange(0, std::memory_order_relaxed);
            if (!n) continue;
            import_total += n;
            if (i == 0) import_none = n; else import_rows.emplace_back(n, i);
        }
        std::sort(system_rows.rbegin(), system_rows.rend());
        std::sort(import_rows.rbegin(), import_rows.rend());
        std::string system_line, import_line;
        for (size_t i = 0; i < system_rows.size() && i < 16; ++i) {
            char b[32];
            std::snprintf(b, sizeof b, " %u:%u", system_rows[i].second, system_rows[i].first);
            system_line += b;
        }
        for (size_t i = 0; i < import_rows.size() && i < 24; ++i) {
            char b[32];
            std::snprintf(b, sizeof b, " %u:%u", import_rows[i].second, import_rows[i].first);
            import_line += b;
        }
        std::fprintf(stderr, "RCOMP-PC-SYS %s total=%llu%s\n", name, (unsigned long long)system_total, system_line.c_str());
        std::fprintf(stderr, "RCOMP-PC-IMPORT %s total=%llu none=%llu%s\n", name, (unsigned long long)import_total,
                     (unsigned long long)import_none, import_line.c_str());
    }
}

static void* sampler_main(void*) {
    // Resolve and publish bounds once, before installing the handler/sending
    // SIGURG. Do not dereference the symbols or call the loader in a signal.
    RcompPcRxBounds rx{reinterpret_cast<uint64_t>(__rcomp_text_begin),
                       reinterpret_cast<uint64_t>(__rcomp_text_end)};
    if (!rx.valid()) rx = {};
    g_pc_rx_begin.store(rx.begin, std::memory_order_relaxed);
    g_pc_rx_end.store(rx.end, std::memory_order_relaxed);
    std::fprintf(stderr, "RCOMP-PC-FORMAT version=2 raw=interrupted-rip candidate=stack-scan-heuristic candidate_counts=subset-never-add rx_begin=%llx rx_end=%llx rx_valid=%u\n",
                 (unsigned long long)rx.begin, (unsigned long long)rx.end,
                 unsigned(rx.valid()));
    {
        struct sigaction action {};
        action.sa_sigaction = pc_sample_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGURG, &action, nullptr);
    }
#if RCOMP_CPU_DIAGNOSTICS
    std::map<std::pair<uint32_t, std::string>, uint32_t> histogram;
#endif
    uint32_t samples = 0;
    for (;;) {
        for (int burst = 0; burst < 50; ++burst) {  // 1 kHz PC sampling of every guest thread
            struct timespec tick = {0, 1000 * 1000};
            nanosleep(&tick, nullptr);
            static uint32_t next_other = 2;
            g_profile_threads.sample_tick(next_other);
        }
#if RCOMP_CPU_DIAGNOSTICS
        g_profile_threads.with_guest_contexts([&](const auto& guest_rows) {
        for (uint32_t tid = 1; tid < 256; ++tid) {
            const char* const* slot = guest_rows[tid].fn;
            if (!slot) continue;
            const char* fn = *slot;
            histogram[{tid, fn ? fn : "?"}]++;
        }
        });
#endif
        if (++samples % 200 == 0) {  // every 10 s: top functions per thread with samples
#if RCOMP_CPU_DIAGNOSTICS
            g_profile_threads.with_guest_contexts([&](const auto& guest_rows) {
            std::map<uint32_t, std::pair<std::string, uint32_t>> best;
            for (const auto& entry : histogram) {
                auto& top = best[entry.first.first];
                if (entry.second > top.second) top = {entry.first.second, entry.second};
            }
            uint32_t busiest = 0;
            {   // function entries per second per guest thread (throughput)
                static unsigned last_idx[256];
                std::string rates;
                busiest = 0; unsigned busiest_rate = 0;
                for (uint32_t tid = 1; tid < 24; ++tid) {
                    if (!guest_rows[tid].ring_idx) continue;
                    const unsigned now_idx = *guest_rows[tid].ring_idx;
                    rates += " t" + std::to_string(tid) + "=" + std::to_string((now_idx - last_idx[tid]) / 10);
                    if (tid != 1 && (now_idx - last_idx[tid]) / 10 > busiest_rate) { busiest_rate = (now_idx - last_idx[tid]) / 10; busiest = tid; }
                    last_idx[tid] = now_idx;
                }
                std::fprintf(stderr, "RCOMP-RATE calls/s:%s\n", rates.c_str());
            }
            for (uint32_t tid : {1u, busiest}) {
                if (!tid) continue;  // composition of the busy threads' most recent 256 function entries
                if (!guest_rows[tid].ring) continue;
                std::map<std::string, uint32_t> recent;
                for (unsigned i = 0; i < 256; ++i) if (guest_rows[tid].ring[i]) recent[guest_rows[tid].ring[i]]++;
                std::string line;
                for (const auto& e : recent) if (e.first.find("gprlr") == std::string::npos && e.first.find("fpr") == std::string::npos) line += " " + e.first.substr(e.first.size() > 10 ? 10 : 0) + "x" + std::to_string(e.second);
                std::fprintf(stderr, "RCOMP-RING tid=%u recent:%s\n", tid, line.c_str());
            }
            for (uint32_t tid = 1; tid < 24; ++tid) {  // guest call chain of every thread (back-chain walk)
                if (!guest_rows[tid].ctx) continue;
                uint32_t sp = static_cast<PPCContext*>(guest_rows[tid].ctx)->r1.u32;
                std::string chain;
                for (int frame = 0; frame < 16 && sp; ++frame) {
                    const uint32_t back = rcomp_debug_read_guest_u32(sp);
                    if (!back || back == 0xDEADBEEFu) break;
                    const uint32_t saved = rcomp_debug_read_guest_u32(back - 8);
                    char buf[16]; std::snprintf(buf, sizeof buf, " %08X", saved); chain += buf;
                    sp = back;
                }
                std::fprintf(stderr, "RCOMP-STACK tid=%u:%s\n", tid, chain.c_str());
            }
            for (uint32_t tid = 2; tid < 24; ++tid) {  // ordered tails of every busy thread
                if (!guest_rows[tid].ring || !guest_rows[tid].ring_idx) continue;
                static unsigned seq_last[256];
                const unsigned now_idx = *guest_rows[tid].ring_idx;
                const bool busy = now_idx - seq_last[tid] > 400000;
                seq_last[tid] = now_idx;
                if (!busy) continue;
                std::string seq;
                for (unsigned i = 0; i < 32; ++i) { const char* f = guest_rows[tid].ring[(now_idx - 32 + i) & 255]; if (f) { std::string n = f; seq += " " + n.substr(n.size() > 6 ? n.size() - 6 : 0); } }
                std::fprintf(stderr, "RCOMP-SEQ tid=%u:%s\n", tid, seq.c_str());
            }
            if (guest_rows[1].ring) {  // ordered tail of the main thread ring
                std::string seq;
                for (unsigned i = 0; i < 40; ++i) { const char* f = guest_rows[1].ring[(*guest_rows[1].ring_idx - 40 + i) & 255]; if (f) { std::string n = f; seq += " " + n.substr(n.size() > 6 ? n.size() - 6 : 0); } }
                std::fprintf(stderr, "RCOMP-SEQ tid=1:%s\n", seq.c_str());
            }
            for (const auto& entry : best)
                std::fprintf(stderr, "RCOMP-SAMPLE tid=%u top=%s samples=%u/200\n", entry.first, entry.second.first.c_str(), entry.second.second);
            histogram.clear();
            });
#endif
            pc_sample_dump();
        }
    }
    return nullptr;
}

extern "C" void rcomp_start_sampler() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, sampler_main, nullptr) == 0) pthread_detach(thread);
}

// Crash-report helper (diagnostic titles): big-endian word at a guest address, 0xDEADBEEF if unmapped.
extern "C" uint32_t rcomp_debug_read_guest_u32(uint32_t address) {
    rcomp::GuestMemory* mem = rcomp::active_guest_memory();
    if (!mem || !mem->is_accessible(address, 4, rcomp::Protect::Read)) return 0xDEADBEEFu;
    const uint8_t* p = mem->translate(address, 4);
    return p ? __builtin_bswap32(*(const uint32_t*)p) : 0xDEADBEEFu;
}
