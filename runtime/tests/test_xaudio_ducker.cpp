// XAudio ducker settings (src/hle_xboxkrnl_audio_ducker.cpp): the enabled flag round-trips through
// XAudioEnableDucker / XAudioIsDuckerEnabled, the getters report the (all-zero) settings, an output that
// is not writable guest memory is an ABI fatal, and a new Runtime starts from the initial state.
#include <initializer_list>

#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"
#include "test_util.h"

PPC_EXTERN_FUNC(__imp__XAudioEnableDucker);
PPC_EXTERN_FUNC(__imp__XAudioIsDuckerEnabled);
PPC_EXTERN_FUNC(__imp__XAudioGetDuckerLevel);
PPC_EXTERN_FUNC(__imp__XAudioGetDuckerThreshold);
PPC_EXTERN_FUNC(__imp__XAudioGetDuckerAttackTime);
PPC_EXTERN_FUNC(__imp__XAudioGetDuckerHoldTime);
PPC_EXTERN_FUNC(__imp__XAudioGetDuckerReleaseTime);

using namespace rcomp;
using namespace rcomp::rt;

namespace {
GuestMemory g_mem;
uint32_t g_buf;

uint32_t call(PPCFunc* f, std::initializer_list<uint64_t> args) {
    alignas(64) PPCContext ctx{};
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6};
    size_t i = 0;
    for (uint64_t v : args) regs[i++]->u64 = v;
    f(ctx, g_mem.base());
    return ctx.r3.u32;
}

void start() {
    CHECK_ST(runtime_init(&g_mem), Status::Ok);
    CHECK_ST(register_xboxkrnl_hle(), Status::Ok);
    CHECK_ST(runtime()->heap.alloc(0x100, 16, true, &g_buf), Status::Ok);
}
}  // namespace

int main() {
    if (g_mem.reserve() != MemStatus::Ok) return 2;
    start();
    CHECK_EQ(call(__imp__XAudioIsDuckerEnabled, {}), 0u);
    CHECK_EQ(call(__imp__XAudioEnableDucker, {1}), 0u);
    CHECK_EQ(call(__imp__XAudioIsDuckerEnabled, {}), 1u);
    PPCFunc* getters[] = {__imp__XAudioGetDuckerLevel, __imp__XAudioGetDuckerThreshold, __imp__XAudioGetDuckerAttackTime,
                          __imp__XAudioGetDuckerHoldTime, __imp__XAudioGetDuckerReleaseTime};
    for (PPCFunc* get : getters) {
        guest_write_be32(g_buf, 0xCDCDCDCDu);
        CHECK_EQ(call(get, {g_buf}), 0u);
        uint32_t value = 1;
        guest_read_be32(g_buf, &value);
        CHECK_EQ(value, 0u);
        bool fatal = false;
        CAPTURE_FATAL(call(get, {0}), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
        CAPTURE_FATAL(call(get, {0x00001000}), fatal);
        CHECK(fatal && g_fatal_kind == RCOMP_FATAL_UNIMPLEMENTED);
    }
    CHECK_EQ(call(__imp__XAudioEnableDucker, {0}), 0u);
    CHECK_EQ(call(__imp__XAudioIsDuckerEnabled, {}), 0u);
    CHECK_EQ(call(__imp__XAudioEnableDucker, {0x100}), 0u);  // any non-zero BOOL
    CHECK_EQ(call(__imp__XAudioIsDuckerEnabled, {}), 1u);

    // A new Runtime generation starts disabled.
    runtime_shutdown();
    clear_imports();
    start();
    CHECK_EQ(call(__imp__XAudioIsDuckerEnabled, {}), 0u);
    runtime_shutdown();
    clear_imports();
    return test_result("rt_xaudio_ducker");
}
