#include <memory>

#include "rcomp/runtime/handle_table.h"
#include "test_util.h"

using namespace rcomp::rt;

namespace {
struct TestObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Test;
    explicit TestObj(int v, bool* destroyed = nullptr) : value(v), destroyed(destroyed) {}
    ~TestObj() override {
        if (destroyed) *destroyed = true;
    }
    HandleKind kind() const override { return kKind; }
    int value;
    bool* destroyed;
};
struct EventObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Event;
    HandleKind kind() const override { return kKind; }
};
struct CountingObj : HandleObject {
    static constexpr HandleKind kKind = HandleKind::Test;
    HandleKind kind() const override { return kKind; }
    void handle_opened() override { ++opened; }
    void handle_closed() override { ++closed; }
    int opened = 0, closed = 0;
};
}  // namespace

int main() {
    HandleTable t(4);
    uint32_t h1 = 0, h2 = 0;
    CHECK_ST(t.insert(std::make_shared<TestObj>(11), &h1), Status::Ok);
    CHECK_ST(t.insert(std::make_shared<EventObj>(), &h2), Status::Ok);
    CHECK(h1 != 0 && h1 != 0xFFFFFFFFu && (h1 & 3) == 0 && h1 != h2);

    std::shared_ptr<TestObj> o;
    CHECK_ST(t.lookup_as<TestObj>(h1, &o), Status::Ok);
    CHECK(o && o->value == 11);

    // Wrong kind is an explicit error, not success.
    std::shared_ptr<EventObj> e;
    CHECK_ST(t.lookup_as<EventObj>(h1, &e), Status::WrongHandleKind);
    CHECK(!e);
    CHECK_ST(t.lookup_as<TestObj>(h2, &o), Status::WrongHandleKind);

    // Garbage handles.
    std::shared_ptr<HandleObject> any;
    CHECK_ST(t.lookup(0, HandleKind::Test, &any), Status::InvalidHandle);
    CHECK_ST(t.lookup(0xFFFFFFFFu, HandleKind::Test, &any), Status::InvalidHandle);
    CHECK_ST(t.lookup(h1 + 4 * 100, HandleKind::Test, &any), Status::InvalidHandle);
    CHECK_ST(t.lookup(h1 | 1, HandleKind::Test, &any), Status::InvalidHandle);

    // Close: object survives while referenced, lookups fail, double close fails.
    bool destroyed = false;
    uint32_t h3 = 0;
    CHECK_ST(t.insert(std::make_shared<TestObj>(33, &destroyed), &h3), Status::Ok);
    std::shared_ptr<TestObj> keep;
    CHECK_ST(t.lookup_as<TestObj>(h3, &keep), Status::Ok);
    CHECK_ST(t.close(h3), Status::Ok);
    CHECK(!destroyed);
    keep.reset();
    CHECK(destroyed);
    CHECK_ST(t.lookup_as<TestObj>(h3, &o), Status::InvalidHandle);
    CHECK_ST(t.close(h3), Status::InvalidHandle);

    // Stale handle: slot reused with a new generation.
    uint32_t h4 = 0;
    CHECK_ST(t.insert(std::make_shared<TestObj>(44), &h4), Status::Ok);
    CHECK((h4 & 0x3FFFFu) == (h3 & 0x3FFFFu));  // same slot
    CHECK(h4 != h3);
    CHECK_ST(t.lookup_as<TestObj>(h3, &o), Status::InvalidHandle);
    CHECK_ST(t.lookup_as<TestObj>(h4, &o), Status::Ok);
    CHECK(o->value == 44);

    // Capacity.
    uint32_t h5 = 0, h6 = 0;
    CHECK_ST(t.insert(std::make_shared<TestObj>(5), &h5), Status::Ok);
    CHECK_ST(t.insert(std::make_shared<TestObj>(6), &h6), Status::TableFull);
    CHECK_EQ(t.live_count(), 4u);
    CHECK_ST(t.insert(nullptr, &h6), Status::InvalidArgument);

    // Exhausting one slot's generation retires it instead of resurrecting an
    // old handle. Retained object references remain alive independently.
    HandleTable finite(1);
    auto retained=std::make_shared<TestObj>(71);
    uint32_t first=0;
    for(unsigned generation=0;generation<256;++generation) {
        uint32_t handle=0;
        CHECK_ST(finite.insert(retained,&handle),Status::Ok);
        if(!generation)first=handle;
        else CHECK_ST(finite.lookup(first,HandleKind::Test,&any),Status::InvalidHandle);
        CHECK_ST(finite.close(handle),Status::Ok);
        CHECK_ST(finite.lookup(handle,HandleKind::Test,&any),Status::InvalidHandle);
    }
    CHECK_ST(finite.insert(retained,&h6),Status::TableFull);
    CHECK_EQ(finite.live_count(),0u);
    CHECK_EQ(retained->value,71);

    // Duplicate publishes a distinct handle to the same object. Lifecycle
    // callbacks count numeric handles independently and close never revives a
    // stale source.
    HandleTable duplicates(3);
    auto counted=std::make_shared<CountingObj>();
    uint32_t source=0,target=0xBAD0CAFE;
    CHECK_ST(duplicates.insert(counted,&source),Status::Ok);
    CHECK_EQ(counted->opened,1); CHECK_EQ(counted->closed,0);
    CHECK_ST(duplicates.duplicate(source,&target),Status::Ok);
    CHECK(source!=target); CHECK_EQ(counted->opened,2); CHECK_EQ(counted->closed,0);
    std::shared_ptr<HandleObject> same_object;
    CHECK_ST(duplicates.lookup_any(target,&same_object),Status::Ok);
    CHECK(same_object==counted);
    CHECK_ST(duplicates.close(source),Status::Ok);
    CHECK_EQ(counted->opened,2); CHECK_EQ(counted->closed,1);
    CHECK_ST(duplicates.lookup_any(source,&same_object),Status::InvalidHandle);
    CHECK_ST(duplicates.lookup_any(target,&same_object),Status::Ok);
    uint32_t untouched=0x12345678;
    CHECK_ST(duplicates.duplicate(source,&untouched),Status::InvalidHandle);
    CHECK_EQ(untouched,0x12345678u);
    CHECK_ST(duplicates.close(target),Status::Ok);
    CHECK_EQ(counted->opened,2); CHECK_EQ(counted->closed,2);

    return test_result("rt_test_handles");
}
