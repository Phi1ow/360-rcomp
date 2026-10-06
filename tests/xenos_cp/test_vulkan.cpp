// Xenos GPU with its real Vulkan backend (rexglue's VulkanCommandProcessor,
// gpu/xenos/rexglue/rcomp/vulkan_output.h) on the host Vulkan device
// (lavapipe in CI). The guest side is driven exactly like a title: PM4 in a
// ring buffer in guest physical memory, write pointer stored in the register
// window. The presented frame is read back from the presenter.
//
// Host evidence only: this proves nothing about PS5_Vulkan.
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../../gpu/xenos/rexglue/rcomp/vulkan_output.h"
#include "rcomp/guest_memory.h"

using namespace rcomp::xenos;

namespace {

int g_fail = 0, g_pass = 0;
FILE* g_out = stdout;
void check(const char* id, bool ok, const std::string& why = "") {
    fprintf(g_out, "%-52s %s%s%s\n", id, ok ? "PASS" : "FAIL", ok ? "" : " ", ok ? "" : why.c_str());
    fflush(g_out);
    g_fail += !ok;
    g_pass += ok;
}

rcomp::GuestMemory g_mem;
uint8_t* g_base;

constexpr uint32_t kRing = 0x00010000, kRingLog2 = 13, kRptrWb = 0x00001000;
constexpr uint32_t kFront = 0x00100000;  // front buffer (physical), 4 KiB aligned
constexpr uint32_t kW = 256, kH = 128;

uint8_t* phys(uint32_t p) { return g_base + kXenosPhysicalWindow + p; }
void wr_phys(uint32_t p, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(phys(p), &v, 4);
}
uint32_t rd_phys(uint32_t p) {
    uint32_t v;
    memcpy(&v, phys(p), 4);
    return __builtin_bswap32(v);
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
    void store(uint32_t at) const {
        for (size_t i = 0; i < w.size(); ++i) wr_phys(at + 4 * uint32_t(i), w[i]);
    }
};

bool kick_and_wait(uint32_t wptr_dwords) {
    const uint32_t v = __builtin_bswap32(wptr_dwords);
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + kXenosRegisterWindow + 4 * kRegCpRbWptr), v, __ATOMIC_RELEASE);
    for (int i = 0; i < 20000; ++i) {
        if (rd_phys(kRptrWb) == wptr_dwords) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Front buffer texel (x, y) of frame `f`: grey level, alpha 0xFF.
uint8_t level(uint32_t x, uint32_t y, uint32_t f = 0) {
    const uint8_t v = uint8_t((x * 2 + y * 3) & 0xFF);
    return f ? uint8_t(255 - v) : v;
}

void write_frame(uint32_t f) {
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t v = level(x, y, f);
            // D3DFMT_A8R8G8B8 as the CPU stores it: big-endian 0xAARRGGBB.
            wr_phys(kFront + 4 * (y * kW + x), 0xFF000000u | (uint32_t(v) << 16) | (uint32_t(v) << 8) | v);
        }
}

// Compares the presenter's last frame with frame `f`; "" if equal.
std::string compare_frame(const VulkanOutput& out, uint32_t f) {
    std::vector<uint8_t> img;
    uint32_t w = 0, h = 0;
    if (!out.Capture(&img, &w, &h)) return "capture failed";
    if (w != kW || h != kH) return "size " + std::to_string(w) + "x" + std::to_string(h);
    uint32_t bad = 0, first = UINT32_MAX;
    for (uint32_t i = 0; i < kW * kH; ++i) {
        const int v = level(i % kW, i / kW, f);
        const uint8_t* px = &img[4 * i];
        // The swap applies the (identity) gamma ramp: allow 1 LSB.
        if (abs(px[0] - v) > 1 || abs(px[1] - v) > 1 || abs(px[2] - v) > 1) {
            if (first == UINT32_MAX) first = i;
            ++bad;
        }
    }
    if (!bad) return "";
    const uint8_t* px = &img[4 * first];
    char why[160];
    snprintf(why, sizeof why, "bad=%u first=(%u,%u) got=%u,%u,%u want=%u", bad, first % kW, first / kW, px[0], px[1],
             px[2], level(first % kW, first / kW, f));
    return why;
}

