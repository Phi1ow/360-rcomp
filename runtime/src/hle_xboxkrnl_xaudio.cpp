// Original R-comp render driver over the platform audio contract. Guest
// frames are six BE float planes of 256 samples. Accepted frames are copied
// into an active bounded platform queue, never acknowledged by a discard sink.
#include "rcomp/runtime/xaudio.h"
#include "hle_more.h"
#include <pthread.h>
#include <time.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <mutex>
#include "rcomp/audio_output.h"
#include "rcomp/diag.h"
#include "rcomp/func_table.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/thread_object.h"

namespace rcomp::rt {
namespace {
constexpr uint32_t kMaxClients = 8;
constexpr uint32_t kDriverTag = 0x41550000u, kDriverTagMask = 0xFFFF0000u;
constexpr uint32_t kSamplesPerFrame = 256, kChannels = 6;
constexpr uint32_t kFrameBytes = kSamplesPerFrame * kChannels * 4;
constexpr long kFrameNanoseconds = 5333333;
constexpr uint32_t kEInvalidArg = 0x80070057u, kEOutOfMemory = 0x8007000Eu;
constexpr uint32_t kENotImpl = 0x80004001u, kEFail = 0x80004005u;
constexpr uint32_t kEBusy = 0x800700AAu;
constexpr uint32_t kCallbackStackSize = 0x40000;
enum class ClientPhase { Free, Active, Closing };
enum class WorkerPhase { Stopped, Starting, Running };
struct Client {
    ClientPhase phase = ClientPhase::Free;
    uint32_t callback = 0, argument_wrapper = 0;
    rcomp_audio_stream stream = 0;
    unsigned leases = 0;
    bool submit_failure_reported = false;
};
struct AudioState {
    // Lifecycle operations serialize separately from callback/submit leases.
    // No platform open/submit/close call holds mutex.
    std::mutex lifecycle, mutex;
    std::condition_variable changed;
    std::array<Client, kMaxClients> clients{};
    uint64_t generation = 0, submitted = 0;
    WorkerPhase worker = WorkerPhase::Stopped;
    pthread_t thread{};
    bool closing = false;
    std::atomic<bool> stop{false};
};
AudioState g_audio;
Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}
uint32_t audio_result(int result) {
    switch (result) {
    case RCOMP_AUDIO_OK: return 0;
    case RCOMP_AUDIO_UNSUPPORTED: return kENotImpl;
    case RCOMP_AUDIO_INVALID_ARGUMENT: return kEInvalidArg;
    case RCOMP_AUDIO_BUSY: return kEBusy;
    default: return kEFail;
    }
}
bool decode_driver(uint32_t driver, uint32_t* slot) {
    if ((driver & kDriverTagMask) != kDriverTag || (driver & 0xFFFFu) >= kMaxClients) return false;
    *slot = driver & 0xFFFFu; return true;
}
bool on_audio_thread() {
    std::lock_guard<std::mutex> lock(g_audio.mutex);
    return g_audio.worker != WorkerPhase::Stopped && pthread_equal(pthread_self(), g_audio.thread);
}
void release_lease(uint32_t slot) {
    std::lock_guard<std::mutex> lock(g_audio.mutex);
    if (!g_audio.clients[slot].leases)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "XAudio lease underflow");
    --g_audio.clients[slot].leases;
    g_audio.changed.notify_all();
}
void free_wrapper(Runtime& r, uint32_t wrapper) {
    const Status status = r.heap.free(wrapper);
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio argument cleanup: %s", status_name(status));
}
void retire_callback(Runtime& r, GuestThread& guest) {
    const Status retired = retire_guest_thread(guest, 0);
    if (retired != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "XAudio callback retirement: %s", status_name(retired));
    const Status destroyed = destroy_guest_thread(r.heap, &guest);
    if (destroyed != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio callback cleanup: %s", status_name(destroyed));
}
void* frame_clock(void*) {
    apply_host_service_affinity("XAUDIO");
    Runtime& r = rt_or_die("XAudio frame clock");
    const uint64_t generation = r.generation;
    GuestThread guest; PPCContext context;
    auto create = [&] {
        if (create_guest_thread(r.heap, {kCallbackStackSize, 0, 0}, &context, &guest) != Status::Ok)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio frame clock: no callback guest memory");
    };
    create();
    timespec next{}; clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_audio.stop.load(std::memory_order_acquire)) {
        next.tv_nsec += kFrameNanoseconds;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        for (uint32_t slot = 0; slot < kMaxClients; ++slot) {
            uint32_t callback = 0, wrapper = 0;
            {
                std::lock_guard<std::mutex> lock(g_audio.mutex);
                auto& client = g_audio.clients[slot];
                if (g_audio.stop.load(std::memory_order_acquire) || client.phase != ClientPhase::Active) continue;
                ++client.leases; callback = client.callback; wrapper = client.argument_wrapper;
            }
            if (!runtime() || runtime()->generation != generation)
                rcomp_fatal(RCOMP_FATAL_INTERNAL, "XAudio callback owner generation expired before join");
            if (thread_object_exited(guest.identity)) { retire_callback(r, guest); create(); }
            PPCFunc* fn = lookup_function(callback);
            if (!fn) rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "XAudio callback 0x%08X has no function", callback);
            context = PPCContext{};
            context.r1.u64 = guest.initial_r1; context.r13.u64 = guest.pcr;
            context.r3.u64 = wrapper; context.lr = kGuestLrSentinel; context.fpscr.loadFromHost();
            uint32_t code = 0;
            if (run_guest_callback(guest, context, r.mem->base(), fn, &code) != Status::Ok)
                rcomp_fatal(RCOMP_FATAL_INTERNAL, "XAudio callback cannot run on its host thread");
            release_lease(slot);
        }
    }
    retire_callback(r, guest);
    return nullptr;
}

