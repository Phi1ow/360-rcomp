// Xenos PM4 command processor (rexglue, gpu/xenos/rexglue) on R-comp guest
// memory. The test plays the title: it writes PM4 packets into a ring buffer
// in the physical window and publishes CP_RB_WPTR through the guest register
// window, exactly as D3D would; the bridge thread forwards it to the command
// processor. Rendering is a TESTDOUBLE that records draws.
//
// Oracles come from the PM4 packet definitions (rexglue xenos.h /
// command_processor.cpp): register writes, memory writes with endian swap,
// indirect buffers, fences, interrupts, shader loads, indexed draws, read
// pointer write-back. A child process checks that an unknown opcode is
// fatal (patch 0004).
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <rex/graphics/xenos.h>

#include "../../gpu/xenos/rexglue/rcomp/xenos_host.h"
#include "TESTDOUBLE_recording_backend.h"
#include "rcomp/guest_memory.h"

using namespace rcomp::xenos;
namespace xenos = rex::graphics::xenos;

namespace {

int g_fail = 0, g_pass = 0;
FILE* g_out = stdout;
void check(const char* id, bool ok, const char* why = "") {
    fprintf(g_out, "%-52s %s%s%s\n", id, ok ? "PASS" : "FAIL", ok ? "" : " ", ok ? "" : why);
    fflush(g_out);
    g_fail += !ok;
    g_pass += ok;
}

rcomp::GuestMemory g_mem;
uint8_t* g_base;

constexpr uint32_t kRing = 0x00010000;      // physical
constexpr uint32_t kRingLog2 = 13;          // 1 << (13 + 3) = 64 KiB
constexpr uint32_t kRptrWb = 0x00001000;
constexpr uint32_t kMemWrite = 0x00002000;
constexpr uint32_t kFence = 0x00002010;
constexpr uint32_t kIb = 0x00003000;
constexpr uint32_t kIndices = 0x00004000;

uint32_t be(uint32_t v) { return __builtin_bswap32(v); }
uint8_t* phys(uint32_t p) { return g_base + kXenosPhysicalWindow + p; }
uint32_t rd_phys(uint32_t p) {
    uint32_t v;
    memcpy(&v, phys(p), 4);
    return be(v);
}
void wr_phys(uint32_t p, uint32_t v) {
    v = be(v);
    memcpy(phys(p), &v, 4);
}

struct Pm4 {
    std::vector<uint32_t> w;
    void type0(uint32_t reg, std::initializer_list<uint32_t> values) {
        w.push_back((uint32_t(values.size() - 1) << 16) | reg);
        w.insert(w.end(), values);
    }
    void type3(uint32_t opcode, std::initializer_list<uint32_t> payload) {
        w.push_back((3u << 30) | (uint32_t(payload.size() - 1) << 16) | (opcode << 8));
        w.insert(w.end(), payload);
    }
    void type3v(uint32_t opcode, const std::vector<uint32_t>& payload) {
        w.push_back((3u << 30) | (uint32_t(payload.size() - 1) << 16) | (opcode << 8));
        w.insert(w.end(), payload.begin(), payload.end());
    }
    void store(uint32_t at) const {
        for (size_t i = 0; i < w.size(); ++i) wr_phys(at + 4 * uint32_t(i), w[i]);
    }
};

std::atomic<int> g_interrupts{0};
std::atomic<uint32_t> g_last_source{99}, g_last_cpu{99}, g_last_cb{0}, g_last_user{0};
void dispatcher(uint32_t cb, uint32_t user, uint32_t source, uint32_t cpu) {
    g_last_cb = cb;
    g_last_user = user;
    g_last_source = source;
    g_last_cpu = cpu;
    ++g_interrupts;
}

TESTDOUBLE_RecordingBackend* g_backend = nullptr;
BackendFactory recording_factory() {
    return [](rex::graphics::GpuHost* h) {
        auto b = std::make_unique<TESTDOUBLE_RecordingBackend>(h);
        g_backend = b.get();
        return std::unique_ptr<rex::graphics::CommandProcessor>(std::move(b));
    };
}

// The title's side of a kick: store the new write pointer in the register
// window (big-endian), then wait for the read pointer write-back.
bool kick_and_wait(uint32_t wptr_dwords) {
    uint32_t v = be(wptr_dwords);
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + kXenosRegisterWindow + 4 * kRegCpRbWptr), v,
                     __ATOMIC_RELEASE);
    for (int i = 0; i < 2000; ++i) {
        if (rd_phys(kRptrWb) == wptr_dwords) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void setup_memory() {
    if (g_mem.reserve() != rcomp::MemStatus::Ok) exit(2);
    g_base = g_mem.base();
    // What MmAllocatePhysicalMemoryEx would have committed for the title.
    if (g_mem.commit(kXenosPhysicalWindow, 0x100000, rcomp::Protect::ReadWrite) != rcomp::MemStatus::Ok) exit(2);
}

// Child process: an unknown type-3 opcode must stop the process (patch 0004).
int run_unknown_opcode_child() {
    setup_memory();
    HostConfig cfg;
    cfg.vblank_hz = 0;
    if (!gpu_start(g_mem, cfg, dispatcher, recording_factory())) return 3;
    gpu_initialize_ring_buffer(kRing, kRingLog2);
    gpu_enable_read_pointer_writeback(kRptrWb, 2);
    Pm4 p;
    p.type3(0x7E, {0});  // not a PM4 opcode
    p.store(kRing);
    kick_and_wait(uint32_t(p.w.size()));
    return 0;  // reached only if the opcode was ignored
}

}  // namespace

