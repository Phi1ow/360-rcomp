// xboxkrnl exports of the 3 October 2026 catalog batch that fit no existing
// subsystem (owner: Agent 3, runtime/): kernel stacks, address validity, I/O
// space mapping, PE headers, stack switching, thread priority queries, the
// display-mode override and the IRP completion entry points.
// Contracts and evidence: runtime/docs/KERNEL_IMPORTS_20261003.md.
//
// Same rules as every HLE file: real behaviour inside the documented subset,
// RCOMP_FATAL_UNIMPLEMENTED (with the arguments) outside it, never a fake
// success. Signatures: XenonRecomp's copy of Xenia's export table for the
// ordinals; Xenia / rexglue-sdk (BSD-3, signatures only, no code copied) and
// community xbdm reverse engineering (copeison/xbdm KernelExports.h) for the
// argument lists, cross-checked with the NT routines of the same name.
#include <math.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>

#include "host_fiber.h"
#include "physical_window.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/guest_thread.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/kernel_variables.h"
#include "rcomp/runtime/runtime.h"
#include "rcomp/runtime/thread_object.h"
#include "rcomp/runtime/virtual_fields.h"

namespace rcomp::rt {

Status register_xboxkrnl_more_hle();

namespace {

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

[[noreturn]] void unimplemented(const char* fn, PPCContext& ctx, const char* what, uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X (not implemented)", fn, what, value,
                (uint32_t)ctx.lr);
}

[[noreturn]] void misuse(const char* fn, PPCContext& ctx, const char* what, uint32_t value) {
    rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s %s=0x%08X lr=0x%08X", fn, what, value, (uint32_t)ctx.lr);
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}

// ---- kernel stacks -----------------------------------------------------------

// Stacks created by MmCreateKernelStack, keyed by the returned stack base (the
// high end): {allocation base (the stack limit), runtime generation}.
struct KernelStack { uint32_t limit; uint64_t generation; };
std::mutex g_stack_mu;
std::map<uint32_t, KernelStack> g_kernel_stacks;

// MmCreateKernelStack (0x00BB): PVOID (ULONG StackSize, ULONG Type). Allocates
// a zeroed, read/write stack of StackSize rounded up to 4 KiB from the
// runtime heap and returns its base (the high end, one past the last byte),
// the value KeSetCurrentStackPointers / MmDeleteKernelStack take. NULL when the
// heap is exhausted (the NT allocator's failure value). Type: only 0 (the
// value Xenia asserts) has an established meaning; others trap. A size of 0 or
// above 16 MiB is not a stack (guest error).
void MmCreateKernelStack(PPCContext& ctx, uint8_t*) {
    const char* fn = "MmCreateKernelStack";
    Runtime& r = rt_or_die(fn);
    const uint32_t size = ctx.r3.u32, type = ctx.r4.u32;
    if (type) unimplemented(fn, ctx, "stack_type", type);
    if (!size || size > 0x01000000u) misuse(fn, ctx, "stack_size", size);
    const uint32_t rounded = (size + 0xFFFu) & ~0xFFFu;
    uint32_t limit = 0;
    if (r.heap.alloc(rounded, 0x10000, true, &limit) != Status::Ok) { ctx.r3.u64 = 0; return; }
    {
        std::lock_guard<std::mutex> lock(g_stack_mu);
        g_kernel_stacks[limit + rounded] = {limit, r.generation};
    }
    ctx.r3.u64 = limit + rounded;
}

// MmDeleteKernelStack (0x00BC): VOID (PVOID StackBase, PVOID StackLimit).
// Frees a stack made by MmCreateKernelStack; StackLimit must be its low end.
// Any other pair is a guest error (a kernel bugcheck on the console). r3 is
// left unchanged (VOID).
void MmDeleteKernelStack(PPCContext& ctx, uint8_t*) {
    const char* fn = "MmDeleteKernelStack";
    Runtime& r = rt_or_die(fn);
    const uint32_t base = ctx.r3.u32, limit = ctx.r4.u32;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(g_stack_mu);
        const auto it = g_kernel_stacks.find(base);
        known = it != g_kernel_stacks.end() && it->second.generation == r.generation && it->second.limit == limit;
        if (known) g_kernel_stacks.erase(it);
    }
    if (!known) misuse(fn, ctx, "unknown_kernel_stack", base);  // no lock held across the fatal path
    if (ctx.r1.u32 >= limit && ctx.r1.u32 <= base) misuse(fn, ctx, "running_on_stack", base);
    // A guest fiber deleted while suspended: its host context goes with it.
    host_fiber_stack_deleted(base, limit);
    const Status status = r.heap.free(limit);
    if (status != Status::Ok)
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s heap free of 0x%08X: %s", fn, limit, status_name(status));
}