// Client = {be32 Callback, be32 Argument}. The callback receives the address
// of a runtime-owned be32 argument wrapper; it persists through active leases.
void XAudioRegisterRenderDriverClient(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XAudioRegisterRenderDriverClient");
    const uint32_t client = ctx.r3.u32, out = ctx.r4.u32;
    uint32_t callback = 0, argument = 0;
    if (!out || !r.mem->is_accessible(out, 4, Protect::ReadWrite) || !client ||
        !r.mem->is_accessible(client, 8, Protect::Read) || !guest_read_be32(client, &callback) ||
        !guest_read_be32(client + 4, &argument) || !callback) { ctx.r3.u64 = kEInvalidArg; return; }
    if (!lookup_function(callback)) rcomp_fatal(RCOMP_FATAL_INDIRECT_TARGET, "XAudio callback 0x%08X missing", callback);
    // A callback never blocks on shutdown holding lifecycle while joining it.
    std::unique_lock<std::mutex> lifecycle(g_audio.lifecycle, std::defer_lock);
    if (on_audio_thread()) {
        if (!lifecycle.try_lock()) { ctx.r3.u64 = kEBusy; return; }
    } else lifecycle.lock();
    uint32_t slot = kMaxClients;
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (g_audio.closing) { ctx.r3.u64 = kEFail; return; }
        for (uint32_t n = 0; n < kMaxClients; ++n)
            if (g_audio.clients[n].phase == ClientPhase::Free) { slot = n; break; }
    }
    if (slot == kMaxClients) { ctx.r3.u64 = kEOutOfMemory; return; }
    uint32_t wrapper = 0;
    if (r.heap.alloc(4, 4, false, &wrapper) != Status::Ok) { ctx.r3.u64 = kEOutOfMemory; return; }
    if (!guest_write_be32(wrapper, argument))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XAudio argument wrapper inaccessible");
    rcomp_audio_stream stream = 0;
    const int opened = rcomp_audio_open(&stream);
    if (opened != RCOMP_AUDIO_OK) { free_wrapper(r, wrapper); ctx.r3.u64 = audio_result(opened); return; }
    if (!stream) rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio backend returned OK without stream");
    int error = 0;
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (g_audio.worker == WorkerPhase::Stopped) {
            g_audio.worker = WorkerPhase::Starting;
            g_audio.stop.store(false, std::memory_order_release);
            pthread_attr_t attr; error = pthread_attr_init(&attr);
            if (!error) {
                error = pthread_attr_setstacksize(&attr, 1u << 20);
                if (!error) error = pthread_create(&g_audio.thread, &attr, frame_clock, nullptr);
                const int destroyed = pthread_attr_destroy(&attr);
                if (!error && destroyed) rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio pthread attr cleanup %d", destroyed);
            }
            g_audio.worker = error ? WorkerPhase::Stopped : WorkerPhase::Running;
        }
        if (!error) {
            g_audio.clients[slot] = {ClientPhase::Active, callback, wrapper, stream, 0};
            g_audio.generation = r.generation;
            if (!guest_write_be32(out, kDriverTag | slot))
                rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XAudio driver output became inaccessible");
        }
    }
    if (error) {
        if (rcomp_audio_close(stream) != RCOMP_AUDIO_OK)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio failed-start backend cleanup");
        free_wrapper(r, wrapper); ctx.r3.u64 = kEFail; return;
    }
    std::fprintf(stderr, "RCOMP-XAUDIO client=%u callback=0x%08X argument=0x%08X wrapper=0x%08X stream=0x%llX\n",
                 slot, callback, argument, wrapper, (unsigned long long)stream);
    ctx.r3.u64 = 0;
}
void XAudioUnregisterRenderDriverClient(PPCContext& ctx, uint8_t*) {
    uint32_t slot = 0;
    if (!decode_driver(ctx.r3.u32, &slot)) { ctx.r3.u64 = kEInvalidArg; return; }
    // Waiting on this callback's own lease would deadlock its host thread.
    if (on_audio_thread()) { ctx.r3.u64 = kEBusy; return; }
    std::unique_lock<std::mutex> lifecycle(g_audio.lifecycle);
    rcomp_audio_stream stream = 0; uint32_t wrapper = 0;
    {
        std::unique_lock<std::mutex> lock(g_audio.mutex);
        auto& client = g_audio.clients[slot];
        if (client.phase == ClientPhase::Free) { ctx.r3.u64 = kEInvalidArg; return; }
        client.phase = ClientPhase::Closing;
        g_audio.changed.wait(lock, [&] { return !client.leases; });
        stream = client.stream; wrapper = client.argument_wrapper;
    }
    const int closed = rcomp_audio_close(stream);
    if (closed != RCOMP_AUDIO_OK) { ctx.r3.u64 = audio_result(closed); return; }
    free_wrapper(rt_or_die("XAudioUnregisterRenderDriverClient"), wrapper);
    { std::lock_guard<std::mutex> lock(g_audio.mutex); g_audio.clients[slot] = Client{}; }
    ctx.r3.u64 = 0;
}
// At most one real Submit failure per registered client lifetime. No clocks
// or atomics per frame; platform-native failures keep their separate log.
void report_submit_failure(uint32_t slot, uint32_t driver, uint32_t samples,
                           uint32_t hresult, bool backend_called, int backend_result) {
    rcomp_audio_stream stream = 0;
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        auto& client = g_audio.clients[slot];
        if (client.phase == ClientPhase::Free || client.submit_failure_reported) return;
        client.submit_failure_reported = true;
        stream = client.stream;
    }
    std::fprintf(stderr, "RCOMP-XAUDIO-SUBMIT client=%u driver=0x%08X samples=0x%08X stream=0x%llX hresult=0x%08X backend_called=%u",
                 slot, driver, samples, (unsigned long long)stream, hresult, unsigned(backend_called));
    if (backend_called) std::fprintf(stderr, " backend_result=%d", backend_result);
    std::fprintf(stderr, "\n");
}
void XAudioSubmitRenderDriverFrame(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XAudioSubmitRenderDriverFrame");
    uint32_t slot = 0;
    const uint32_t samples = ctx.r4.u32;
    const uint32_t driver = ctx.r3.u32;
    if (!decode_driver(driver, &slot)) { ctx.r3.u64 = kEInvalidArg; return; }
    if (!samples || !r.mem->is_accessible(samples, kFrameBytes, Protect::Read)) {
        report_submit_failure(slot, driver, samples, kEInvalidArg, false, 0);
        ctx.r3.u64 = kEInvalidArg; return;
    }
    rcomp_audio_stream stream = 0;
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        auto& client = g_audio.clients[slot];
        if (client.phase == ClientPhase::Free) { ctx.r3.u64 = kEInvalidArg; return; }
        active = client.phase == ClientPhase::Active && !g_audio.closing;
        if (active) { ++client.leases; stream = client.stream; }
    }
    if (!active) {
        report_submit_failure(slot, driver, samples, kEFail, false, 0);
        ctx.r3.u64 = kEFail; return;
    }
    std::array<float, kSamplesPerFrame * kChannels> interleaved;
    const uint8_t* input = r.mem->translate(samples, kFrameBytes);
    if (!input) rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "XAudio frame became inaccessible");
    for (unsigned frame = 0; frame < kSamplesPerFrame; ++frame)
        for (unsigned channel = 0; channel < kChannels; ++channel) {
            uint32_t bits; std::memcpy(&bits, input + (channel * kSamplesPerFrame + frame) * 4, 4);
            bits = __builtin_bswap32(bits);
            std::memcpy(&interleaved[frame * kChannels + channel], &bits, 4);
        }
    const int result = rcomp_audio_submit(stream, interleaved.data());
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (result == RCOMP_AUDIO_OK) ++g_audio.submitted;
    }
    const uint32_t hresult = audio_result(result);
    if (result != RCOMP_AUDIO_OK)
        report_submit_failure(slot, driver, samples, hresult, true, result);
    release_lease(slot);
    ctx.r3.u64 = hresult;
}

