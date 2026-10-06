// R-comp host of rexglue's Xenos command processor (owner: gpu/xenos).
#include "xenos_host.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <x86intrin.h>

#include "rcomp/diag.h"
#include "rcomp/fast_clock.h"
#include <rcomp_xenos/diagnostics.h>

extern "C" {
void rcomp_profile_register_host_thread() __attribute__((weak));
void rcomp_profile_unregister_host_thread() __attribute__((weak));
std::atomic<uint64_t> rcomp_prof[128];
std::atomic<uint64_t> rcomp_gpu_ticks[8];
std::atomic<uint64_t> rcomp_band_loads{0};
std::atomic<uint64_t> rcomp_direct_load_count{0};  // texture loads written directly to storage views  // texture loads restricted to resolved tile bands
std::atomic<uint64_t> rcomp_gpu_marks[8];  // GPU segments per label
std::atomic<uint64_t> rcomp_fence_sites[8][2];  // blocking GPU-completion waits: return address, count  // GPU timestamp ticks (100 MHz) per label; 7 = submission span
#if RCOMP_XENOS_PROFILE_TIMINGS
extern uint64_t rcomp_radv_submit_stats[8] __attribute__((weak));  // PS5 RADV winsys diagnostic counters
#endif
std::atomic<uint64_t> rcomp_op_ns[128], rcomp_op_count[128], rcomp_packet_start_ns;
std::atomic<uint32_t> rcomp_cp_packets{0}, rcomp_cp_last_packet{0}, rcomp_cp_wait[6];  // bring-up tracing (see command_processor.cpp)
}

namespace rcomp::xenos {

namespace {

// Registers titles read and that rexglue's GraphicsSystem::ReadRegister
// answers specially (values from rexglue @c94f5eb graphics_system.cpp).
constexpr uint32_t kRegRbEdramTiming = 0x0F00, kRbEdramTimingValue = 0x08100748;
constexpr uint32_t kRegRbBcControl = 0x0F01, kRbBcControlValue = 0x0000200E;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1InterruptStatus = 0x1951;  // 1 = vblank
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

uint32_t load_be32(const uint8_t* p) {
    return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const uint32_t*>(p), __ATOMIC_ACQUIRE));
}
void store_be32(uint8_t* p, uint32_t v) { __atomic_store_n(reinterpret_cast<uint32_t*>(p), __builtin_bswap32(v), __ATOMIC_RELEASE); }

std::mutex g_mu;
std::atomic<uint32_t> g_ring_phys{0}, g_ring_log2{0};
std::atomic<uint32_t> g_wb_phys{0};  // bring-up tracing: rptr write-back word (physical)
std::unique_ptr<XenosHost> g_host;

}  // namespace

XenosHost::XenosHost(uint8_t* guest_base, const HostConfig& cfg, InterruptDispatcher dispatcher)
    : guest_base_(guest_base), cfg_(cfg), dispatcher_(dispatcher),
      memory_(std::make_unique<rex::memory::Memory>(guest_base)) {}

XenosHost::~XenosHost() { Stop(); }

void XenosHost::PublishReadRegisters() {
    uint8_t* w = guest_base_ + kXenosRegisterWindow;
    const uint32_t h = std::min(cfg_.display_height, 0x0FFFu), wd = std::min(cfg_.display_width, 0x0FFFu);
    store_be32(w + 4 * kRegRbEdramTiming, kRbEdramTimingValue);
    store_be32(w + 4 * kRegRbBcControl, kRbBcControlValue);
    store_be32(w + 4 * kRegD1ModeVCounter, h);
    store_be32(w + 4 * kRegD1InterruptStatus, 1);
    store_be32(w + 4 * kRegD1ModeViewportSize, (wd << 16) | h);
}