// ---- addresses ----------------------------------------------------------------

// MmIsAddressValid (0x00BF): BOOLEAN (PVOID). TRUE when a read of that byte
// would not fault: committed, readable guest memory, or a runtime kernel
// object R-comp serves through its virtual-field arena (a live thread Body or
// a registered kernel variable/type token), which are valid kernel memory on
// the console. Everything else FALSE.
void MmIsAddressValid(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("MmIsAddressValid");
    const uint32_t address = ctx.r3.u32;
    bool valid = r.mem->is_accessible(address, 1, Protect::Read);
    if (!valid && address >= kOpaqueRuntimeArenaBase && address < kOpaqueRuntimeArenaEnd) {
        if (address >= kThreadVirtualTokenBase) {
            const uint32_t body = address - (address - kThreadVirtualTokenBase) % kThreadVirtualTokenStride;
            std::shared_ptr<ThreadObjectIdentity> identity;
            valid = find_thread_object(body, &identity) == Status::Ok;
        } else {
            uint32_t ignored = 0;
            const uint32_t token = address & ~0xFFu;
            valid = (token == kExThreadObjectTypeVirtualAddress && thread_object_type_address()) ||
                    (token == kExEventObjectTypeVirtualAddress && event_object_type_address()) ||
                    (token == kXboxKrnlVersionVirtualAddress && find_variable_import(kModuleXboxkrnl, 0x0158, &ignored)) ||
                    (token == kKeTimeStampBundleVirtualAddress && find_variable_import(kModuleXboxkrnl, 0x00AD, &ignored)) ||
                    (token == kXboxHardwareInfoVirtualAddress && find_variable_import(kModuleXboxkrnl, 0x0156, &ignored)) ||
                    (token == kTitleProcessVirtualAddress && find_variable_import(kModuleXboxkrnl, 0x0158, &ignored));
        }
    }
    ctx.r3.u64 = valid ? 1 : 0;
}

// MmMapIoSpace (0x00C2): PVOID (ULONG Unknown, PHYSICAL_ADDRESS, ULONG Size,
// ULONG Protect). Maps a physical range into the CPU address space. The only
// observed use (Xenia: XMA contexts, arguments 2 / 0x40 / 0x404) maps memory
// that already lies in R-comp's physical window, so the mapping is that
// window address: an address already inside a physical window is returned as
// is (the behaviour Xenia relies on), a physical address P is returned as its
// 0xA0000000-window address, when the whole range is committed. Device
// register ranges (MMIO) are not mapped by R-comp and trap.
void MmMapIoSpace(PPCContext& ctx, uint8_t*) {
    const char* fn = "MmMapIoSpace";
    Runtime& r = rt_or_die(fn);
    const uint32_t address = ctx.r4.u32, size = ctx.r5.u32;
    if (!size) misuse(fn, ctx, "size", size);
    uint32_t mapped = 0;
    if (in_physical_windows(address)) mapped = address;
    else if (address < xenos::kXenosPhysicalSize) mapped = xenos::kXenosPhysicalWindow + address;
    if (!mapped || uint64_t(mapped) + size > 0x100000000ull || !r.mem->is_accessible(mapped, size, Protect::Read))
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!%s physical=0x%08X size=0x%X (args 0x%08X, protect 0x%08X) lr=0x%08X: not committed "
                    "physical memory; device register (MMIO) ranges are not mapped by R-comp",
                    fn, address, size, ctx.r3.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    ctx.r3.u64 = mapped;
}

