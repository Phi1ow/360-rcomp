// CPU/title bootstrap shared by the graphical shell and host rehearsals.
// The caller joins the initial thread and GPU callback producers before
// destruction. This owner quiesces workers created through ExCreateThread.
#pragma once
#include <memory>
#include "rcomp/app/title.h"
#include "rcomp/guest_memory.h"

namespace rcomp::app {
class TitleRuntime {
public:
    static std::unique_ptr<TitleRuntime> Create(const TitleConfig& cfg, std::string* error);
    ~TitleRuntime();
    bool RunEntry(uint32_t arg, uint32_t* exit_code, std::string* error);
    GuestMemory& memory() { return mem_; }
    uint32_t entry_point() const { return entry_; }
private:
    TitleRuntime() = default;
    GuestMemory mem_;
    bool owned_ = false;
    uint32_t entry_ = 0, stack_size_ = 0;
    FILE* log_ = nullptr;
};
} // namespace rcomp::app