// Original fixed guest speaker/category queries remain below this insertion.
// XAudioGetSpeakerConfig(PDWORD Config) -> HRESULT. R-comp does not query the
// PS5 audio output; it reports the fixed console profile 0x00010001 used by the
// public rexglue-sdk c94f5eb reference (a compatibility value, not a claim about
// the attached speakers).
void XAudioGetSpeakerConfig(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XAudioGetSpeakerConfig");
    const uint32_t out = ctx.r3.u32;
    if (!out || !r.mem->is_accessible(out, 4, Protect::ReadWrite)) { ctx.r3.u64 = kEInvalidArg; return; }
    guest_write_be32(out, 0x00010001u);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolumeChangeMask(DWORD Driver, PDWORD Mask) -> HRESULT:
// no system mixer exists, so no category volume ever changes: Mask = 0.
void XAudioGetVoiceCategoryVolumeChangeMask(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XAudioGetVoiceCategoryVolumeChangeMask");
    uint32_t slot = 0;
    const uint32_t out = ctx.r4.u32;
    if (!out || !r.mem->is_accessible(out, 4, Protect::ReadWrite)) { ctx.r3.u64 = kEInvalidArg; return; }
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        if (!decode_driver(ctx.r3.u32, &slot) || g_audio.clients[slot].phase != ClientPhase::Active) { ctx.r3.u64 = kEInvalidArg; return; }
    }
    guest_write_be32(out, 0);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolume(DWORD Category, float* Volume) -> HRESULT: every
// category is at unity gain (there is no system volume mixer).
void XAudioGetVoiceCategoryVolume(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("XAudioGetVoiceCategoryVolume");
    const uint32_t out = ctx.r4.u32;
    if (!out || !r.mem->is_accessible(out, 4, Protect::ReadWrite)) { ctx.r3.u64 = kEInvalidArg; return; }
    guest_write_be32(out, 0x3F800000u);  // 1.0f
    ctx.r3.u64 = 0;
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x01F3, "XAudioRegisterRenderDriverClient", &XAudioRegisterRenderDriverClient},
    {0x01F4, "XAudioUnregisterRenderDriverClient", &XAudioUnregisterRenderDriverClient},
    {0x01F5, "XAudioSubmitRenderDriverFrame", &XAudioSubmitRenderDriverFrame},
    {0x01F7, "XAudioGetVoiceCategoryVolumeChangeMask", &XAudioGetVoiceCategoryVolumeChangeMask},
    {0x01F8, "XAudioGetVoiceCategoryVolume", &XAudioGetVoiceCategoryVolume},
    {0x01FF, "XAudioGetSpeakerConfig", &XAudioGetSpeakerConfig},
};

}  // namespace

