// Guest PPC varargs formatting; the native host va_list is never reused.
#pragma once
#include "rcomp/runtime/status.h"

namespace rcomp::rt {
Status register_xboxkrnl_format_hle();
}