// MmLockAndMapSegmentArray (0x00C0) / MmUnlockAndUnmapSegmentArray (0x00C9):
// no public source establishes their arguments or result (Xenia/rexglue stub
// them), so any call traps with its argument registers.
void MmLockAndMapSegmentArray(PPCContext& ctx, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!MmLockAndMapSegmentArray (0x%08X 0x%08X 0x%08X 0x%08X) lr=0x%08X: no established contract",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
}
void MmUnlockAndUnmapSegmentArray(PPCContext& ctx, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!MmUnlockAndUnmapSegmentArray (0x%08X 0x%08X 0x%08X 0x%08X) lr=0x%08X: no established contract",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
}

// RtlImageNtHeader (0x01A9): PIMAGE_NT_HEADERS (PVOID ModuleAddress). The NT
// rule (RtlImageNtHeaderEx): NULL for a NULL base, a missing "MZ" signature, an
// e_lfanew of 256 MiB or more, or a missing "PE\0\0" signature; otherwise
// Base + e_lfanew. Headers are little-endian (PE). An unreadable non-NULL base
// is a guest fault, as on the console.
void RtlImageNtHeader(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlImageNtHeader";
    Runtime& r = rt_or_die(fn);
    const uint32_t base = ctx.r3.u32;
    if (!base) { ctx.r3.u64 = 0; return; }
    if (!r.mem->is_accessible(base, 0x40, Protect::Read)) misuse(fn, ctx, "unreadable_image", base);
    const uint8_t* p = r.mem->host(base);
    const uint32_t nt = le32(p + 0x3C);
    if (p[0] != 'M' || p[1] != 'Z' || nt >= 0x10000000u || uint64_t(base) + nt + 4 > 0x100000000ull) {
        ctx.r3.u64 = 0;
        return;
    }
    if (!r.mem->is_accessible(base + nt, 4, Protect::Read)) misuse(fn, ctx, "unreadable_nt_headers", base + nt);
    ctx.r3.u64 = le32(r.mem->host(base + nt)) == 0x00004550u ? base + nt : 0;
}

// ---- threads -------------------------------------------------------------------

