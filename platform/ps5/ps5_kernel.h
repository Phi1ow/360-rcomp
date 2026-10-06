// PS5 kernel memory primitives used by platform/ps5/os_vm_ps5.cpp (owner: Agent 2).
//
// The ps5-payload-sdk v0.42 exports these names from its libkernel stubs
// (target/lib/libkernel{,_web,_sys}.so) but declares none of them in a header,
// so the prototypes are written here. Sources for each signature, in order of
// weight: calls that ran on a console in PS5_RetroArch@18dc105 (src/overflow_heap.c,
// patches/dolphin/ps5-port.patch) and PS5_Vulkan@9639c41 (driver/ps5vk_direct_memory.c);
// the long-standing PS4 prototypes of the same NIDs. Only the prototypes are
// taken (facts about an ABI), no code. Everything not marked "console-observed"
// is an assumption; see platform/ps5/README.md.
//
// Return convention (console-observed): 0 on success, otherwise an SCE error
// 0x8002xxxx whose low 16 bits are the FreeBSD errno (0x8002000C = ENOMEM).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reserve [*addr, *addr + len) with no backing and no access. flags 0 treats
// *addr as a hint that is refused (not relocated) when it cannot be honoured;
// flags kPs5MapFixed places it exactly, replacing what is there
// (console-observed in the Dolphin port, including 4 GiB, and 64 GiB -> ENOMEM).
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);

int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                      size_t alignment, int memory_type, int64_t* phys_out);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                 int64_t direct_start, size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t start, size_t len);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMprotect(const void* addr, size_t len, int prot);

int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelConfiguredFlexibleMemorySize(size_t* size);
int32_t sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                           size_t alignment, int64_t* phys_out, size_t* size_out);

// Sleep the calling thread (microseconds). Used to park a title that has
// finished: see rcomp_title_park() in title/main.cpp.
int32_t sceKernelUsleep(uint32_t usec);

#ifdef __cplusplus
}
#endif

namespace rcomp::ps5 {

// CPU protection bits (same values as FreeBSD PROT_READ/PROT_WRITE).
// GPU bits (0x10/0x20) are deliberately never requested for guest memory.
constexpr int kProtCpuRead = 0x01;
constexpr int kProtCpuWrite = 0x02;

// FreeBSD MAP_FIXED; the value the Dolphin port passes to both
// sceKernelReserveVirtualRange and sceKernelMapDirectMemory.
constexpr int kMapFixed = 0x10;

// Direct-memory type used for CPU read/write heaps by both reference projects
// (PS5_RetroArch overflow_heap.c "CPU read and write only"; PS5_Vulkan
// PS5VK_DIRECT_MEMORY_TYPE). Console-observed to work; semantics undocumented.
constexpr int kDirectMemoryTypeCpuRw = 12;

// GPU-visible window of PS5_Vulkan: its shader compiler combines 32-bit
// pointers with high word 2, so every mapping the driver makes must lie in
// [0x2_0000_0000, 0x3_0000_0000) (PS5_Vulkan driver/ps5vk_private.h). Guest
// memory must never occupy it.
constexpr uint64_t kGpuWindowBegin = 0x200000000ull;
constexpr uint64_t kGpuWindowEnd = 0x300000000ull;

}  // namespace rcomp::ps5
