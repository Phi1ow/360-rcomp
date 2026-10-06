// XMA audio hardware model: register window, context array, context lifetime.
//
// The Xbox 360 XMA decoder is a memory-mapped device. GTA IV's statically
// linked audio code reads the context-array address from the register file at
// 0x7FEA0000 (index 0x600 = ContextArrayAddress, first seen at 0x7FEA1800) and
// then drives 64-byte context records in guest physical memory directly. This
// file models exactly that state: a real, physically addressed context array,
// the register file, and XMACreateContext/XMAReleaseContext. Register layout:
// public rexglue-sdk c94f5eb include/rex/audio/xma/register_table.inc (BSD-3,
// derived from Xenia); no reference code is copied.
//
// Optional XMAFRAMES decoding uses a separately pinned FFmpeg dependency.
// Without that dependency kicks never consume input (runtime/docs/XMA.md).
#include <string>
#include "diagnostics.h"
#include <atomic>
#include <time.h>
#include "rcomp/runtime/xma.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "xma_decoder.h"
#include "xma_internal.h"
#include "hle_more.h"
#include "physical_window.h"
#ifndef RCOMP_XMA_DECODER
#define RCOMP_XMA_DECODER 0
#endif

#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/virtual_fields.h"
#include "rcomp/xenos_gpu.h"