Status register_xboxkrnl_xaudio_hle() {
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    // Ducker settings: src/hle_xboxkrnl_audio_ducker.cpp.
    return register_xboxkrnl_audio_ducker_hle();
}

void shutdown_xaudio() {
    if (on_audio_thread())
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "XAudio shutdown cannot join its own callback thread");
    std::unique_lock<std::mutex> lifecycle(g_audio.lifecycle);
    pthread_t thread{}; bool join = false;
    {
        std::lock_guard<std::mutex> lock(g_audio.mutex);
        g_audio.closing = true;
        g_audio.stop.store(true, std::memory_order_release);
        if (g_audio.worker == WorkerPhase::Running) { join = true; thread = g_audio.thread; }
        for (auto& client : g_audio.clients)
            if (client.phase == ClientPhase::Active) client.phase = ClientPhase::Closing;
    }
    if (join) {
        const int joined = pthread_join(thread, nullptr);
        if (joined) rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio callback join failed %d", joined);
    }
    for (uint32_t slot = 0; slot < kMaxClients; ++slot) {
        uint32_t wrapper = 0; rcomp_audio_stream stream = 0;
        {
            std::unique_lock<std::mutex> lock(g_audio.mutex);
            auto& client = g_audio.clients[slot];
            if (client.phase == ClientPhase::Free) continue;
            g_audio.changed.wait(lock, [&] { return !client.leases; });
            wrapper = client.argument_wrapper; stream = client.stream;
        }
        const int result = rcomp_audio_close(stream);
        if (result != RCOMP_AUDIO_OK)
            rcomp_fatal(RCOMP_FATAL_PLATFORM, "XAudio backend shutdown failed %d; stream retained", result);
        free_wrapper(rt_or_die("XAudio shutdown"), wrapper);
        { std::lock_guard<std::mutex> lock(g_audio.mutex); g_audio.clients[slot] = Client{}; }
    }
    std::lock_guard<std::mutex> lock(g_audio.mutex);
    g_audio.worker = WorkerPhase::Stopped;
    g_audio.generation = g_audio.submitted = 0;
    g_audio.closing = false;
}

}  // namespace rcomp::rt