// The fetch constant VdSwap writes (hle_xboxkrnl_video.cpp): linear
// k_8_8_8_8, 8-in-32 endian swap, identity swizzle, 2D.
void swap_packet(Pm4& p, uint32_t front = kFront, bool tiled = false) {
    const uint32_t fetch0 = 2u /*texture*/ | ((kW / 32) << 22) /*pitch*/ | (tiled ? 1u << 31 : 0u);
    const uint32_t fetch1 = 6u /*k_8_8_8_8*/ | (2u << 6) /*k8in32*/ | front;
    const uint32_t fetch2 = (kW - 1) | ((kH - 1) << 13);
    const uint32_t fetch3 = (0u | (1u << 3) | (2u << 6) | (3u << 9)) << 1;  // swizzle XYZW
    const uint32_t fetch5 = 1u << 9;                                       // k2DOrStacked
    p.type0(0x4800, {fetch0, fetch1, fetch2, fetch3, 0, fetch5});
    p.type3(0x64 /*XE_SWAP*/, {0x53574150 /*'SWAP'*/, front, kW, kH});
}

// Render-backend state shared by the draws below: one 32bpp colour render
// target at EDRAM tile 0, pitch kW, full-surface scissors, no window offset.
constexpr uint32_t kResolved = 0x00200000;  // resolve destination (tiled)
constexpr uint32_t kVerts = 0x00300000;     // vertex data
void surface_state(Pm4& p) {
    p.type0(0x2000, {kW});                     // RB_SURFACE_INFO: pitch, 1x MSAA
    p.type0(0x2001, {0});                      // RB_COLOR_INFO: base 0, k_8_8_8_8
    p.type0(0x2080, {0});                      // PA_SC_WINDOW_OFFSET
    p.type0(0x2081, {0x80000000u, kW | (kH << 16)});  // WINDOW_SCISSOR_TL (offset off), BR
    p.type0(0x200E, {0, kW | (kH << 16)});     // SCREEN_SCISSOR_TL, BR
    p.type0(0x2104, {0xF});                    // RB_COLOR_MASK
}
// Direct3D 9's resolve: RB_MODECONTROL = copy, a 3-vertex rectangle list
// whose vertices (vertex fetch constant 0) give the rectangle.
void resolve(Pm4& p, bool clear, uint32_t clear_color) {
    p.type0(0x2208, {6});                                       // RB_MODECONTROL: kCopy
    p.type0(0x2318, {clear ? 1u << 8 : 0u});                    // RB_COPY_CONTROL: colour 0, raw
    p.type0(0x2319, {kResolved, kW | (kH << 16), (6u << 7) | 2u});  // DEST_BASE, PITCH, INFO (k_8_8_8_8, 8in32)
    p.type0(0x231E, {clear_color});                             // RB_COLOR_CLEAR
    p.type0(0x4800, {kVerts | 3u /*vertex*/, 2u /*8in32*/ | (6u << 2) /*6 dwords*/});
    p.type3(0x22 /*DRAW_INDX*/, {0, 8u /*rectangle list*/ | (2u << 6) /*auto index*/ | (3u << 16)});
}
// ---- Xenos microcode (encodings of rexglue's format/ucode.h) -------------
uint32_t cf_exec_end(uint32_t address, uint32_t count, uint32_t sequence) {
    return address | (count << 12) | (sequence << 16);  // low 32 bits; high word below
}
// One control-flow slot: EXEC_END (opcode 2) + NOP, packed in 3 dwords.
void cf_slot(std::vector<uint32_t>& u, uint32_t address, uint32_t count, uint32_t sequence) {
    const uint32_t a0 = cf_exec_end(address, count, sequence), a1 = 2u << 12 /*opcode EXEC_END*/;
    const uint32_t b0 = 0, b1 = 0;  // NOP
    u.push_back(a0);
    u.push_back(a1 | ((b0 & 0xFFFF) << 16));
    u.push_back((b0 >> 16) | (b1 << 16));
}
// vfetch r<dst>.xyzw, r0.x, vf0: k_32_32_32_32_FLOAT, stride 4 dwords.
void vfetch_float4(std::vector<uint32_t>& u, uint32_t dst) {
    u.push_back(0u /*vfetch*/ | (0u << 5) /*src r0*/ | (dst << 12) | (1u << 19) /*must be one*/);
    u.push_back((0u | (1u << 3) | (2u << 6) | (3u << 9)) | (38u << 16) /*format*/);
    u.push_back(4u /*stride*/);
}
// vector MAX dst, src, src with export: copies src (register or constant).
void alu_export_copy(std::vector<uint32_t>& u, uint32_t export_dst, uint32_t src, bool src_is_temp) {
    u.push_back(export_dst | (1u << 15) /*export*/ | (0xFu << 16) /*xyzw*/ | (50u << 26) /*scalar retain*/);
    u.push_back(0);  // identity swizzles, no negation
    const uint32_t sel = src_is_temp ? 1u : 0u;
    u.push_back((src << 8) | (src << 16) | (2u << 24) /*MAX*/ | (sel << 30) | (sel << 31));
}

