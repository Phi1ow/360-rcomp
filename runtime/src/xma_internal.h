// Internal contract between the XMA device model (src/hle_xboxkrnl_xma.cpp) and the XMA* kernel helpers
// (src/hle_xboxkrnl_xma_api.cpp). Not a public interface.
#pragma once

#include <stdint.h>

#include <functional>

namespace rcomp::rt::xma_device {

// A per-context register group of the device (Kick 0x650, Lock 0x690, Clear 0x6A0). A helper writes
// the context's bit to it exactly as a title store to the register window does, side effects included
// (decoder kick, lock fence, clear reset).
enum class Signal { Kick, Lock, Clear };

// For a live context (an address XMACreateContext returned and XMAReleaseContext has not released; any
// physical window alias of it is accepted), holds the device mutex, optionally writes `signal` first,
// then runs `body(record)` with the record's guest address. Holding the mutex keeps the decoder worker
// from loading or publishing that record meanwhile. False, nothing done, for any other address.
bool with_context(uint32_t address, const Signal* signal, const std::function<void(uint32_t record)>& body);

}  // namespace rcomp::rt::xma_device