namespace rcomp::rt {
namespace {

constexpr uint32_t kRegisterBase = 0x7FEA0000u;
constexpr uint32_t kRegisterSize = 0x4000u;
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextBytes = 64;
constexpr uint32_t kArrayBytes = kContextCount * kContextBytes;
constexpr uint32_t kRegContextArrayAddress = 0x600;
constexpr uint32_t kRegCurrentContextIndex = 0x606;
constexpr uint32_t kRegNextContextIndex = 0x607;
constexpr uint32_t kRegKick = 0x650;   // + 0..9
constexpr uint32_t kRegLock = 0x690;   // + 0..9
constexpr uint32_t kRegClear = 0x6A0;  // + 0..9
constexpr uint32_t kRegGroup = 10;
constexpr uint32_t kStatusNoMemory = 0xC0000017u;

struct XmaState {
    std::mutex mutex;
    bool ready = false;
    uint64_t generation = 0;
    uint32_t array_va = 0;
    uint32_t array_phys = 0;
    std::array<bool, kContextCount> allocated{};
    std::array<uint32_t, kRegGroup> kick{}, lock{}, clear{};
    std::array<bool, kRegGroup> kick_reported{};
    uint32_t current = 0, next = 0;
    // Registers with no published meaning behave as plain storage (zero-initialised), like the
    // public reference register file; only the documented ones above have side effects.
    std::array<uint32_t, kRegisterSize / 4> raw{};
#if RCOMP_XMA_DECODER
    std::condition_variable work;
    std::thread worker;
    bool stopping = false;
    std::array<bool, kContextCount> pending{};
    std::array<xma::Decoder, kContextCount> decoders;
#endif
};
XmaState g_xma;

bool in_group(uint32_t index, uint32_t first) { return index >= first && index < first + kRegGroup; }

#if RCOMP_XMA_DECODER
bool load_context(uint32_t id, xma::Context* out) {
    for (uint32_t word = 0; word < out->size(); ++word)
        if (!guest_read_be32(g_xma.array_va + id * kContextBytes + word * 4, &(*out)[word])) return false;
    return true;
}
bool publish_context(uint32_t id, const xma::Context& before, const xma::Context& after) {
    // Guest owns addresses, packet counts, output-read cursor and format. The
    // lock register fences the worker before it modifies these fields; merge
    // hardware-owned fields so asynchronous output reads aren't overwritten.
    xma::Context fresh;
    Runtime* r = runtime();
    const uint32_t address = g_xma.array_va + id * kContextBytes;
    if (!r || !r->mem->is_accessible(address, kContextBytes, Protect::ReadWrite) ||
        !load_context(id, &fresh)) return false;
    constexpr uint32_t masks[16] = {0xF83FF000u, 0x80000000u, xma::kCursorMask, 0,
                                    0x80000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // Valid input flags are the final publication, after cursor/output state.
    for (uint32_t n = 1; n <= fresh.size(); ++n) {
        const uint32_t word = n % fresh.size();
        if (!masks[word] || before[word] == after[word]) continue;
        if (!guest_write_be32(address + word * 4,
                             (fresh[word] & ~masks[word]) | (after[word] & masks[word]))) return false;
    }
    return true;
}
void xma_worker() {
    apply_host_service_affinity("XMA");
    std::unique_lock<std::mutex> lock(g_xma.mutex);
    for (;;) {
        g_xma.work.wait(lock, [] {
            if (g_xma.stopping) return true;
            for (bool pending : g_xma.pending) if (pending) return true;
            return false;
        });
        if (g_xma.stopping) return;
        uint32_t id = g_xma.next;
        for (uint32_t n = 0; n < kContextCount; ++n, id = (id + 1) % kContextCount) {
            if (!g_xma.pending[id]) continue;
            g_xma.pending[id] = false;
            g_xma.current = id;
            g_xma.next = (id + 1) % kContextCount;
            Runtime* r = runtime();
            xma::Context before;
            if (g_xma.ready && g_xma.allocated[id] && r && r->generation == g_xma.generation &&
                load_context(id, &before)) {
                xma::Context after = before;
                if (g_xma.decoders[id].work(*r->mem, after, id) && !publish_context(id, before, after)) {
                    g_xma.decoders[id].reset();
                    std::fprintf(stderr, "RCOMP-XMA FAIL context=%u: guest context publication inaccessible\n", id);
                }
            }
            // The device has finished this context and now selects its next
            // candidate. LOCK is synchronously fenced by this same mutex.
            g_xma.current = g_xma.next;
            break;
        }
        // Release between contexts so lock/reset requests cannot starve.
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
    }
}
#endif

#if RCOMP_RUNTIME_DIAGNOSTICS
// bring-up tracing: which registers the title touches
std::atomic<uint64_t> g_reg_reads[kRegisterSize / 4], g_reg_writes[kRegisterSize / 4];
std::atomic<uint64_t> g_reg_total{0};
void trace_register(uint32_t index, bool write, uint64_t value, uint32_t lr) {
    (write ? g_reg_writes : g_reg_reads)[index].fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = g_reg_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 150 || (write && n <= 3000))
        std::fprintf(stderr, "RCOMP-XMAREG %s reg=0x%03X value=0x%llX lr=0x%08X\n", write ? "write" : "read", index, (unsigned long long)value, lr);
    if (n % 200000 == 0) {
        std::string line;
        for (uint32_t i = 0; i < kRegisterSize / 4; ++i) {
            const uint64_t r = g_reg_reads[i].load(), w = g_reg_writes[i].load();
            if (r + w >= 1000) line += " 0x" + std::to_string(i) + ":r" + std::to_string(r) + "/w" + std::to_string(w);
        }
        std::fprintf(stderr, "RCOMP-XMAREG-SUMMARY%s\n", line.c_str());
    }
}
#endif

bool apply_register_locked(uint32_t index, uint64_t value, uint32_t* context);
void report_kick(bool report, uint32_t context);

VirtualAccessStatus xma_read(uint32_t ea, uint8_t width, uint64_t* value, VirtualAccessDiagnostic* diagnostic) {
    if (diagnostic) {
        diagnostic->provider = "xma-registers";
        diagnostic->region_base = kRegisterBase;
        diagnostic->offset = ea - kRegisterBase;
    }
    if (width != 4 || !value) return VirtualAccessStatus::InvalidWidth;
    const uint32_t index = (ea - kRegisterBase) / 4;
#if RCOMP_RUNTIME_DIAGNOSTICS
    trace_register(index, false, 0, diagnostic ? diagnostic->lr : 0);
#endif
    std::lock_guard<std::mutex> lock(g_xma.mutex);
    if (!g_xma.ready) return VirtualAccessStatus::Stale;
    uint32_t device_value = 0;
    if (index == kRegContextArrayAddress) device_value = g_xma.array_phys;
    else if (index == kRegCurrentContextIndex) {
#if RCOMP_XMA_DECODER
        device_value = g_xma.current;
#else
        // The decoder sweeps its context array continuously; the title polls this register waiting for
        // the sweep to move (found: 130k polls, always 0, stalled audio). Modelled as a fixed-rate sweep
        // of one context per 10 us. No decoding happens behind it (see the NOT implemented section).
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
        device_value = uint32_t((ns / 10000ull) % kContextCount);
#endif
    }
    else if (index == kRegNextContextIndex) device_value = g_xma.next;
    else if (in_group(index, kRegKick)) device_value = g_xma.kick[index - kRegKick];
    else if (in_group(index, kRegLock)) device_value = g_xma.lock[index - kRegLock];
    else if (in_group(index, kRegClear)) device_value = g_xma.clear[index - kRegClear];
    else device_value = g_xma.raw[index];
    // The register file is little-endian hardware and the title reads/writes it with lwbrx/stwbrx, so
    // the value a plain guest word access sees is the byte-swapped register (found: the title computed
    // context indices from a byte-swapped ContextArrayAddress and kicked registers 0xE10+).
    *value = __builtin_bswap32(device_value);
    return VirtualAccessStatus::Handled;
}

VirtualAccessStatus xma_write(uint32_t ea, uint8_t width, uint64_t value, VirtualAccessDiagnostic* diagnostic) {
    if (diagnostic) {
        diagnostic->provider = "xma-registers";
        diagnostic->region_base = kRegisterBase;
        diagnostic->offset = ea - kRegisterBase;
    }
    if (width != 4) return VirtualAccessStatus::InvalidWidth;
    const uint32_t index = (ea - kRegisterBase) / 4;
    value = __builtin_bswap32(uint32_t(value));  // see xma_read: device value is the byte-swapped guest word
#if RCOMP_RUNTIME_DIAGNOSTICS
    trace_register(index, true, value, diagnostic ? diagnostic->lr : 0);
#endif
    bool report = false;
    uint32_t context = 0;
    {
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        if (!g_xma.ready) return VirtualAccessStatus::Stale;
        if (index == kRegContextArrayAddress) return VirtualAccessStatus::ReadOnly;
        report = apply_register_locked(index, uint32_t(value), &context);
    }
    report_kick(report, context);
    return VirtualAccessStatus::Handled;
}

// The side effects of a store of `value` to register `index` (not ContextArrayAddress). Caller holds
// g_xma.mutex with the device ready. True when this is the first kick of a register group (`*context`).
bool apply_register_locked(uint32_t index, uint64_t value, uint32_t* context) {
    bool report = false;
    {
        if (index == kRegCurrentContextIndex) g_xma.current = uint32_t(value) % kContextCount;
        else if (index == kRegNextContextIndex) g_xma.next = uint32_t(value) % kContextCount;
        else if (in_group(index, kRegKick)) {
            *context = index - kRegKick;
            g_xma.kick[*context] = uint32_t(value);
            if (value && !g_xma.kick_reported[*context]) { g_xma.kick_reported[*context] = true; report = true; }
#if RCOMP_XMA_DECODER
            for (unsigned bit = 0; bit < 32; ++bit)
                if (value & (1u << bit)) g_xma.pending[*context * 32 + bit] = true;
            g_xma.work.notify_one();
#endif
        } else if (in_group(index, kRegLock)) {
            const uint32_t group = index - kRegLock;
            g_xma.lock[group] = uint32_t(value);
#if RCOMP_XMA_DECODER
            for (unsigned bit = 0; bit < 32; ++bit)
                if (value & (1u << bit)) g_xma.pending[group * 32 + bit] = false;
            // No decoder can still be active while this mutex is held. Move
            // the idle scheduling candidate outside this locked group mask.
            for (unsigned n = 0; n < kContextCount && g_xma.current / 32 == group &&
                 (value & (1u << (g_xma.current % 32))); ++n)
                g_xma.current = (g_xma.current + 1) % kContextCount;
            g_xma.next = g_xma.current;
#endif
        } else if (in_group(index, kRegClear)) {
            const uint32_t group = index - kRegClear;
            g_xma.clear[group] = uint32_t(value);
#if RCOMP_XMA_DECODER
            for (unsigned bit = 0; bit < 32; ++bit) if (value & (1u << bit)) {
                const uint32_t id = group * 32 + bit;
                g_xma.pending[id] = false;
                g_xma.decoders[id].reset();
                xma::Context c;
                if (load_context(id, &c)) {
                    c[0] &= ~(xma::kInputValid | 0xF8000000u);
                    c[1] &= ~0x80000000u;
                    c[2] = (c[2] & ~xma::kCursorMask) | 32;
                    c[9] &= ~31u;
                    for (unsigned word : {0u, 1u, 2u, 9u})
                        guest_write_be32(g_xma.array_va + id * kContextBytes + word * 4, c[word]);
                }
            }
#endif
        }
        else g_xma.raw[index] = uint32_t(value);
    }
    return report;
}

// A kick records requested work only. Without a decoder, neither input
// valid flags nor packet/output fields may claim that work completed.
void report_kick(bool report, uint32_t context) {
    if (report)
#if RCOMP_XMA_DECODER
        std::fprintf(stdout, "RCOMP-XMA kick register=%u: XMAFRAMES decoder enabled\n", context);
#else
        std::fprintf(stdout, "RCOMP-XMA kick register=%u: XMA2 decoder not implemented, input not consumed\n", context);
#endif
}

const VirtualFieldProvider kProvider{kRegisterBase, kRegisterSize, "xma-registers", &xma_read, &xma_write};

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

// XMACreateContext(PDWORD ContextOut) -> NTSTATUS. Allocates one of the 320
// hardware contexts (zeroed) and returns its guest address in the physical
// window; exhaustion is STATUS_NO_MEMORY.
void XMACreateContext(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XMACreateContext");
    const uint32_t out = ctx.r3.u32;
    if (!out || (out & 3) || !r.mem->is_accessible(out, 4, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!XMACreateContext output 0x%08X lr=0x%08X", out,
                    (uint32_t)ctx.lr);
    uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        if (!g_xma.ready) rcomp_fatal(RCOMP_FATAL_INTERNAL, "XMACreateContext without XMA state");
        for (uint32_t i = 0; i < kContextCount; ++i) {
            if (g_xma.allocated[i]) continue;
            g_xma.allocated[i] = true;
            address = g_xma.array_va + i * kContextBytes;
#if RCOMP_XMA_DECODER
            g_xma.pending[i] = false;
            g_xma.decoders[i].reset();
#endif
            break;
        }
    }
    if (!address) { ctx.r3.u64 = kStatusNoMemory; return; }
    for (uint32_t i = 0; i < kContextBytes; ++i) {
        uint8_t* p = r.mem->translate(address + i, 1);
        if (p) *p = 0;
    }
    guest_write_be32(out, address);
    ctx.r3.u64 = 0;
}

// XMAReleaseContext(PVOID Context) -> VOID. Only a context returned by
// XMACreateContext and not yet released is valid.
void XMAReleaseContext(PPCContext& ctx, uint8_t*) {
    const uint32_t address = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(g_xma.mutex);
    const uint32_t offset = address - g_xma.array_va;
    if (!g_xma.ready || address < g_xma.array_va || offset >= kArrayBytes || offset % kContextBytes ||
        !g_xma.allocated[offset / kContextBytes])
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!XMAReleaseContext invalid context 0x%08X lr=0x%08X",
                    address, (uint32_t)ctx.lr);
    g_xma.allocated[offset / kContextBytes] = false;
#if RCOMP_XMA_DECODER
    g_xma.pending[offset / kContextBytes] = false;
    g_xma.decoders[offset / kContextBytes].reset();
#endif
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x0224, "XMACreateContext", &XMACreateContext},
    {0x0226, "XMAReleaseContext", &XMAReleaseContext},
};

}  // namespace

