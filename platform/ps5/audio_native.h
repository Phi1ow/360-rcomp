// Minimal original ABI declarations. The SDK import libraries and firmware
// exports are audited separately; declarations do not establish PS5 behaviour.
#pragma once
#include <cstdint>
extern "C" {
int32_t sceAudioOutInit();
int32_t sceAudioOutOpen(int32_t user, int32_t port, int32_t index,
                       uint32_t frames, uint32_t frequency, uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, const void* samples);
int32_t sceAudioOutClose(int32_t handle);
}