// Everything but the fatal-path check (which needs a child process).
int run_all(const char* self_path) {
    setup_memory();
    HostConfig cfg;
    cfg.vblank_hz = 0;  // interrupts from the command stream only
    check("xenos_cp/start", gpu_start(g_mem, cfg, dispatcher, recording_factory()) && g_backend);
    if (!g_backend) return 1;
    gpu_initialize_ring_buffer(kRing, kRingLog2);
    gpu_enable_read_pointer_writeback(kRptrWb, 2);
    gpu_set_interrupt_callback(0x82001234, 0xABCD);

    // Registers the title reads are published in the window.
    uint32_t v;
    memcpy(&v, g_base + kXenosRegisterWindow + 4 * 0x0F00, 4);
    check("xenos_cp/register_window_published", be(v) == 0x08100748);

    // Indirect buffer: two register writes.
    Pm4 ib;
    ib.type0(0x4010, {0xCAFEF00D, 0x12345678});
    ib.store(kIb);
    // Index buffer: 3 x u16 (k8in16 swap), a triangle.
    wr_phys(kIndices, 0x00000001);
    wr_phys(kIndices + 4, 0x00020000);

    Pm4 p;
    p.type3(0x48 /*ME_INIT*/, {0x000003FF, 0x00000000});
    p.type0(0x4000, {0x3F800000, 0x40000000, 0x40400000});  // 3 ALU constant registers
    p.type3(0x10 /*NOP*/, {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF});
    p.type3(0x3D /*MEM_WRITE*/, {kMemWrite | 2 /*k8in32*/, 0x11223344, 0x55667788});
    p.type3(0x3F /*INDIRECT_BUFFER*/, {kIb, uint32_t(ib.w.size())});
    p.type3(0x58 /*EVENT_WRITE_SHD*/, {0x00000000, kFence | 2, 0x600DF00D});
    p.type3(0x54 /*INTERRUPT*/, {0x4});  // cpu 2
    // Shaders (synthetic, not a game's): a vertex shader word stream loaded
    // through IM_LOAD_IMMEDIATE, as D3D does before a draw.
    p.type3v(0x2B /*IM_LOAD_IMMEDIATE*/, {0 /*vertex*/, 3 /*start 0, 3 dwords*/, 0x00001000, 0x10000000, 0x00000000});
    // Indexed triangle list from the index buffer (DMA source, 16-bit).
    const uint32_t initiator = uint32_t(xenos::PrimitiveType::kTriangleList) | (0u << 6) /*DMA*/ | (3u << 16);
    p.type3(0x22 /*DRAW_INDX*/, {0 /*viz query*/, initiator, kIndices, (1u << 30) /*k8in16*/ | 3});
    p.store(kRing);

    const bool done = kick_and_wait(uint32_t(p.w.size()));
    check("xenos_cp/ring_consumed_rptr_writeback", done);
    if (!done)
        fprintf(g_out, "diag: wptr=%zu rptr_wb=%u reg_wptr=%u 0x4000=%08X\n", p.w.size(), rd_phys(kRptrWb),
                gpu_host()->register_file()->values[kRegCpRbWptr], gpu_host()->register_file()->values[0x4000]);
    auto* rf = gpu_host()->register_file();
    check("xenos_cp/type0_register_writes",
          rf->values[0x4000] == 0x3F800000 && rf->values[0x4001] == 0x40000000 && rf->values[0x4002] == 0x40400000);
    check("xenos_cp/mem_write_k8in32", rd_phys(kMemWrite) == 0x11223344 && rd_phys(kMemWrite + 4) == 0x55667788);
    check("xenos_cp/indirect_buffer", rf->values[0x4010] == 0xCAFEF00D && rf->values[0x4011] == 0x12345678);
    check("xenos_cp/event_write_shd_fence", rd_phys(kFence) == 0x600DF00D);
    check("xenos_cp/interrupt_to_title_callback", g_interrupts.load() == 1 && g_last_source == 1 &&
                                                      g_last_cpu == 2 && g_last_cb == 0x82001234 &&
                                                      g_last_user == 0xABCD);
    {
        std::lock_guard<std::mutex> lk(g_backend->mu);
        check("xenos_cp/im_load_immediate_vertex_shader",
              g_backend->shaders.size() == 1 && g_backend->shaders[0]->type() == xenos::ShaderType::kVertex &&
                  g_backend->shaders[0]->ucode_data().size() == 3 &&
                  g_backend->shaders[0]->ucode_dwords()[0] == 0x00001000);
        const bool d = g_backend->draws.size() == 1 &&
                       g_backend->draws[0].prim == xenos::PrimitiveType::kTriangleList &&
                       g_backend->draws[0].index_count == 3 && g_backend->draws[0].indexed &&
                       g_backend->draws[0].index_base == kIndices && g_backend->draws[0].index_count_in_buffer == 3;
        check("xenos_cp/draw_indx_dma_triangle", d);
    }

    // Wrap-around: a second batch continuing from the first write pointer.
    Pm4 p2;
    p2.type0(0x4003, {0x77777777});
    const uint32_t start = uint32_t(p.w.size());
    for (size_t i = 0; i < p2.w.size(); ++i) wr_phys(kRing + 4 * uint32_t(start + i), p2.w[i]);
    check("xenos_cp/second_kick", kick_and_wait(start + uint32_t(p2.w.size())) && rf->values[0x4003] == 0x77777777);

    gpu_stop();
    check("xenos_cp/stop", !gpu_running());

    // Unknown opcode: fatal, in a child process (host only: a PS5 title
    // cannot start a child process).
#ifndef RCOMP_HARNESS_LIBRARY
    if (self_path) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            execl("/proc/self/exe", self_path, "--unknown-opcode-child", (char*)nullptr);
            _exit(4);
        }
        int ws = 0;
        waitpid(pid, &ws, 0);
        check("xenos_cp/unknown_opcode_is_fatal", WIFEXITED(ws) && WEXITSTATUS(ws) == 70,
              "child did not exit with RCOMP-FATAL code 70");
    } else
