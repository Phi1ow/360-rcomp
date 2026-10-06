#include "rcomp/audio_output.h"
#include "audio_native.h"
#include "../common/audio_mix.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <pthread.h>

namespace {
using namespace rcomp::audio;
constexpr unsigned kPorts = 8;
constexpr unsigned kPendingBlocks = 8;
// The platform contract carries no guest user identity. Route the shared
// native device through SYSTEM, as the public SDL PS5 AudioOut backend does.
constexpr int32_t kSystemUser = 0xFF;
constexpr int32_t kMainPort = 0;
constexpr int32_t kPortIndex = 0;
constexpr uint32_t kStereoS16 = 1;
constexpr uint32_t kFrequency = 48000;

enum class Phase { Empty, Opening, Active, DeviceFailed, Closing, CloseFailed };
struct Block { std::array<float, kInputSamples> samples{}; };
struct Stream {
  // lifecycle serializes joins / native closes. mu protects every published
  // state and queue field. No mu is held across a blocking native operation.
  std::mutex lifecycle;
  std::mutex mu;
  Phase phase = Phase::Empty;
  uint32_t generation = 0;
  bool published = false;
  int32_t port = -1;
  pthread_t worker{};
  bool worker_owned = false;
  bool stop = false;
  int32_t native_error = 0;
  unsigned read = 0;
  unsigned count = 0;
  unsigned native_next = 1;
  std::array<Block, kPendingBlocks> pending{};
  Block in_flight{};
  // A native call may retain its buffer until the next output. Alternate
  // durable storage, retained even after worker failure until Close succeeds.
  std::array<std::array<int16_t, kStereoSamples>, 2> native{};
};
struct Registry {
  std::mutex opening;
  bool audio_initialized = false;
  std::array<Stream, kPorts> streams;
};
Registry& registry() {
  // Process lifetime: no mutex / sample-storage teardown while a native port
  // or worker is still owned, including failed cleanup at process exit.
  alignas(Registry) static unsigned char storage[sizeof(Registry)];
  static Registry* const value = new (storage) Registry;
  return *value;
}

void native_failure(const char* operation, int32_t port, int32_t result) {
  // One message per failed operation. Never log per successful audio block.
  std::fprintf(stderr, "RCOMP-AUDIO native %s port=%d result=%d (0x%08x)\n",
               operation, port, result, static_cast<uint32_t>(result));
}

bool decode(rcomp_audio_stream token, unsigned& slot, uint32_t& generation) {
  const uint32_t low = static_cast<uint32_t>(token);
  generation = static_cast<uint32_t>(token >> 32);
  if (!generation || !low || low > kPorts) return false;
  slot = low - 1;
  return true;
}

// Called with lifecycle held. The port and all native buffers remain owned
// after a failed join/close so the same operation can be retried safely.
int stop_and_close(Stream& stream) {
  bool join = false;
  pthread_t worker{};
  {
    std::lock_guard<std::mutex> lock(stream.mu);
    stream.stop = true;
    stream.phase = Phase::Closing;
    stream.count = 0;  // Explicit cancellation permitted by close contract.
    join = stream.worker_owned;
    worker = stream.worker;
  }
  if (join) {
    const int result = pthread_join(worker, nullptr);
    if (result != 0) {
      native_failure("pthread_join", stream.port, result);
      std::lock_guard<std::mutex> lock(stream.mu);
      stream.phase = Phase::CloseFailed;
      stream.native_error = result;
      return RCOMP_AUDIO_DEVICE_ERROR;
    }
    std::lock_guard<std::mutex> lock(stream.mu);
    stream.worker_owned = false;
  }
  // lifecycle prevents open / another close from changing this port.
  const int32_t result = stream.port < 0 ? 0 : sceAudioOutClose(stream.port);
  if (result < 0) native_failure("close", stream.port, result);
  std::lock_guard<std::mutex> lock(stream.mu);
  if (result < 0) {
    stream.phase = Phase::CloseFailed;
    stream.native_error = result;
    return RCOMP_AUDIO_DEVICE_ERROR;
  }
  stream.port = -1;
  stream.published = false;
  stream.phase = Phase::Empty;
  return RCOMP_AUDIO_OK;
}

void* output_worker(void* opaque) {
  auto& stream = *static_cast<Stream*>(opaque);
  for (;;) {
    unsigned index;
    bool have_block;
    {
      std::lock_guard<std::mutex> lock(stream.mu);
      if (stream.stop) break;
      index = stream.native_next;
      stream.native_next ^= 1;
      have_block = stream.count != 0;
      if (have_block) {
        stream.in_flight = stream.pending[stream.read];
        stream.read = (stream.read + 1) % kPendingBlocks;
        --stream.count;
      }
    }
    if (have_block) {
      downmix(stream.in_flight.samples.data(), stream.native[index].data());
    } else {
      // Real device underrun: output silence through AudioOut, not a discard
      // sink or an acknowledgement of guest data that was not accepted.
      stream.native[index].fill(0);
    }
    const int32_t result = sceAudioOutOutput(stream.port, stream.native[index].data());
    if (result < 0) {
      native_failure("output", stream.port, result);
      std::lock_guard<std::mutex> lock(stream.mu);
      stream.native_error = result;
      if (!stream.stop) stream.phase = Phase::DeviceFailed;
      break;
    }
  }
  return nullptr;
}
}  // namespace

