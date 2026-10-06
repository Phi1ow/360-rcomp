// Input backend on the console's pad (owner: Agent 2). Controller 0 = the
// initial user's pad; controllers 1..3 are not connected. Vibration is not
// driven yet (no call verified on console): RCOMP_INPUT_UNSUPPORTED.
//
// Calls and their order follow the sequence verified on console by
// ProsperoLight (recorded in PS5_RetroArch@18dc105 src/input_ps5.cpp):
// sceUserServiceInitialize, sceUserServiceGetInitialUser, scePadInit,
// scePadOpen(user, 0, 0, NULL) retried up to 10 times 100 ms apart, then
// scePadRead(handle, samples, capacity) returning the samples since the last
// read. Link: libScePad.so, libSceUserService.so. PS5 execution: NOT TESTED.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include <mutex>
#include <vector>

#include "pad_map.h"
#include "rcomp/input.h"

extern "C" {
int32_t scePadInit();
int32_t scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void* params);
int32_t scePadRead(int32_t handle, void* samples, int32_t capacity);
int32_t sceUserServiceInitialize(const void* params);
int32_t sceUserServiceGetInitialUser(int32_t* user_id);
int32_t sceKernelUsleep(uint32_t microseconds);
}

namespace {

constexpr int kCapacity = 16;
std::mutex g_mu;
bool g_tried = false;
int32_t g_handle = -1;
uint8_t g_samples[kCapacity][rcomp::ps5pad::kSampleSize];
rcomp_pad g_last{};
bool g_connected = false;

// Diagnostic autopilot (measurement builds only, RCOMP_PS5_AUTOPILOT=ON): /app0/autopilot.txt lines
// "<start_ms> <buttons_hex> <duration_ms> [<lx> <ly> <rx> <ry> [<lt> <rt>]]" OR extra XINPUT buttons into
// the real pad state while the title has been running for [start, start+duration); the optional
// stick values (-32768..32767, XInput convention: +y = up) and trigger values (0..255) replace the
// pad's sticks/triggers for that window (the last active step wins). Absent file = no effect. Times
// are milliseconds since the first input read.
struct AutoStep {
    uint32_t start_ms, buttons, duration_ms;
    bool sticks = false, triggers = false;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
};
std::vector<AutoStep> g_script;
bool g_script_loaded = false;
timespec g_script_origin{};

uint64_t elapsed_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t ns = (int64_t(now.tv_sec) - int64_t(g_script_origin.tv_sec)) * 1000000000ll +
                       (int64_t(now.tv_nsec) - int64_t(g_script_origin.tv_nsec));
    return ns > 0 ? uint64_t(ns / 1000000ll) : 0;
}

void load_script() {
    if (g_script_loaded) return;
    g_script_loaded = true;
    clock_gettime(CLOCK_MONOTONIC, &g_script_origin);
#if !RCOMP_PS5_AUTOPILOT
    // Player builds (RCOMP_PS5_AUTOPILOT=OFF) never read a script: only the player's controller drives the game.
    return;
#endif
    FILE* f = fopen("/app0/autopilot.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned start = 0, buttons = 0, duration = 0;
        int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        const int n = sscanf(line, "%u %x %u %d %d %d %d %d %d", &start, &buttons, &duration, &lx, &ly, &rx, &ry, &lt, &rt);
        if (n < 3) continue;
        AutoStep step{start, buttons, duration};
        auto stick = [](int v) { return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v); };
        auto trigger = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
        if (n >= 7) { step.sticks = true; step.lx = stick(lx); step.ly = stick(ly); step.rx = stick(rx); step.ry = stick(ry); }
        if (n >= 9) { step.triggers = true; step.lt = trigger(lt); step.rt = trigger(rt); }
        g_script.push_back(step);
    }
    fclose(f);
    fprintf(stderr, "RCOMP-PAD autopilot: %zu steps loaded\n", g_script.size());
}

// Applies the active script steps to *pad; returns the extra buttons (for the log).
uint16_t apply_autopilot(rcomp_pad* pad) {
    if (g_script.empty()) return 0;
    const uint64_t now = elapsed_ms();
    uint16_t extra = 0;
    for (const AutoStep& step : g_script) {
        if (now < step.start_ms || now >= uint64_t(step.start_ms) + step.duration_ms) continue;
        extra |= uint16_t(step.buttons);
        if (step.sticks) { pad->thumb_lx = step.lx; pad->thumb_ly = step.ly; pad->thumb_rx = step.rx; pad->thumb_ry = step.ry; }
        if (step.triggers) { pad->left_trigger = step.lt; pad->right_trigger = step.rt; }
    }
    pad->buttons |= extra;
    return extra;
}

