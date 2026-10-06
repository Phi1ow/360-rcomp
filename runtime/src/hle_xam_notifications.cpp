// Real listener queues and readiness. ABI from pinned public Xenia/rexglue;
// no synthesized startup sign-in/UI/network notifications.
#include "rcomp/runtime/notifications.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {
namespace {
struct Message { uint32_t id, parameter; };
struct Listener final : HandleObject {
    static constexpr HandleKind kKind=HandleKind::Notification;
    const uint64_t generation,mask;
    const uint32_t max_version;
    std::mutex mutex;
    std::deque<Message> pending;
    std::atomic<bool> ready{false};
    Listener(uint64_t generation_,uint64_t mask_,uint32_t version)
        :generation(generation_),mask(mask_),max_version(version) {}
    HandleKind kind() const override { return kKind; }
    bool dispatcher_ready() const override { return ready.load(std::memory_order_acquire); }
};
std::mutex registry_mutex;
std::vector<std::weak_ptr<Listener>> listeners;
Runtime& current(const char* name) {
    auto* owner=runtime();
    if (!owner) rcomp_fatal(RCOMP_FATAL_INTERNAL,"%s before runtime_init",name);
    return *owner;
}
void XamNotifyCreateListener(PPCContext& ctx,uint8_t*) {
    auto& owner=current("XamNotifyCreateListener");
    const uint64_t mask=ctx.r3.u64;
    const uint32_t version=ctx.r4.u32;
    if (version>10) rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                               "XamNotifyCreateListener max_version=%u exceeds documented profile",version);
    uint32_t handle=0;
#if defined(__cpp_exceptions)
    try {
#endif
        auto listener=std::make_shared<Listener>(owner.generation,mask,version);
        std::lock_guard<std::mutex> lock(registry_mutex);
        listeners.erase(std::remove_if(listeners.begin(),listeners.end(),
                         [](const auto& weak){return weak.expired();}),listeners.end());
        listeners.reserve(listeners.size()+1);
        const Status status=owner.handles.insert(listener,&handle);
        if (status==Status::Ok) listeners.emplace_back(listener);
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) { handle=0; }
#endif
    ctx.r3.u64=handle;
}
void XNotifyGetNext(PPCContext& ctx,uint8_t*) {
    auto& owner=current("XNotifyGetNext");
    const uint32_t handle=ctx.r3.u32,match=ctx.r4.u32,id_ptr=ctx.r5.u32,param_ptr=ctx.r6.u32;
    if ((id_ptr&&!owner.mem->is_accessible(id_ptr,4,Protect::ReadWrite)) ||
        (param_ptr&&!owner.mem->is_accessible(param_ptr,4,Protect::ReadWrite)))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS,"XNotifyGetNext output not writable");
    if (param_ptr) guest_write_be32(param_ptr,0);
    if (!id_ptr) {ctx.r3.u64=0;return;}
    guest_write_be32(id_ptr,0);
    std::shared_ptr<Listener> listener;
    if (owner.handles.lookup_as<Listener>(handle,&listener)!=Status::Ok ||
        listener->generation!=owner.generation) {ctx.r3.u64=0;return;}
    Message result{};
    bool found=false;
    {
        std::lock_guard<std::mutex> lock(listener->mutex);
        auto selected=listener->pending.begin();
        if (match) selected=std::find_if(selected,listener->pending.end(),
                                         [&](const Message& item){return item.id==match;});
        if (selected!=listener->pending.end()) {
            result=*selected;listener->pending.erase(selected);found=true;
        }
        listener->ready.store(!listener->pending.empty(),std::memory_order_release);
    }
    if (found) {
        guest_write_be32(id_ptr,result.id);
        if (param_ptr) guest_write_be32(param_ptr,result.parameter);
    }
    ctx.r3.u64=found?1:0;
}
// XNotifyPositionUI(DWORD Position) -> VOID: where the system notification
// popup would appear (XNOTIFYUI_POS_*: 0..2 top, 4..6 centre, 8..10 bottom).
// R-comp draws no notification UI, so the request is only recorded; unknown
// position codes stop with a diagnostic instead of being ignored.
std::atomic<uint32_t> g_notify_position{0};
void XNotifyPositionUI(PPCContext& ctx,uint8_t*) {
    const uint32_t position=ctx.r3.u32;
    if (position>10 || position==3 || position==7)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,"XNotifyPositionUI position=%u lr=0x%08X",position,(uint32_t)ctx.lr);
    g_notify_position.store(position,std::memory_order_relaxed);
}
// XamShowSigninUI(DWORD Panes, DWORD Flags) -> ERROR_SUCCESS. There is no sign-in
// service or profile store, so nobody can sign in: the (nonexistent) system UI
// opens and is dismissed without a change. The title sees exactly that:
// XN_SYS_UI (0x9) with parameter 1 (opened) then 0 (closed), no sign-in change.
// Found when the actual GTA IV was told to sign in (A on "You are not signed in").
constexpr uint32_t kXnSysUi = 0x00000009u;
void XamShowSigninUI(PPCContext& ctx,uint8_t*) {
    (void)publish_xam_notification(kXnSysUi,1);
    (void)publish_xam_notification(kXnSysUi,0);
    ctx.r3.u64=0;
}
struct Impl {uint32_t ordinal;const char* name;PPCFunc* function;};
const Impl entries[]={
    {0x028A,"XamNotifyCreateListener",&XamNotifyCreateListener},
    {0x028B,"XNotifyGetNext",&XNotifyGetNext},
    {0x028C,"XNotifyPositionUI",&XNotifyPositionUI},
    {0x02BC,"XamShowSigninUI",&XamShowSigninUI},
};
}
Status publish_xam_notification(uint32_t id,uint32_t parameter) {
    auto* owner=runtime();
    if (!owner) return Status::NotInitialized;
    if (id&0x80000000u) return Status::InvalidArgument;
    const uint32_t category=(id>>25)&63,version=(id>>16)&511;
    bool enqueued=false;
    Status status=Status::Ok;
#if defined(__cpp_exceptions)
    try {
#endif
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        for (auto it=listeners.begin();it!=listeners.end();) {
            auto listener=it->lock();
            if (!listener) {it=listeners.erase(it);continue;}
            ++it;
            if (listener->generation!=owner->generation || !(listener->mask&(uint64_t(1)<<category)) ||
                version>listener->max_version) continue;
            std::lock_guard<std::mutex> queue_lock(listener->mutex);
            if (listener->pending.size()>=65536) {status=Status::OutOfMemory;break;}
            listener->pending.push_back({id,parameter});
            listener->ready.store(true,std::memory_order_release);
            enqueued=true;
        }
#if defined(__cpp_exceptions)
    } catch (const std::bad_alloc&) {status=Status::OutOfMemory;}
#endif
    // Dispatcher waits only perform atomic readiness reads. Releasing these
    // locks before waking avoids queue/dispatcher lock inversion.
    if (enqueued) notify_runtime_waiters();
    return status;
}
void reset_xam_notifications() {
    g_notify_position.store(0,std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(registry_mutex);listeners.clear();
}
Status register_xam_notifications_hle() {
    for (const auto& entry:entries) {
        uint32_t ordinal=0;
        if (!export_ordinal(kModuleXam,entry.name,&ordinal)||ordinal!=entry.ordinal) return Status::Conflict;
        const Status status=register_import(kModuleXam,entry.ordinal,entry.function,entry.name);
        if (status!=Status::Ok) return status;
    }
    return Status::Ok;
}
}