// vfetch r<dst>.xyzw, r0.x, vf0 with a dword offset inside a vertex of
// `stride` dwords (k_32_32_32_32_FLOAT).
void vfetch_float4_at(std::vector<uint32_t>& u, uint32_t dst, uint32_t stride, uint32_t offset) {
    u.push_back(0u | (0u << 5) | (dst << 12) | (1u << 19));
    u.push_back((0u | (1u << 3) | (2u << 6) | (3u << 9)) | (38u << 16));
    u.push_back(stride | (offset << 8));
}
// tfetch2D r<dst>, r<src>.xy, tf<const>: point sampling, base map only.
void tfetch2d(std::vector<uint32_t>& u, uint32_t dst, uint32_t src, uint32_t tf) {
    u.push_back(1u /*tfetch*/ | (src << 5) | (dst << 12) | (tf << 20) | ((0u | (1u << 2)) << 26) /*src .xy*/);
    u.push_back((0u | (1u << 3) | (2u << 6) | (3u << 9)) | (0u << 12) /*mag point*/ | (0u << 14) /*min point*/ |
                (2u << 16) /*mip base map*/ | (1u << 28) /*computed lod*/);
    u.push_back((1u << 1) /*sample at centre*/ | (1u << 14) /*2D*/);
}
float fbits(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
uint32_t ubits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
void load_shader(Pm4& p, uint32_t type, const std::vector<uint32_t>& code) {
    p.w.push_back((3u << 30) | (uint32_t(code.size() + 2 - 1) << 16) | (0x2Bu << 8));  // IM_LOAD_IMMEDIATE
    p.w.push_back(type);
    p.w.push_back(uint32_t(code.size()));
    p.w.insert(p.w.end(), code.begin(), code.end());
}
// Fixed-function state of the draws below (as step 4): no blending, no
// culling, viewport mapping clip space to the kW x kH target, z in [0, 1].
void draw_state(Pm4& p) {
    surface_state(p);
    p.type0(0x2208, {4});
    p.type0(0x2201, {0x00010001u});
    p.type0(0x2202, {0});
    p.type0(0x2204, {0});
    p.type0(0x2205, {0});
    p.type0(0x2206, {0x43F});
    p.type0(0x210F, {ubits(kW / 2.0f), ubits(kW / 2.0f), ubits(-(kH / 2.0f)), ubits(kH / 2.0f), ubits(1), ubits(0)});
    p.type0(0x2312, {0xFFFF});
    p.type0(0x2100, {0x00FFFFFF});
}
void write_floats(uint32_t at, std::initializer_list<float> v) {
    uint32_t i = 0;
    for (float f : v) wr_phys(at + 4 * i++, ubits(f));
}
// Full-screen quad as a 6-vertex triangle list at depth z (float4 positions).
void write_quad(uint32_t at, float z) {
    write_floats(at, {-1, 1, z, 1, 1, 1, z, 1, -1, -1, z, 1, 1, 1, z, 1, 1, -1, z, 1, -1, -1, z, 1});
}

// Ring write position (dwords) and submission of the next packets.
uint32_t g_wptr = 0;
bool submit(const Pm4& p) {
    p.store(kRing + 4 * g_wptr);
    g_wptr += uint32_t(p.w.size());
    return kick_and_wait(g_wptr);
}

void write_resolve_rect() {
    const float v[6] = {0.0f, 0.0f, float(kW), 0.0f, 0.0f, float(kH)};
    for (int i = 0; i < 6; ++i) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        wr_phys(kVerts + 4 * i, u);
    }
}