void open_once() {
    if (g_tried) return;
    g_tried = true;
    sceUserServiceInitialize(nullptr);  // may already be initialised by the system
    int32_t user = -1;
    const int32_t user_rc = sceUserServiceGetInitialUser(&user);
    const int32_t init_rc = scePadInit();
    fprintf(stderr, "RCOMP-PAD user=%d (rc=0x%08X) scePadInit=0x%08X\n", user, (unsigned)user_rc, (unsigned)init_rc);
    if (user_rc < 0 || init_rc < 0) return;
    for (int attempt = 0; attempt < 10 && g_handle < 0; ++attempt) {
        g_handle = scePadOpen(user, 0, 0, nullptr);
        fprintf(stderr, "RCOMP-PAD scePadOpen attempt %d -> 0x%08X\n", attempt, (unsigned)g_handle);
        if (g_handle < 0) sceKernelUsleep(100000);
    }
}

}  // namespace

extern "C" int rcomp_input_read(uint32_t user, rcomp_pad* out) {
    if (user != 0) return RCOMP_INPUT_NOT_CONNECTED;
    std::lock_guard<std::mutex> lk(g_mu);
    open_once();
    if (g_handle < 0) return RCOMP_INPUT_NOT_CONNECTED;
    // The title polls the pad in a tight loop (GetState/GetCapabilities): read the device at most
    // once per millisecond and serve the cached state in between.
    static timespec last_read{};
    static int32_t cached_n = 0;
    timespec now_ts{};
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    const int64_t since_us = (int64_t(now_ts.tv_sec) - last_read.tv_sec) * 1000000ll + (now_ts.tv_nsec - last_read.tv_nsec) / 1000;
    int32_t n = 0;
    if (since_us >= 1000 || since_us < 0) {
        n = scePadRead(g_handle, g_samples, kCapacity);
        timespec done{};
        clock_gettime(CLOCK_MONOTONIC, &done);
        const int64_t took_us = (int64_t(done.tv_sec) - now_ts.tv_sec) * 1000000ll + (done.tv_nsec - now_ts.tv_nsec) / 1000;
        static int slow = 0;
        if (took_us > 5000 && slow < 20) { ++slow; fprintf(stderr, "RCOMP-PAD scePadRead took %lld us (n=%d)\n", (long long)took_us, (int)n); }
        last_read = now_ts;
        cached_n = n;
    } else {
        n = 0;  // no new samples: keep the previous state
        (void)cached_n;
    }
    static int traced = 0;
    if (traced < 12 || (n < 0 && traced < 40)) {
        ++traced;
        fprintf(stderr, "RCOMP-PAD scePadRead -> %d (connected=%d)\n", (int)n, g_connected ? 1 : 0);
    }
    if (n > 0) {
        const uint8_t* s = g_samples[n - 1];  // newest sample
        uint32_t buttons;
        int32_t connected;
        memcpy(&buttons, s, 4);
        memcpy(&connected, s + rcomp::ps5pad::kConnectedOffset, 4);
        g_connected = connected != 0;
        static int shown = 0;
        if (shown < 6) {
            ++shown;
            fprintf(stderr, "RCOMP-PAD sample buttons=0x%08X connected_word=0x%08X bytes:", buttons, (unsigned)connected);
            for (int i = 0; i < 96; ++i) fprintf(stderr, "%s%02X", (i % 16) ? "" : " ", s[i]);
            fprintf(stderr, "\n");
        }
        g_last = rcomp::ps5pad::from_sample(buttons, s[4], s[5], s[6], s[7], s[8], s[9]);
    } else if (n < 0) {
        g_connected = false;
    }
    load_script();
    // Unattended diagnostic runs (autopilot script present): with no physical controller awake the
    // script drives a neutral virtual pad.
    if (!g_connected && g_script.empty()) return RCOMP_INPUT_NOT_CONNECTED;
    *out = g_last;
    const uint16_t extra = apply_autopilot(out);
    if (extra) {
        static uint16_t last_reported = 0;
        if (extra != last_reported) { last_reported = extra; fprintf(stderr, "RCOMP-PAD autopilot buttons=0x%04X at %llu ms\n", extra, (unsigned long long)elapsed_ms()); }
    }
    return RCOMP_INPUT_OK;
}

extern "C" int rcomp_input_set_vibration(uint32_t user, uint16_t, uint16_t) {
    return user == 0 ? RCOMP_INPUT_UNSUPPORTED : RCOMP_INPUT_NOT_CONNECTED;
}