#endif
    {
        fprintf(g_out, "%-52s NOT TESTED (no child process in a title; host run covers it)\n",
                "xenos_cp/unknown_opcode_is_fatal");
    }

    // vblank timer: interrupts with source 0 at the configured rate.
    {
        HostConfig vb;
        vb.vblank_hz = 200;
        g_interrupts = 0;
        const bool started = gpu_start(g_mem, vb, dispatcher, recording_factory());
        gpu_set_interrupt_callback(0x82005678, 0x1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const uint32_t n = gpu_host() ? gpu_host()->vblank_count() : 0;
        const bool src0 = g_last_source.load() == 0 && g_last_cb.load() == 0x82005678;
        gpu_stop();
        // ~50 vblanks expected in 250 ms at 200 Hz; allow scheduling slack.
        check("xenos_cp/vblank_interrupts", started && n >= 30 && n <= 60 && src0 && g_interrupts.load() > 0);
    }
    fprintf(g_out, "RCOMP-XENOS-CP pass=%d fail=%d\n", g_pass, g_fail);
    fflush(g_out);
    return g_fail ? 1 : 0;
}

#ifdef RCOMP_HARNESS_LIBRARY
// PS5 test title entry.
extern "C" int rcomp_xenos_cp_selftest(FILE* out) {
    g_out = out;
    return run_all(nullptr);
}
#else
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--unknown-opcode-child")) return run_unknown_opcode_child();
    return run_all(argv[0]);
}
#endif