bool XenosHost::Start(const BackendFactory& backend) {
    if (running_) return false;
    cp_ = backend ? backend(this) : nullptr;
    if (!cp_ || !cp_->Initialize()) {
        cp_.reset();
        return false;
    }
    PublishReadRegisters();
    // The write pointer is sampled before the bridge thread exists: a title
    // store that lands before the thread first runs must count as a change.
    const uint32_t initial_wptr = load_be32(guest_base_ + kXenosRegisterWindow + 4 * kRegCpRbWptr);
    running_ = true;
    bridge_.Start([this, initial_wptr] { BridgeMain(initial_wptr); }, size_t(1) << 20, "BRIDGE");
    return true;
}

void XenosHost::Stop() {
    if (!running_.exchange(false)) return;
    if (bridge_.joinable()) bridge_.join();
    if (cp_) {
        cp_->Shutdown();
        cp_.reset();
    }
}

// Forwards CP_RB_WPTR stores the title makes in the register window to the
// command processor (what rexglue's MMIO write handler did) and ticks vblank.
void XenosHost::BridgeMain(uint32_t last) {
    if (rcomp_profile_register_host_thread) rcomp_profile_register_host_thread();
    // Retire the signal sampler before pthread destroys this thread's TLS.
    struct ProfileRetirement {
        ~ProfileRetirement() {
            if (rcomp_profile_unregister_host_thread) rcomp_profile_unregister_host_thread();
        }
    } profile_retirement;
    uint8_t* wptr_reg = guest_base_ + kXenosRegisterWindow + 4 * kRegCpRbWptr;
    using clock = std::chrono::steady_clock;
    const auto period = cfg_.vblank_hz ? std::chrono::nanoseconds(1000000000ull / cfg_.vblank_hz)
                                       : std::chrono::nanoseconds(0);
    // The loop reads the clock 10,000 times a second: from the TSC, without a system call.
    const auto fast_now = [] { return clock::time_point(std::chrono::nanoseconds(rcomp::fast_monotonic_ns())); };
    auto next_vblank = fast_now() + period;
    // Bring-up tracing (stderr): every distinct write-pointer store, first 128.
#if RCOMP_XENOS_DIAGNOSTICS
    uint32_t traced = 0;
    long sched_max_us = 0;
#endif
#if RCOMP_XENOS_DIAGNOSTICS || RCOMP_XENOS_PROFILE_TIMINGS
    auto next_alive = clock::now() + std::chrono::seconds(2);
#endif
    while (running_) {
        const uint32_t v = load_be32(wptr_reg);
        const auto now = cfg_.vblank_hz || RCOMP_XENOS_DIAGNOSTICS || RCOMP_XENOS_PROFILE_TIMINGS
                             ? fast_now() : clock::time_point{};
#if RCOMP_XENOS_DIAGNOSTICS
        if (now >= next_alive) {
            next_alive += std::chrono::seconds(2);
            std::fprintf(stderr, "RCOMP-SCHED online_cpus=%ld bridge_sleep_overshoot_max_us=%ld poll_us=%u\n", sysconf(_SC_NPROCESSORS_ONLN), sched_max_us, (unsigned)cfg_.bridge_poll_us);
            sched_max_us = 0;
            std::fprintf(stderr, "RCOMP-XENOS cp packets=%u last=0x%08X\n", rcomp_cp_packets.load(), rcomp_cp_last_packet.load());
            std::fprintf(stderr, "RCOMP-XENOS cp in_wait=%u info=0x%X addr=0x%X ref=0x%X mask=0x%X value=0x%X\n", rcomp_cp_wait[5].load(), rcomp_cp_wait[0].load(), rcomp_cp_wait[1].load(), rcomp_cp_wait[2].load(), rcomp_cp_wait[3].load(), rcomp_cp_wait[4].load());
            std::fprintf(stderr, "RCOMP-PROF draws=%llu draw_ms=%llu pipelines=%llu pipeline_ms=%llu\n", (unsigned long long)rcomp_prof[0].load(), (unsigned long long)(rcomp_prof[1].load() / 1000000), (unsigned long long)rcomp_prof[2].load(), (unsigned long long)(rcomp_prof[3].load() / 1000000));
            {   // where the command processor spends its time: per-opcode totals and the packet in flight now
                uint32_t top[3] = {0, 0, 0};
                for (uint32_t op = 0; op < 128; ++op) {
                    for (int k = 0; k < 3; ++k) {
                        if (rcomp_op_ns[op].load() > rcomp_op_ns[top[k]].load()) { for (int m = 2; m > k; --m) top[m] = top[m - 1]; top[k] = op; break; }
                    }
                }
                const uint64_t started = rcomp_packet_start_ns.load();
                const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
                std::fprintf(stderr, "RCOMP-OPS in_flight_ms=%llu top: 0x%02X %llums/%llu  0x%02X %llums/%llu  0x%02X %llums/%llu\n",
                             (unsigned long long)(started ? (now_ns - started) / 1000000 : 0),
                             top[0], (unsigned long long)(rcomp_op_ns[top[0]].load() / 1000000), (unsigned long long)rcomp_op_count[top[0]].load(),
                             top[1], (unsigned long long)(rcomp_op_ns[top[1]].load() / 1000000), (unsigned long long)rcomp_op_count[top[1]].load(),
                             top[2], (unsigned long long)(rcomp_op_ns[top[2]].load() / 1000000), (unsigned long long)rcomp_op_count[top[2]].load());
            }
            std::fprintf(stderr, "RCOMP-PROF fence_waits=%llu fence_wait_ms=%llu queue_submits=%llu submit_ms=%llu\n", (unsigned long long)rcomp_prof[6].load(), (unsigned long long)(rcomp_prof[7].load() / 1000000), (unsigned long long)rcomp_prof[8].load(), (unsigned long long)(rcomp_prof[9].load() / 1000000));
            std::fprintf(stderr, "RCOMP-PROF cp_interrupts=%llu interrupt_cb_ms=%llu\n", (unsigned long long)rcomp_prof[4].load(), (unsigned long long)(rcomp_prof[5].load() / 1000000));
            {   // the title's CPU-side view of GPU fences: D3D device (interrupt user data) fields the wait loop reads
                const uint32_t dev = interrupt_user_data_.load();
                if (dev && dev < 0x7FFFFFF0u) {
                    const uint32_t p1 = load_be32(guest_base_ + dev + 10896), d8 = load_be32(guest_base_ + dev + 10888), d0 = load_be32(guest_base_ + dev + 10996);
                    uint32_t v0 = 0, v1 = 0;
                    if (p1 && p1 < 0x7FFFFFF0u) { v0 = load_be32(guest_base_ + p1); v1 = load_be32(guest_base_ + p1 + 4); }
                    std::fprintf(stderr, "RCOMP-XENOS device=0x%08X fence_ptr=0x%08X *fence=0x%08X,0x%08X [+10888]=0x%08X [+10996]=0x%08X\n", dev, p1, v0, v1, d8, d0);
                }
            }
            const uint32_t wb = g_wb_phys.load();
            const uint32_t rptr = wb ? load_be32(guest_base_ + kXenosPhysicalWindow + wb) : 0xFFFFFFFFu;
            std::fprintf(stderr, "RCOMP-XENOS bridge alive wptr=0x%X rptr_writeback=0x%X vblanks=%u\n", last, rptr, vblanks_.load());
            // Stall analysis (once): find every dword naming the polled word in the queued IBs.
            static uint32_t last_packets = 0xFFFFFFFFu;
            static bool scanned = false;
            const uint32_t now_packets = rcomp_cp_packets.load();
            const uint32_t polled = rcomp_cp_wait[1].load() & 0x1FFFFFFCu;
            if (!scanned && now_packets == last_packets && g_ring_phys.load() && polled) {
                scanned = true;
                const uint32_t ring_b = g_ring_phys.load();
                const uint32_t mask_b = (1u << (g_ring_log2.load() + 1)) - 1;
                for (uint32_t at = rptr; at != last; at = (at + 1) & mask_b) {
                    const uint32_t h = load_be32(guest_base_ + kXenosPhysicalWindow + ring_b + 4 * at);
                    if ((h >> 30) != 3 || ((h >> 8) & 0x7F) != 0x3F) continue;
                    const uint32_t ib = load_be32(guest_base_ + kXenosPhysicalWindow + ring_b + 4 * ((at + 1) & mask_b)) & 0x1FFFFFFFu;
                    const uint32_t sz = load_be32(guest_base_ + kXenosPhysicalWindow + ring_b + 4 * ((at + 2) & mask_b)) & 0xFFFFFu;
                    std::fprintf(stderr, "RCOMP-XENOS scan IB at ring %u: phys=0x%X dwords=%u\n", at, ib, sz);
                    for (uint32_t i = 0; i < sz && i < 16384; ++i) {
                        const uint32_t w = load_be32(guest_base_ + kXenosPhysicalWindow + ib + 4 * i);
                        if ((w & 0x1FFFFFFCu) != polled) continue;
                        std::fprintf(stderr, "RCOMP-XENOS   match dword %u:", i);
                        for (uint32_t k = (i > 6 ? i - 6 : 0); k < sz && k < i + 7; ++k)
                            std::fprintf(stderr, " %08X", load_be32(guest_base_ + kXenosPhysicalWindow + ib + 4 * k));
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            last_packets = now_packets;
            if (const uint32_t ring = g_ring_phys.load(); ring && rptr != 0xFFFFFFFFu) {
                const uint32_t mask = (1u << (g_ring_log2.load() + 1)) - 1;  // dwords in the ring - 1
                std::fprintf(stderr, "RCOMP-XENOS ring[rptr..]:");
                for (uint32_t i = 0; i < 12; ++i)
                    std::fprintf(stderr, " %08X", load_be32(guest_base_ + kXenosPhysicalWindow + ring + 4 * ((rptr + i) & mask)));
                std::fprintf(stderr, "\n");
                // If the stalled packet is an INDIRECT_BUFFER (type 3, opcode 0x3F), dump its target.
                const uint32_t header = load_be32(guest_base_ + kXenosPhysicalWindow + ring + 4 * (rptr & mask));
                if ((header >> 30) == 3 && ((header >> 8) & 0x7F) == 0x3F) {
                    const uint32_t ib = load_be32(guest_base_ + kXenosPhysicalWindow + ring + 4 * ((rptr + 1) & mask)) & 0x1FFFFFFFu;
                    const uint32_t size = load_be32(guest_base_ + kXenosPhysicalWindow + ring + 4 * ((rptr + 2) & mask)) & 0xFFFFFu;
                    std::fprintf(stderr, "RCOMP-XENOS IB phys=0x%X dwords=%u:", ib, size);
                    for (uint32_t i = 0; i < size && i < 32; ++i)
                        std::fprintf(stderr, " %08X", load_be32(guest_base_ + kXenosPhysicalWindow + ib + 4 * i));
                    std::fprintf(stderr, "\n");
                }
            }
        }
#elif RCOMP_XENOS_PROFILE_TIMINGS
        if (now >= next_alive) {
            next_alive += std::chrono::seconds(2);
            // Cumulative (count, Mcycles) per slot and wall Mcycles since start;
            // the analysis differences consecutive lines.
            static const uint64_t tsc0 = __rdtsc();
            char line[3072];
            int n = std::snprintf(line, sizeof(line), "RCOMP-PROFT wall=%llu",
                                  (unsigned long long)((__rdtsc() - tsc0) / 1000000));
            for (int slot = 0; slot < 128 && n > 0 && n < int(sizeof(line)); slot += 2)
                n += std::snprintf(line + n, sizeof(line) - n, " %d:%llu/%llu", slot,
                                   (unsigned long long)rcomp_prof[slot].load(),
                                   (unsigned long long)(rcomp_prof[slot + 1].load() / 1000000));
            std::fprintf(stderr, "%s\n", line);
            char ops[768];
            n = std::snprintf(ops, sizeof(ops), "RCOMP-OPST");
            for (uint32_t op = 0; op < 128 && n > 0 && n < int(sizeof(ops)); ++op)
                if (rcomp_op_count[op].load())
                    n += std::snprintf(ops + n, sizeof(ops) - n, " %02X:%llu/%llu", op,
                                       (unsigned long long)rcomp_op_count[op].load(),
                                       (unsigned long long)(rcomp_op_ns[op].load() / 1000000));
            std::fprintf(stderr, "%s\n", ops);
            std::fprintf(stderr, "RCOMP-GPUT draw=%llu transfer=%llu resolve=%llu swap=%llu texload=%llu gap=%llu span=%llu\n",
                         (unsigned long long)rcomp_gpu_ticks[0].load(), (unsigned long long)rcomp_gpu_ticks[1].load(),
                         (unsigned long long)rcomp_gpu_ticks[2].load(), (unsigned long long)rcomp_gpu_ticks[3].load(),
                         (unsigned long long)rcomp_gpu_ticks[4].load(), (unsigned long long)rcomp_gpu_ticks[6].load(),
                         (unsigned long long)rcomp_gpu_ticks[7].load());
            std::fprintf(stderr, "RCOMP-GPUN draw=%llu transfer=%llu resolve=%llu swap=%llu texload=%llu cp_cpu=%llu%c",
                         (unsigned long long)rcomp_gpu_marks[0].load(), (unsigned long long)rcomp_gpu_marks[1].load(),
                         (unsigned long long)rcomp_gpu_marks[2].load(), (unsigned long long)rcomp_gpu_marks[3].load(),
                         (unsigned long long)rcomp_gpu_marks[4].load(), (unsigned long long)rcomp_gpu_marks[7].load(), 10);
            std::fprintf(stderr, "RCOMP-BANDLOADS %llu direct=%llu%c", (unsigned long long)rcomp_band_loads.load(),
                         (unsigned long long)rcomp_direct_load_count.load(), 10);
            for (auto& site : rcomp_fence_sites)
                if (site[0].load())
                    std::fprintf(stderr, "RCOMP-FENCE-SITE 0x%llx %llu%c", (unsigned long long)site[0].load(),
                                 (unsigned long long)site[1].load(), 10);
            if (rcomp_radv_submit_stats)
                std::fprintf(stderr, "RCOMP-RADVT calls=%llu words=%llu flush=%llu dcb=%llu suspend=%llu claim=%llu copy=%llu\n",
                             (unsigned long long)rcomp_radv_submit_stats[0], (unsigned long long)rcomp_radv_submit_stats[1],
                             (unsigned long long)(rcomp_radv_submit_stats[2] / 1000000),
                             (unsigned long long)(rcomp_radv_submit_stats[3] / 1000000),
                             (unsigned long long)(rcomp_radv_submit_stats[4] / 1000000),
                             (unsigned long long)(rcomp_radv_submit_stats[5] / 1000000),
                             (unsigned long long)(rcomp_radv_submit_stats[6] / 1000000));
        }
#endif
        if (v != last) {
#if RCOMP_XENOS_DIAGNOSTICS
            if (traced < 128) { ++traced; std::fprintf(stderr, "RCOMP-XENOS wptr 0x%X -> 0x%X\n", last, v); }
#endif
            last = v;
            register_file_.values[kRegCpRbWptr] = v;
            cp_->UpdateWritePointer(v);
        }
        if (cfg_.vblank_hz && now >= next_vblank) {
            next_vblank += period;
            cp_->increment_counter();
            vblanks_.fetch_add(1);
            DispatchInterruptCallback(0, 2);
        }
        struct timespec ts = {0, long(cfg_.bridge_poll_us) * 1000};
#if RCOMP_XENOS_DIAGNOSTICS
        const auto sleep_start = clock::now();
#endif
        nanosleep(&ts, nullptr);
#if RCOMP_XENOS_DIAGNOSTICS
        {   // scheduling latency probe: how much longer than requested a short sleep takes
            const auto over = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - sleep_start).count() - long(cfg_.bridge_poll_us);
            if (over > sched_max_us) sched_max_us = over;
        }
#endif
    }
}

void XenosHost::OnHostGpuLossFromAnyThread(bool is_responsible) {
    rcomp_fatal(RCOMP_FATAL_PLATFORM, "xenos: host GPU lost%s", is_responsible ? " (reported by the backend)" : "");
}

void XenosHost::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
#if RCOMP_XENOS_DIAGNOSTICS
    std::fprintf(stderr, "RCOMP-XENOS set interrupt callback=0x%08X user=0x%08X\n", callback, user_data);
#endif
    interrupt_user_data_ = user_data;
    interrupt_callback_ = callback;
}

void XenosHost::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
    const uint32_t cb = interrupt_callback_.load();
#if RCOMP_XENOS_DIAGNOSTICS
    if (false && source != 0) {
        std::fprintf(stderr, "RCOMP-XENOS interrupt source=%u cpu=%u callback=0x%08X user=0x%08X%s\n", source, cpu, cb,
                     interrupt_user_data_.load(), (cb && dispatcher_) ? "" : " (NOT DISPATCHED)");
    }
#endif
    if (!cb || !dispatcher_) return;
#if RCOMP_XENOS_PROFILE_TIMINGS
    const uint64_t t0 = __rdtsc();
#endif
    dispatcher_(cb, interrupt_user_data_.load(), source, cpu == 0xFFFFFFFFu ? 2 : cpu);
#if RCOMP_XENOS_PROFILE_TIMINGS
    if (source != 0) {  // bring-up profiling: time spent inside guest interrupt callbacks (source 1 = command stream)
        rcomp_prof[4].fetch_add(1, std::memory_order_relaxed);
        rcomp_prof[5].fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    }
#endif
}