// KeSetCurrentStackPointers (0x009B): VOID (PVOID StackPointer, PKTHREAD Thread,
// PVOID StackAllocBase, PVOID StackBase, PVOID StackLimit). Switches the
// calling thread to another stack: r1 = StackPointer, then it returns to LR
// (rexglue: KTHREAD stack fields and PCR stack_base_ptr/stack_end_ptr
// updated). Implemented for the calling thread: the PCR at r13 publishes
// StackBase (+0x70) and StackLimit (+0x74), the KTHREAD +0xD0/+0x5C/+0x60
// fields StackAllocBase/StackBase/StackLimit. The new range must be committed
// read/write memory containing StackPointer. Another thread's Body traps (no
// established use).
// Its user is xapi's SwitchToFiber, which has loaded the target fiber's
// registers and LR before this tail call. With StackPointer == r1 the
// continuation is the caller's own return (switch to the running fiber);
// otherwise the host context of the target fiber takes over
// (src/host_fiber.h, runtime/docs/THREAD_OBJECTS.md ("Guest fibers")) and this call returns when the
// calling fiber is resumed.
void KeSetCurrentStackPointers(PPCContext& ctx, uint8_t* membase) {
    const char* fn = "KeSetCurrentStackPointers";
    Runtime& r = rt_or_die(fn);
    const uint32_t sp = ctx.r3.u32, body = ctx.r4.u32, alloc_base = ctx.r5.u32, base = ctx.r6.u32,
                   limit = ctx.r7.u32;
    GuestThread* self = current_guest_thread();
    if (!self || !self->identity) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s outside a guest thread", fn);
    if (body != thread_object_body(self->identity)) unimplemented(fn, ctx, "other_thread", body);
    if (limit >= base || sp < limit || sp > base || (sp & 7) ||
        !r.mem->is_accessible(limit, base - limit, Protect::ReadWrite))
        misuse(fn, ctx, "stack_range", sp);
    const uint32_t pcr = ctx.r13.u32;
    if (pcr != self->pcr || !r.mem->is_accessible(pcr + kPcrStackBase, 8, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s: r13=0x%08X is not the thread's PCR", fn, pcr);
    guest_write_be32(pcr + kPcrStackBase, base);
    guest_write_be32(pcr + kPcrStackEnd, limit);
    thread_object_set_stack(self->identity, alloc_base, base, limit);
    if (sp == ctx.r1.u32) return;
    host_fiber_transfer(ctx, membase, sp, base, limit);
}

// KeQueryBasePriorityThread (0x0081): LONG (PKTHREAD). The thread's current
// base-priority increment, as recorded by KeSetBasePriorityThread and the
// ExCreateThread priority hints (runtime/docs/SCHEDULER.md). Unknown Body:
// guest fault.
void KeQueryBasePriorityThread(PPCContext& ctx, uint8_t*) {
    std::shared_ptr<ThreadObjectIdentity> identity;
    if (find_thread_object(ctx.r3.u32, &identity) != Status::Ok)
        misuse("KeQueryBasePriorityThread", ctx, "unknown_thread_body", ctx.r3.u32);
    ctx.r3.u64 = uint32_t(thread_object_base_priority(identity));
}

// ---- video ---------------------------------------------------------------------

// VdSetDisplayModeOverride (0x01D4): (ULONG, ULONG, double RefreshRate in f1,
// ULONG, ULONG) per Xenia/rexglue (unnamed integer arguments; the integer
// arguments after the double are read from r6/r7, their positional slots).
// R-comp never overrides the scan-out mode (the PS5 presentation owns it), so
// the request with every argument zero -- no override -- describes the state
// that holds and returns 0 (the value Xenia returns). A real override cannot
// be applied and traps with its values.
void VdSetDisplayModeOverride(PPCContext& ctx, uint8_t*) {
    const double refresh = ctx.f1.f64;
    if (!ctx.r3.u32 && !ctx.r4.u32 && refresh == 0.0 && !ctx.r6.u32 && !ctx.r7.u32) {
        ctx.r3.u64 = 0;
        return;
    }
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!VdSetDisplayModeOverride (0x%08X 0x%08X refresh=%.3f 0x%08X 0x%08X; r5=0x%08X) lr=0x%08X: "
                "R-comp cannot override the presented display mode",
                ctx.r3.u32, ctx.r4.u32, refresh, ctx.r6.u32, ctx.r7.u32, ctx.r5.u32, (uint32_t)ctx.lr);
}

// ---- IRP completion ---------------------------------------------------------------

// IoCompleteRequest (0x0035): VOID (PIRP, CCHAR PriorityBoost) and
// IoInvalidDeviceRequest (0x0041): the default IRP dispatch routine of a
// driver object. Both act on an IRP the I/O manager sent to a guest driver.
// R-comp's I/O manager never builds or dispatches IRPs (IoAllocateIrp,
// IoBuild*Request and IoCallDriver are not implemented; NtCreateFile/NtReadFile
// never route to a guest driver), so no IRP R-comp could complete exists: a
// call is reported with its arguments.
void IoCompleteRequest(PPCContext& ctx, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!IoCompleteRequest irp=0x%08X boost=%u lr=0x%08X: R-comp dispatches no IRP to guest drivers",
                ctx.r3.u32, ctx.r4.u32 & 0xFF, (uint32_t)ctx.lr);
}
void IoInvalidDeviceRequest(PPCContext& ctx, uint8_t*) {
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!IoInvalidDeviceRequest (0x%08X 0x%08X) lr=0x%08X: R-comp dispatches no IRP to guest drivers",
                ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x0035, "IoCompleteRequest", &IoCompleteRequest},
    {0x0041, "IoInvalidDeviceRequest", &IoInvalidDeviceRequest},
    {0x0081, "KeQueryBasePriorityThread", &KeQueryBasePriorityThread},
    {0x009B, "KeSetCurrentStackPointers", &KeSetCurrentStackPointers},
    {0x00BB, "MmCreateKernelStack", &MmCreateKernelStack},
    {0x00BC, "MmDeleteKernelStack", &MmDeleteKernelStack},
    {0x00BF, "MmIsAddressValid", &MmIsAddressValid},
    {0x00C0, "MmLockAndMapSegmentArray", &MmLockAndMapSegmentArray},
    {0x00C2, "MmMapIoSpace", &MmMapIoSpace},
    {0x00C9, "MmUnlockAndUnmapSegmentArray", &MmUnlockAndUnmapSegmentArray},
    {0x01A9, "RtlImageNtHeader", &RtlImageNtHeader},
    {0x01D4, "VdSetDisplayModeOverride", &VdSetDisplayModeOverride},
};

}  // namespace

Status register_xboxkrnl_more_hle() {
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
