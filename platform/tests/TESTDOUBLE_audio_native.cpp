// Host test double only. Never linked into a production platform target.
#include "TESTDOUBLE_audio_native.h"
#include "../ps5/audio_native.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {
struct Port {
  bool alive = true;
  bool gated = false;
  bool fail_output = false;
  bool fail_close = false;
  unsigned calls = 0;
  unsigned closes = 0;
  std::vector<TESTDOUBLE_audio::Samples> captured;
};
struct State {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<int32_t, Port> ports;
  int32_t last = 0;
  bool init_fail = false, open_fail = false;
  bool prime_fail = false, close_fail = false, new_gate = false;
  bool valid_config = true;
};
State& state() { static State value; return value; }
constexpr auto timeout = std::chrono::seconds(3);
}  // namespace

namespace TESTDOUBLE_audio {
void reset() {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.init_fail = s.open_fail = s.prime_fail = s.close_fail = s.new_gate = false;
  s.valid_config = true;
  for (auto& [id, p] : s.ports) {
    (void)id;
    p.gated = p.fail_output = p.fail_close = false;
  }
  s.changed.notify_all();
}
#define FLAG(name, field) void name(bool value) { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); s.field = value; s.changed.notify_all(); }
FLAG(fail_init, init_fail)
FLAG(fail_open, open_fail)
FLAG(fail_prime, prime_fail)
FLAG(fail_any_close, close_fail)
FLAG(gate_new_ports, new_gate)
#undef FLAG
#define PORTFLAG(name, field) void name(int32_t port, bool value) { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); s.ports.at(port).field = value; s.changed.notify_all(); }
PORTFLAG(fail_output, fail_output)
PORTFLAG(fail_close, fail_close)
PORTFLAG(gate, gated)
#undef PORTFLAG
int32_t last_port() { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); return s.last; }
unsigned alive_ports() { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); unsigned n = 0; for (auto& [id, p] : s.ports) { (void)id; n += p.alive; } return n; }
unsigned close_calls(int32_t port) { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); return s.ports.at(port).closes; }
bool wait_calls(int32_t port, unsigned count) { auto& s = state(); std::unique_lock<std::mutex> l(s.mutex); return s.changed.wait_for(l, timeout, [&] { return s.ports.at(port).calls >= count; }); }
bool wait_captures(int32_t port, unsigned count) { auto& s = state(); std::unique_lock<std::mutex> l(s.mutex); return s.changed.wait_for(l, timeout, [&] { return s.ports.at(port).captured.size() >= count; }); }
std::vector<Samples> captures(int32_t port) { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); return s.ports.at(port).captured; }
bool config_valid() { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); return s.valid_config; }
}  // namespace TESTDOUBLE_audio

extern "C" int32_t sceAudioOutInit() { auto& s = state(); std::lock_guard<std::mutex> l(s.mutex); return s.init_fail ? -101 : 0; }
extern "C" int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index,
                                    uint32_t frames, uint32_t hz, uint32_t format) {
  auto& s = state(); std::lock_guard<std::mutex> l(s.mutex);
  const bool valid = user == 0xFF && type == 0 && index == 0 &&
                     frames == 256 && hz == 48000 && format == 1;
  s.valid_config &= valid;
  if (!valid) return -106;
  if (s.open_fail) return -103;
  Port p; p.gated = s.new_gate;
  s.ports.emplace(++s.last, std::move(p));
  return s.last;
}
extern "C" int32_t sceAudioOutOutput(int32_t port, const void* samples) {
  auto& s = state(); std::unique_lock<std::mutex> l(s.mutex);
  Port& p = s.ports.at(port);
  const unsigned call = ++p.calls;
  s.changed.notify_all();
  if (!p.alive || !samples || (call == 1 && s.prime_fail)) return -104;
  // The priming call is immediate. Worker calls may be held before reading
  // the native pointer to exercise storage ownership during close.
  if (call > 1) s.changed.wait(l, [&] { return !p.gated || p.fail_output || !p.alive; });
  if (p.fail_output || !p.alive) return -104;
  TESTDOUBLE_audio::Samples copy;
  std::memcpy(copy.data(), samples, sizeof(copy));
  p.captured.push_back(copy);
  s.changed.notify_all();
  l.unlock();
  if (call > 1) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return 0;
}
extern "C" int32_t sceAudioOutClose(int32_t port) {
  auto& s = state(); std::lock_guard<std::mutex> l(s.mutex);
  Port& p = s.ports.at(port); ++p.closes;
  if (s.close_fail || p.fail_close) return -105;
  p.alive = false;
  s.changed.notify_all();
  return 0;
}