Status register_xboxkrnl_xma_hle() {
    Runtime& r = rt_or_die("register_xboxkrnl_xma_hle");
#if RCOMP_XMA_DECODER
    xma::install_decoder_logger();
    if (!xma::decoder_available()) {
        std::fprintf(stderr, "RCOMP-XMA FAIL: configured FFmpeg lacks XMAFRAMES decoder\n");
        return Status::Unsupported;
    }
#endif
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    {
        // The XMA* context helpers of src/hle_xboxkrnl_xma_api.cpp.
        const Status status = register_xboxkrnl_xma_api_hle();
        if (status != Status::Ok) return status;
    }
    {
        // Registering twice for one Runtime keeps the same hardware state.
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        if (g_xma.ready && g_xma.generation == r.generation) return Status::Ok;
    }
    (void)unregister_virtual_field_provider(kProvider);
    reset_xma();
    uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(r.physical_allocation_mutex);
        const uint64_t lo = xenos::kXenosPhysicalWindow;
        const uint64_t hi = uint64_t(xenos::kXenosPhysicalWindow) + xenos::kXenosPhysicalSize;
        const Status status = r.physical.alloc_in(0x10000, 0x10000, lo, hi, true, true, &address);
        if (status != Status::Ok) return status;
    }
    {
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        g_xma.allocated.fill(false);
        g_xma.kick.fill(0);
        g_xma.lock.fill(0);
        g_xma.clear.fill(0);
        g_xma.kick_reported.fill(false);
        g_xma.current = g_xma.next = 0;
        g_xma.raw.fill(0);
        g_xma.array_va = address;
        g_xma.array_phys = address - xenos::kXenosPhysicalWindow;
        g_xma.generation = r.generation;
        g_xma.ready = true;
#if RCOMP_XMA_DECODER
        g_xma.pending.fill(false);
        g_xma.stopping = false;
        g_xma.worker = std::thread(&xma_worker);
#endif
    }
    const Status status = register_virtual_field_provider(kProvider);
    if (status != Status::Ok) { reset_xma(); return status; }
    return Status::Ok;
}