bool gpu_start(rcomp::GuestMemory& mem, const HostConfig& cfg, InterruptDispatcher dispatcher,
               const BackendFactory& backend) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_host) return false;
    if (!mem.overlaps_runtime_range(kXenosRegisterWindow, kXenosRegisterWindowSize) &&
        mem.reserve_runtime_range(kXenosRegisterWindow, kXenosRegisterWindowSize, "xenos-registers") !=
            rcomp::MemStatus::Ok)
        return false;
    auto host = std::make_unique<XenosHost>(mem.base(), cfg, dispatcher);
    if (!host->Start(backend)) return false;
    g_host = std::move(host);
    return true;
}

void gpu_stop() {
    std::unique_ptr<XenosHost> h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        h = std::move(g_host);
    }
    if (h) h->Stop();
}

XenosHost* gpu_host() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_host.get();
}

// ---- include/rcomp/xenos_gpu.h -------------------------------------------------

namespace {
rex::graphics::CommandProcessor* cp_or_die(const char* what) {
    XenosHost* h = gpu_host();
    if (!h || !h->command_processor())
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: %s before the GPU was started", what);
    return h->command_processor();
}
}  // namespace

bool gpu_running() { return gpu_host() != nullptr; }

DisplayMode gpu_display_mode() {
    XenosHost* h = gpu_host();
    if (!h) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: display mode queried before the GPU was started");
    const HostConfig& c = h->config();
    return {c.display_width, c.display_height, c.refresh_hz};
}

void gpu_initialize_ring_buffer(uint32_t ptr, uint32_t size_log2) {
#if RCOMP_XENOS_DIAGNOSTICS
    std::fprintf(stderr, "RCOMP-XENOS ring base=0x%X size_log2=%u\n", ptr, size_log2);
#endif
    g_ring_phys = ptr;
    g_ring_log2 = size_log2;
    cp_or_die("InitializeRingBuffer")->InitializeRingBuffer(ptr, size_log2);
}

void gpu_enable_read_pointer_writeback(uint32_t ptr, uint32_t block_size_log2) {
#if RCOMP_XENOS_DIAGNOSTICS
    std::fprintf(stderr, "RCOMP-XENOS rptr writeback phys=0x%X block_log2=%u\n", ptr, block_size_log2);
#endif
    g_wb_phys = ptr;
    cp_or_die("EnableReadPointerWriteBack")->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data) {
    XenosHost* h = gpu_host();
    if (!h) rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: SetInterruptCallback before the GPU was started");
    h->SetInterruptCallback(callback, user_data);
}

}  // namespace rcomp::xenos