int run_all() {
    if (g_mem.reserve() != rcomp::MemStatus::Ok) return 2;
    g_base = g_mem.base();
    if (g_mem.commit(kXenosPhysicalWindow, 0x400000, rcomp::Protect::ReadWrite) != rcomp::MemStatus::Ok) return 2;

    std::string err;
    // With a display when the instance offers a surface without a window
    // system (PS5: display plane; host: headless); otherwise headless only
    // and the on-screen step is NOT TESTED.
    std::unique_ptr<VulkanOutput> out = VulkanOutput::Create(/*with_display=*/true, &err);
    const bool with_display = out != nullptr;
    if (!out) {
        fprintf(g_out, "xenos_vk display output unavailable (%s); rendering checks only\n", err.c_str());
        out = VulkanOutput::Create(/*with_display=*/false, &err);
    }
    if (!out) {
        fprintf(g_out, "xenos_vk/device                                      BLOCKED %s\n", err.c_str());
        fprintf(g_out, "RCOMP-XENOS-VK blocked\n");
        return 3;
    }
    fprintf(g_out, "xenos_vk device: %s\n", out->device_name().c_str());
    HostConfig cfg;
    cfg.vblank_hz = 0;
    cfg.display_width = kW;
    cfg.display_height = kH;
    out->Attach(cfg);
    const bool started = gpu_start(g_mem, cfg, nullptr, out->backend_factory());
    check("xenos_vk/start", started);
    if (!started) return 1;
    gpu_initialize_ring_buffer(kRing, kRingLog2);
    gpu_enable_read_pointer_writeback(kRptrWb, 2);

    // 1. A frame the CPU wrote into physical memory reaches the display.
    write_frame(0);
    Pm4 p;
    p.type3(0x48 /*ME_INIT*/, {0x000003FF, 0x00000000});
    swap_packet(p);
    p.store(kRing);
    check("xenos_vk/swap_consumed", kick_and_wait(uint32_t(p.w.size())));
    std::string why = compare_frame(*out, 0);
    check("xenos_vk/cpu_frame_presented", why.empty(), why);

    // 2. The CPU rewrites the same front buffer, then swaps again: the new
    // contents must be shown (GPU caches invalidated at the kick, without
    // write-protection faults).
    write_frame(1);
    Pm4 p2;
    swap_packet(p2);
    p2.store(kRing + 4 * uint32_t(p.w.size()));
    check("xenos_vk/second_swap_consumed", kick_and_wait(uint32_t(p.w.size() + p2.w.size())));
    why = compare_frame(*out, 1);
    check("xenos_vk/cpu_rewrite_seen_by_next_swap", why.empty(), why);

    // 3. EDRAM: a resolve with clear fills the render target with a colour,
    // a second resolve copies it to memory (tiled), the swap presents it.
    write_resolve_rect();
    Pm4 p3;
    surface_state(p3);
    resolve(p3, true, 0xFFC0C0C0u);
    resolve(p3, false, 0);
    swap_packet(p3, kResolved, /*tiled=*/true);
    const uint32_t at3 = uint32_t(p.w.size() + p2.w.size());
    p3.store(kRing + 4 * at3);
    check("xenos_vk/resolve_consumed", kick_and_wait(at3 + uint32_t(p3.w.size())));
    {
        std::vector<uint8_t> img;
        uint32_t w = 0, h = 0, bad = 0;
        const bool ok = out->Capture(&img, &w, &h) && w == kW && h == kH;
        for (uint32_t i = 0; ok && i < kW * kH; ++i)
            if (abs(img[4 * i] - 0xC0) > 1 || abs(img[4 * i + 1] - 0xC0) > 1 || abs(img[4 * i + 2] - 0xC0) > 1) ++bad;
        char why3[96];
        snprintf(why3, sizeof why3, "captured=%d bad=%u px0=%u,%u,%u", ok, bad, ok ? img[0] : 0, ok ? img[1] : 0,
                 ok ? img[2] : 0);
        check("xenos_vk/edram_clear_resolved_and_presented", ok && bad == 0, why3);
    }

    // 4. A triangle drawn by translated Xenos shaders: the vertex shader
    // fetches a float4 clip-space position and exports it, the pixel shader
    // exports constant c0. Rendered into the EDRAM (still holding the grey
    // clear of step 3), resolved, presented.
    {
        std::vector<uint32_t> vs, ps;
        cf_slot(vs, 1, 2, 0b0001);  // fetch, then ALU
        vfetch_float4(vs, 0);
        alu_export_copy(vs, 62 /*oPos*/, 0, true);
        cf_slot(ps, 1, 1, 0);
        alu_export_copy(ps, 0 /*oC0*/, 0 /*c0*/, false);
        constexpr uint32_t kTri = 0x00310000;
        const float tri[12] = {-1, 1, 0, 1, 1, 1, 0, 1, -1, -1, 0, 1};  // TL, TR, BL
        for (int i = 0; i < 12; ++i) {
            uint32_t u;
            memcpy(&u, &tri[i], 4);
            wr_phys(kTri + 4 * i, u);
        }
        auto f = [](float v) {
            uint32_t u;
            memcpy(&u, &v, 4);
            return u;
        };
        Pm4 p4;
        auto load = [&](uint32_t type, const std::vector<uint32_t>& code) {
            p4.w.push_back((3u << 30) | (uint32_t(code.size() + 2 - 1) << 16) | (0x2Bu << 8));  // IM_LOAD_IMMEDIATE
            p4.w.push_back(type);
            p4.w.push_back(uint32_t(code.size()));  // start 0, size in dwords
            p4.w.insert(p4.w.end(), code.begin(), code.end());
        };
        load(0, vs);
        load(1, ps);
        surface_state(p4);
        p4.type0(0x2208, {4});                            // RB_MODECONTROL: colour + depth
        p4.type0(0x2200, {0});                            // RB_DEPTHCONTROL: off
        p4.type0(0x2201, {0x00010001u});                  // RB_BLENDCONTROL0: ONE, ZERO
        p4.type0(0x2202, {0});                            // RB_COLORCONTROL: no alpha test
        p4.type0(0x2204, {0});                            // PA_CL_CLIP_CNTL
        p4.type0(0x2205, {0});                            // PA_SU_SC_MODE_CNTL: no cull, solid
        p4.type0(0x2206, {0x43F});                        // PA_CL_VTE_CNTL: viewport, w0
        p4.type0(0x210F, {f(kW / 2.0f), f(kW / 2.0f), f(-(kH / 2.0f)), f(kH / 2.0f), f(1), f(0)});
        p4.type0(0x2312, {0xFFFF});                       // PA_SC_AA_MASK
        p4.type0(0x2100, {0x00FFFFFF});                   // VGT_MAX_VTX_INDX
        p4.type0(0x2180, {0});                            // SQ_PROGRAM_CNTL: 1 register each
        p4.type0(0x4400, {f(1.0f), f(0.25f), f(0.0f), f(1.0f)});  // PS c0 = (1, 0.25, 0, 1)
        p4.type0(0x4800, {kTri | 3u, 2u | (12u << 2)});   // vertex fetch 0: 12 dwords, 8in32
        p4.type3(0x22 /*DRAW_INDX*/, {0, 4u /*triangle list*/ | (2u << 6) /*auto*/ | (3u << 16)});
        resolve(p4, false, 0);
        swap_packet(p4, kResolved, /*tiled=*/true);
        const uint32_t at4 = at3 + uint32_t(p3.w.size());
        p4.store(kRing + 4 * at4);
        check("xenos_vk/draw_consumed", kick_and_wait(at4 + uint32_t(p4.w.size())));
        std::vector<uint8_t> img;
        uint32_t w = 0, h = 0, in_bad = 0, out_bad = 0, in_n = 0, out_n = 0;
        const bool ok = out->Capture(&img, &w, &h) && w == kW && h == kH;
        for (uint32_t y = 0; ok && y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                // Inside the TL-TR-BL half: (x+.5)/W + (y+.5)/H < 1; skip a
                // 2-pixel band around the diagonal edge.
                const float d = (x + 0.5f) / kW + (y + 0.5f) / kH - 1.0f;
                if (d > -2.0f / kH && d < 2.0f / kH) continue;
                const uint8_t* px = &img[4 * (y * kW + x)];
                if (d < 0) {
                    ++in_n;
                    if (abs(px[0] - 255) > 1 || abs(px[1] - 64) > 2 || px[2] > 1) ++in_bad;
                } else {
                    ++out_n;
                    if (abs(px[0] - 0xC0) > 1 || abs(px[1] - 0xC0) > 1 || abs(px[2] - 0xC0) > 1) ++out_bad;
                }
            }
        char why4[160];
        snprintf(why4, sizeof why4, "captured=%d inside %u/%u bad, outside %u/%u bad, px(4,4)=%u,%u,%u", ok, in_bad,
                 in_n, out_bad, out_n, ok ? img[4 * (4 * kW + 4)] : 0, ok ? img[4 * (4 * kW + 4) + 1] : 0,
                 ok ? img[4 * (4 * kW + 4) + 2] : 0);
        check("xenos_vk/shader_triangle_drawn_resolved_presented", ok && in_n && out_n && !in_bad && !out_bad, why4);
        g_wptr = at4 + uint32_t(p4.w.size());
    }

    // 5. Sampled texture: a full-screen quad whose pixel shader samples a
    // texture the CPU wrote (linear k_8_8_8_8, same size as the target,
    // point sampling at texel centres), so pixel (x, y) must show texel (x, y).
    {
        constexpr uint32_t kTex = 0x00320000, kQuad = 0x00318000;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x)  // A=FF R=x G=2y B=0x40, CPU order 0xAARRGGBB
                wr_phys(kTex + 4 * (y * kW + x), 0xFF000040u | (x << 16) | (((2 * y) & 0xFF) << 8));
        // Vertices: float4 position, float4 texcoord (stride 8 dwords).
        write_floats(kQuad, {-1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, -1, -1, 0, 1, 0, 1, 0, 0,
                             1, 1, 0, 1, 1, 0, 0, 0, 1, -1, 0, 1, 1, 1, 0, 0, -1, -1, 0, 1, 0, 1, 0, 0});
        std::vector<uint32_t> vs, ps;
        cf_slot(vs, 1, 4, 0b00000101);  // fetch, fetch, ALU, ALU
        vfetch_float4_at(vs, 1, 8, 0);
        vfetch_float4_at(vs, 2, 8, 4);
        alu_export_copy(vs, 62 /*oPos*/, 1, true);
        alu_export_copy(vs, 0 /*interpolator 0*/, 2, true);
        cf_slot(ps, 1, 2, 0b0001);  // fetch, ALU
        tfetch2d(ps, 1, 0, 1);
        alu_export_copy(ps, 0 /*oC0*/, 1, true);
        Pm4 p5;
        load_shader(p5, 0, vs);
        load_shader(p5, 1, ps);
        draw_state(p5);
        p5.type0(0x2200, {0});                          // no depth
        p5.type0(0x2180, {2u | (1u << 8)});             // SQ_PROGRAM_CNTL: VS 3 regs, PS 2 regs, 1 interpolator
        p5.type0(0x4800, {kQuad | 3u, 2u | (48u << 2)});  // vertex fetch 0: 48 dwords, 8in32
        // Texture fetch 1 (fetch slot 1, 6 dwords): linear k_8_8_8_8, 8in32, 2D,
        // swizzle ZYXW as Direct3D sets it for A8R8G8B8 (the 8in32 word's
        // low byte is B; with an identity swizzle the shader's .x would be B).
        p5.type0(0x4806, {2u | ((kW / 32) << 22), 6u | (2u << 6) | kTex, (kW - 1) | ((kH - 1) << 13),
                          (2u | (1u << 3) | (0u << 6) | (3u << 9)) << 1, 0, 1u << 9});
        p5.type3(0x22, {0, 4u | (2u << 6) | (6u << 16)});
        resolve(p5, false, 0);
        swap_packet(p5, kResolved, true);
        check("xenos_vk/textured_draw_consumed", submit(p5));
        std::vector<uint8_t> img;
        uint32_t w = 0, h = 0, bad = 0, first = UINT32_MAX;
        const bool ok = out->Capture(&img, &w, &h) && w == kW && h == kH;
        for (uint32_t i = 0; ok && i < kW * kH; ++i) {
            const uint32_t x = i % kW, y = i / kW;
            const uint8_t* px = &img[4 * i];
            if (abs(px[0] - int(x)) > 1 || abs(px[1] - int((2 * y) & 0xFF)) > 1 || abs(px[2] - 0x40) > 1) {
                if (first == UINT32_MAX) first = i;
                ++bad;
            }
        }
        char why5[160];
        snprintf(why5, sizeof why5, "captured=%d bad=%u first=(%d,%d) got=%u,%u,%u", ok, bad,
                 first == UINT32_MAX ? -1 : int(first % kW), first == UINT32_MAX ? -1 : int(first / kW),
                 first == UINT32_MAX ? 0 : img[4 * first], first == UINT32_MAX ? 0 : img[4 * first + 1],
                 first == UINT32_MAX ? 0 : img[4 * first + 2]);
        check("xenos_vk/texture_sampled_per_texel", ok && bad == 0, why5);
    }

    // 6. Depth (D24S8 at EDRAM tile 64): quad B at z=0.8 drawn with ALWAYS
    // (writes depth), triangle A at z=0.2 with LESS, then quad C at z=0.5 with
    // LESS. C must replace B everywhere except under A (0.5 > 0.2).
    {
        constexpr uint32_t kQb = 0x00340000, kTa = 0x00340200, kQc = 0x00340400;
        write_quad(kQb, 0.8f);
        write_floats(kTa, {-1, 1, 0.2f, 1, 1, 1, 0.2f, 1, -1, -1, 0.2f, 1});  // TL, TR, BL
        write_quad(kQc, 0.5f);
        std::vector<uint32_t> vs, ps;
        cf_slot(vs, 1, 2, 0b0001);
        vfetch_float4(vs, 0);
        alu_export_copy(vs, 62, 0, true);
        cf_slot(ps, 1, 1, 0);
        alu_export_copy(ps, 0, 0, false);
        Pm4 p6;
        load_shader(p6, 0, vs);
        load_shader(p6, 1, ps);
        draw_state(p6);
        p6.type0(0x2002, {64});  // RB_DEPTH_INFO: base tile 64, k_24_8
        p6.type0(0x2180, {0});
        auto draw = [&](uint32_t verts, uint32_t count, uint32_t zfunc, float r, float g, float b) {
            p6.type0(0x2200, {(1u << 1) | (1u << 2) | (zfunc << 4)});  // z enable + write, zfunc
            p6.type0(0x4400, {ubits(r), ubits(g), ubits(b), ubits(1)});
            p6.type0(0x4800, {verts | 3u, 2u | ((count * 4) << 2)});
            p6.type3(0x22, {0, 4u | (2u << 6) | (count << 16)});
        };
        draw(kQb, 6, 7 /*ALWAYS*/, 0, 0, 1);
        draw(kTa, 3, 1 /*LESS*/, 1, 0, 0);
        draw(kQc, 6, 1 /*LESS*/, 0, 1, 0);
        p6.type0(0x2200, {0});
        resolve(p6, false, 0);
        swap_packet(p6, kResolved, true);
        check("xenos_vk/depth_draws_consumed", submit(p6));
        std::vector<uint8_t> img;
        uint32_t w = 0, h = 0, in_bad = 0, out_bad = 0, in_n = 0, out_n = 0;
        const bool ok = out->Capture(&img, &w, &h) && w == kW && h == kH;
        for (uint32_t y = 0; ok && y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                const float d = (x + 0.5f) / kW + (y + 0.5f) / kH - 1.0f;
                if (d > -2.0f / kH && d < 2.0f / kH) continue;
                const uint8_t* px = &img[4 * (y * kW + x)];
                if (d < 0) {
                    ++in_n;
                    if (px[0] < 254 || px[1] > 1 || px[2] > 1) ++in_bad;  // A (red) kept
                } else {
                    ++out_n;
                    if (px[0] > 1 || px[1] < 254 || px[2] > 1) ++out_bad;  // C (green) over B
                }
            }
        char why6[160];
        snprintf(why6, sizeof why6, "captured=%d under A %u/%u bad, elsewhere %u/%u bad, px(4,4)=%u,%u,%u "
                 "px(250,120)=%u,%u,%u", ok, in_bad, in_n, out_bad, out_n, ok ? img[4 * (4 * kW + 4)] : 0,
                 ok ? img[4 * (4 * kW + 4) + 1] : 0, ok ? img[4 * (4 * kW + 4) + 2] : 0,
                 ok ? img[4 * (120 * kW + 250)] : 0, ok ? img[4 * (120 * kW + 250) + 1] : 0,
                 ok ? img[4 * (120 * kW + 250) + 2] : 0);
        check("xenos_vk/depth_test_less_hides_farther", ok && in_n && out_n && !in_bad && !out_bad, why6);
    }

    // 7. On screen: the last presented frame (step 6) goes to the display
    // (PS5_Vulkan display plane; host: headless surface) through a
    // swapchain; the swapchain image is read back before presentation.
    if (!with_display) {
        fprintf(g_out, "%-52s NOT TESTED (no surface support: %s)\n", "xenos_vk/frame_on_display", err.c_str());
    } else {
        std::string derr;
        std::unique_ptr<VulkanDisplay> display = out->CreateDisplay(kW, kH, &derr);
        std::vector<uint8_t> frame, shown;
        uint32_t fw = 0, fh = 0;
        bool ok = display && out->Capture(&frame, &fw, &fh);
        std::string why7 = display ? "" : derr;
        if (ok) {
            fprintf(g_out, "xenos_vk display: %s %ux%u\n",
                    display->kind() == VulkanDisplay::Kind::kDisplayPlane ? "display plane" : "headless surface",
                    display->width(), display->height());
            // Keep the frame up for a few seconds on a real screen (FIFO).
            const int presents = display->kind() == VulkanDisplay::Kind::kDisplayPlane ? 300 : 3;
            for (int i = 0; ok && i < presents; ++i)
                ok = display->Present(frame.data(), fw, fh, i == presents - 1 ? &shown : nullptr, &derr);
            if (!ok) why7 = derr;
        }
        if (ok) {
            // Compare frame pixels with the display pixels they map to
            // (letterboxed, scaled): every 4th pixel away from edges when
            // scaled, all pixels when the sizes match.
            const uint32_t W = display->width(), H = display->height();
            uint32_t dw = W, dh = uint32_t(uint64_t(W) * fh / fw);
            if (dh > H) dh = H, dw = uint32_t(uint64_t(H) * fw / fh);
            const uint32_t dx = (W - dw) / 2, dy = (H - dh) / 2;
            const bool exact = dw == fw && dh == fh;
            uint32_t n = 0, bad = 0;
            for (uint32_t y = 0; y < fh; y += exact ? 1 : 4)
                for (uint32_t x = 0; x < fw; x += exact ? 1 : 4) {
                    const float d = (x + 0.5f) / fw + (y + 0.5f) / fh - 1.0f;  // step 6's diagonal edge
                    if (!exact && (x < 2 || y < 2 || x + 2 >= fw || y + 2 >= fh || (d > -4.0f / fh && d < 4.0f / fh)))
                        continue;
                    const uint32_t sx = dx + uint32_t((x + 0.5) * dw / fw), sy = dy + uint32_t((y + 0.5) * dh / fh);
                    const uint8_t* a = &frame[4 * (size_t(y) * fw + x)];
                    const uint8_t* b = &shown[4 * (size_t(sy) * W + sx)];
                    ++n;
                    if (abs(a[0] - b[0]) > 2 || abs(a[1] - b[1]) > 2 || abs(a[2] - b[2]) > 2) ++bad;
                }
            // Letterbox bars (if any) are black.
            if (dx && (shown[0] || shown[1] || shown[2])) ++bad;
            char w7[128];
            snprintf(w7, sizeof w7, "%u/%u compared pixels differ", bad, n);
            ok = n && !bad;
            if (!ok) why7 = w7;
        }
        check("xenos_vk/frame_on_display", ok, why7);
        // Headless host: also the scaled, letterboxed path a TV takes.
        if (ok && display->kind() == VulkanDisplay::Kind::kHeadless) {
            display.reset();
            std::unique_ptr<VulkanDisplay> big = out->CreateDisplay(640, 480, &derr);
            bool ok2 = big && big->Present(frame.data(), fw, fh, &shown, &derr);
            uint32_t n = 0, bad = 0;
            if (ok2) {
                // 256x128 into 640x480: 640x320 at y 80, black bars above and below.
                for (uint32_t y = 2; y + 2 < fh; y += 4)
                    for (uint32_t x = 2; x + 2 < fw; x += 4) {
                        const float d = (x + 0.5f) / fw + (y + 0.5f) / fh - 1.0f;
                        if (d > -4.0f / fh && d < 4.0f / fh) continue;
                        const uint32_t sx = uint32_t((x + 0.5) * 640 / fw), sy = 80 + uint32_t((y + 0.5) * 320 / fh);
                        const uint8_t* a = &frame[4 * (size_t(y) * fw + x)];
                        const uint8_t* b = &shown[4 * (size_t(sy) * 640 + sx)];
                        ++n;
                        if (abs(a[0] - b[0]) > 2 || abs(a[1] - b[1]) > 2 || abs(a[2] - b[2]) > 2) ++bad;
                    }
                for (uint32_t y : {0u, 79u, 400u, 479u})
                    for (uint32_t x : {0u, 320u, 639u}) {
                        const uint8_t* b = &shown[4 * (size_t(y) * 640 + x)];
                        ++n;
                        if (b[0] || b[1] || b[2]) ++bad;
                    }
            }
            char w8[160];
            snprintf(w8, sizeof w8, "%s %u/%u compared pixels differ", ok2 ? "" : derr.c_str(), bad, n);
            check("xenos_vk/frame_scaled_letterboxed_on_display", ok2 && n && !bad, w8);
        }
    }

    gpu_stop();
    fprintf(g_out, "RCOMP-XENOS-VK pass=%d fail=%d (HOST ONLY)\n", g_pass, g_fail);
    fflush(g_out);
    return g_fail ? 1 : 0;
}

}  // namespace

#ifdef RCOMP_HARNESS_LIBRARY
extern "C" int rcomp_xenos_vulkan_selftest(FILE* out) {
    g_out = out;
    return run_all();
}
#else
int main() { return run_all(); }
#endif