extern "C" int rcomp_audio_open(rcomp_audio_stream* out) {
  if (!out) return RCOMP_AUDIO_INVALID_ARGUMENT;
  Registry& r = registry();
  std::lock_guard<std::mutex> opening(r.opening);
  if (!r.audio_initialized) {
    const int32_t result = sceAudioOutInit();
    if (result < 0) {
      native_failure("init", -1, result);
      return RCOMP_AUDIO_DEVICE_ERROR;
    }
    r.audio_initialized = true;
  }
  for (unsigned slot = 0; slot < kPorts; ++slot) {
    Stream& stream = r.streams[slot];
    std::lock_guard<std::mutex> lifecycle(stream.lifecycle);
    bool rollback;
    {
      std::lock_guard<std::mutex> lock(stream.mu);
      if (stream.published) continue;
      // A failed native rollback was never exposed to the caller. Keep its
      // resources in this slot and retry cleanup on the next open attempt.
      if (stream.phase != Phase::Empty && stream.phase != Phase::CloseFailed) continue;
      rollback = stream.phase == Phase::CloseFailed;
    }
    if (rollback && stop_and_close(stream) != RCOMP_AUDIO_OK) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(stream.mu);
      if (stream.generation == UINT32_MAX) continue;  // Never recycle tokens.
      stream.phase = Phase::Opening;
      stream.stop = false;
      stream.read = stream.count = 0;
      stream.native_error = 0;
      stream.native_next = 1;
      for (auto& block : stream.native) block.fill(0);
    }
    stream.port = sceAudioOutOpen(kSystemUser, kMainPort, kPortIndex,
                                kFrames, kFrequency, kStereoS16);
    if (stream.port < 0) {
      std::fprintf(stderr,
          "RCOMP-AUDIO native open user=%d (0x%08x) port_type=%d index=%d "
          "frames=%u hz=%u format=%u result=%d (0x%08x)\n",
          kSystemUser, static_cast<uint32_t>(kSystemUser), kMainPort, kPortIndex,
          kFrames, kFrequency, kStereoS16, stream.port,
          static_cast<uint32_t>(stream.port));
      std::lock_guard<std::mutex> lock(stream.mu);
      stream.native_error = stream.port;
      stream.port = -1;
      stream.phase = Phase::Empty;
      return RCOMP_AUDIO_DEVICE_ERROR;
    }
    // Check the first real output before publishing a usable stream. Its
    // buffer remains owned in Stream until the native port is closed.
    const int32_t prime_result = sceAudioOutOutput(stream.port, stream.native[0].data());
    if (prime_result < 0) {
      native_failure("prime", stream.port, prime_result);
      {
        std::lock_guard<std::mutex> lock(stream.mu);
        stream.native_error = prime_result;
      }
      stop_and_close(stream);
      return RCOMP_AUDIO_DEVICE_ERROR;
    }
    pthread_attr_t attr;
    int result = pthread_attr_init(&attr);
    if (result == 0) {
      result = pthread_attr_setstacksize(&attr, 256 * 1024);
      if (result == 0) result = pthread_create(&stream.worker, &attr, output_worker, &stream);
      pthread_attr_destroy(&attr);
    }
    if (result != 0) {
      native_failure("pthread_start", stream.port, result);
      {
        std::lock_guard<std::mutex> lock(stream.mu);
        stream.native_error = result;
      }
      stop_and_close(stream);
      return RCOMP_AUDIO_DEVICE_ERROR;
    }
    {
      std::lock_guard<std::mutex> lock(stream.mu);
      stream.worker_owned = true;
      ++stream.generation;
      stream.published = true;
      // The worker may fail between create and this lock. Do not erase that
      // failure by changing DeviceFailed back to Active.
      if (stream.phase == Phase::Opening) stream.phase = Phase::Active;
      if (stream.phase == Phase::Active) {
        *out = (uint64_t(stream.generation) << 32) | (slot + 1);
        return RCOMP_AUDIO_OK;
      }
      stream.published = false;
      --stream.generation;  // No token was published on the failed open.
    }
    stop_and_close(stream);
    return RCOMP_AUDIO_DEVICE_ERROR;
  }
  return RCOMP_AUDIO_BUSY;
}

extern "C" int rcomp_audio_submit(rcomp_audio_stream token, const float* input) {
  unsigned slot;
  uint32_t generation;
  if (!input || !decode(token, slot, generation)) return RCOMP_AUDIO_INVALID_ARGUMENT;
  Stream& stream = registry().streams[slot];
  std::lock_guard<std::mutex> lock(stream.mu);
  if (generation > stream.generation) return RCOMP_AUDIO_INVALID_ARGUMENT;
  if (generation != stream.generation || !stream.published) return RCOMP_AUDIO_STOPPED;
  if (stream.phase == Phase::DeviceFailed) return RCOMP_AUDIO_DEVICE_ERROR;
  if (stream.phase != Phase::Active) return RCOMP_AUDIO_STOPPED;
  if (stream.count == kPendingBlocks) return RCOMP_AUDIO_BUSY;
  if (!finite_block(input)) return RCOMP_AUDIO_INVALID_ARGUMENT;
  Block& block = stream.pending[(stream.read + stream.count) % kPendingBlocks];
  std::memcpy(block.samples.data(), input, sizeof(float) * kInputSamples);
  ++stream.count;
  return RCOMP_AUDIO_OK;
}

extern "C" int rcomp_audio_close(rcomp_audio_stream token) {
  unsigned slot;
  uint32_t generation;
  if (!decode(token, slot, generation)) return RCOMP_AUDIO_INVALID_ARGUMENT;
  Stream& stream = registry().streams[slot];
  std::lock_guard<std::mutex> lifecycle(stream.lifecycle);
  {
    std::lock_guard<std::mutex> lock(stream.mu);
    if (generation > stream.generation) return RCOMP_AUDIO_INVALID_ARGUMENT;
    if (generation < stream.generation || !stream.published) return RCOMP_AUDIO_OK;
  }
  return stop_and_close(stream);
}