void reset_xma() {
    {
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        g_xma.ready = false;
#if RCOMP_XMA_DECODER
        g_xma.stopping = true;
        g_xma.pending.fill(false);
        g_xma.work.notify_one();
#endif
    }
#if RCOMP_XMA_DECODER
    if (g_xma.worker.joinable()) g_xma.worker.join();
#endif
    std::lock_guard<std::mutex> lock(g_xma.mutex);
#if RCOMP_XMA_DECODER
    for (auto& decoder : g_xma.decoders) decoder.reset();
#endif
    g_xma.array_va = g_xma.array_phys = 0;
}

// ---- kernel helper access (src/xma_internal.h) -------------------------------------------------------
namespace xma_device {

bool with_context(uint32_t address, const Signal* signal, const std::function<void(uint32_t record)>& body) {
    Runtime* r = runtime();
    if (!r || !r->mem) return false;
    const uint32_t canonical = physical_canonical(*r->mem, address);
    bool report = false;
    uint32_t reported = 0;
    {
        std::lock_guard<std::mutex> lock(g_xma.mutex);
        const uint32_t offset = canonical - g_xma.array_va;
        if (!g_xma.ready || g_xma.generation != r->generation || canonical < g_xma.array_va ||
            offset >= kArrayBytes || offset % kContextBytes || !g_xma.allocated[offset / kContextBytes])
            return false;
        const uint32_t id = offset / kContextBytes;
        if (signal) {
            const uint32_t first = *signal == Signal::Kick ? kRegKick : *signal == Signal::Lock ? kRegLock : kRegClear;
            report = apply_register_locked(first + id / 32, 1u << (id % 32), &reported);
        }
        body(canonical);
    }
    report_kick(report, reported);
    return true;
}

}  // namespace xma_device

}  // namespace rcomp::rt
