// R-comp implementations of the few rexglue platform functions the Vulkan
// backend and presenter use (owner: gpu/xenos). rexglue's versions live in
// files that also carry its window system and signal-based memory code.
#include <unistd.h>

#include <rex/memory/utils.h>
#include <rex/ui/window.h>

#include "rcomp/diag.h"

namespace rex::memory {

size_t page_size() {
    static const size_t size = [] {
        const long v = sysconf(_SC_PAGESIZE);
        return v > 0 ? size_t(v) : size_t(4096);
    }();
    return size;
}

}  // namespace rex::memory

namespace rex::ui {

// R-comp has no window system (RCOMP_XENOS_NO_WINDOW_SYSTEM): no Window
// object is ever created, so the presenter never reaches this. Fatal if it
// does rather than pretending a window took the presenter.
void Window::SetPresenter(Presenter*) {
    rcomp_fatal(RCOMP_FATAL_INTERNAL, "xenos: Window::SetPresenter without a window system");
}

}  // namespace rex::ui
