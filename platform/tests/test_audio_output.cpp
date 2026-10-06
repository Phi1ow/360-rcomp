#include "rcomp/audio_output.h"
#include "../common/audio_mix.h"
#include "TESTDOUBLE_audio_native.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

namespace {
unsigned tests = 0;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); std::exit(1); } } while (false)
void pass(const char* name) { ++tests; std::printf("PASS %s\n", name); }
using Input = std::array<float, rcomp::audio::kInputSamples>;
bool await_status(rcomp_audio_stream token, int status, Input& input) {
  for (unsigned i = 0; i < 1000; ++i) {
    if (rcomp_audio_submit(token, input.data()) == status) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
}  // namespace

int main() {
  using namespace TESTDOUBLE_audio;
  Input input{};
  rcomp_audio_stream token = UINT64_C(0xdeadbeef);
  REQUIRE(rcomp_audio_open(nullptr) == RCOMP_AUDIO_INVALID_ARGUMENT);
  REQUIRE(rcomp_audio_submit(0, input.data()) == RCOMP_AUDIO_INVALID_ARGUMENT);
  REQUIRE(rcomp_audio_close(0) == RCOMP_AUDIO_INVALID_ARGUMENT);
  fail_init(true);
  REQUIRE(rcomp_audio_open(&token) == RCOMP_AUDIO_DEVICE_ERROR);
  REQUIRE(token == UINT64_C(0xdeadbeef));
  reset(); fail_open(true);
  REQUIRE(rcomp_audio_open(&token) == RCOMP_AUDIO_DEVICE_ERROR);
  REQUIRE(token == UINT64_C(0xdeadbeef));
  REQUIRE(alive_ports() == 0);
  reset(); fail_prime(true);
  REQUIRE(rcomp_audio_open(&token) == RCOMP_AUDIO_DEVICE_ERROR);
  REQUIRE(token == UINT64_C(0xdeadbeef));
  REQUIRE(alive_ports() == 0);
  REQUIRE(config_valid());
  pass("failed init/open/first output accepts no stream on SYSTEM route");

  // Cleanup failure on failed open remains owned, next open retries it.
  reset(); fail_prime(true); fail_any_close(true);
  REQUIRE(rcomp_audio_open(&token) == RCOMP_AUDIO_DEVICE_ERROR);
  const int32_t hidden = last_port();
  REQUIRE(alive_ports() == 1);
  REQUIRE(token == UINT64_C(0xdeadbeef));
  reset(); gate_new_ports(true);
  REQUIRE(rcomp_audio_open(&token) == RCOMP_AUDIO_OK);
  const int32_t port = last_port();
  REQUIRE(close_calls(hidden) == 2);
  REQUIRE(alive_ports() == 1);
  REQUIRE(config_valid());
  REQUIRE(wait_calls(port, 2));
  REQUIRE(rcomp_audio_submit(token, nullptr) == RCOMP_AUDIO_INVALID_ARGUMENT);
  input[0] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(rcomp_audio_submit(token, input.data()) == RCOMP_AUDIO_INVALID_ARGUMENT);
  input[0] = std::numeric_limits<float>::infinity();
  REQUIRE(rcomp_audio_submit(token, input.data()) == RCOMP_AUDIO_INVALID_ARGUMENT);
  input.fill(0);
  for (unsigned frame = 0; frame < 256; ++frame) input[frame * 6] = 0.5f;
  TESTDOUBLE_audio::Samples expected;
  rcomp::audio::downmix(input.data(), expected.data());
  for (unsigned i = 0; i < 8; ++i) REQUIRE(rcomp_audio_submit(token, input.data()) == RCOMP_AUDIO_OK);
  REQUIRE(rcomp_audio_submit(token, input.data()) == RCOMP_AUDIO_BUSY);
  input.fill(-99.0f); // caller buffer is reused before the device reads queue.
  gate(port, false);
  REQUIRE(wait_captures(port, 10));
  auto out = captures(port);
  for (unsigned i = 2; i < 10; ++i) REQUIRE(out[i] == expected);
  REQUIRE(rcomp_audio_close(token) == RCOMP_AUDIO_OK);
  REQUIRE(alive_ports() == 0);
  REQUIRE(rcomp_audio_close(token) == RCOMP_AUDIO_OK);
  REQUIRE(rcomp_audio_submit(token, input.data()) == RCOMP_AUDIO_STOPPED);
  pass("bounded queue copies six-channel input and uses stereo native config");

  // Native output failure poisons submissions; cleanup failure retains port.
  reset(); gate_new_ports(true);
  rcomp_audio_stream failed = 0;
  REQUIRE(rcomp_audio_open(&failed) == RCOMP_AUDIO_OK);
  const int32_t failed_port = last_port();
  REQUIRE(wait_calls(failed_port, 2));
  fail_output(failed_port, true);
  input.fill(0);
  REQUIRE(await_status(failed, RCOMP_AUDIO_DEVICE_ERROR, input));
  fail_close(failed_port, true);
  REQUIRE(rcomp_audio_close(failed) == RCOMP_AUDIO_DEVICE_ERROR);
  REQUIRE(alive_ports() == 1);
  REQUIRE(rcomp_audio_submit(failed, input.data()) == RCOMP_AUDIO_STOPPED);
  fail_close(failed_port, false);
  REQUIRE(rcomp_audio_close(failed) == RCOMP_AUDIO_OK);
  REQUIRE(alive_ports() == 0);
  REQUIRE(close_calls(failed_port) == 2);
  pass("native failure rejects submission; failed close preserves ownership for retry");

  // Close waits for the consumer, cancels all queued data, rejects new submit.
  reset(); gate_new_ports(true);
  rcomp_audio_stream closing = 0;
  REQUIRE(rcomp_audio_open(&closing) == RCOMP_AUDIO_OK);
  const int32_t closing_port = last_port();
  REQUIRE(wait_calls(closing_port, 2));
  for (unsigned i = 0; i < 8; ++i) REQUIRE(rcomp_audio_submit(closing, input.data()) == RCOMP_AUDIO_OK);
  std::atomic<bool> closed{false};
  std::thread closer([&] { REQUIRE(rcomp_audio_close(closing) == RCOMP_AUDIO_OK); closed.store(true); });
  REQUIRE(await_status(closing, RCOMP_AUDIO_STOPPED, input));
  REQUIRE(!closed.load());
  REQUIRE(close_calls(closing_port) == 0);
  gate(closing_port, false);
  closer.join();
  REQUIRE(closed.load());
  REQUIRE(captures(closing_port).size() == 2);
  REQUIRE(alive_ports() == 0);
  pass("close waits for blocked native consumer and cancels pending blocks");

  // Old close remains idempotent after reuse and cannot close a new port.
  reset(); gate_new_ports(true);
  std::array<rcomp_audio_stream, 8> streams{};
  for (auto& s : streams) REQUIRE(rcomp_audio_open(&s) == RCOMP_AUDIO_OK);
  REQUIRE(alive_ports() == 8);
  rcomp_audio_stream overflow = UINT64_C(0xaabbccdd);
  REQUIRE(rcomp_audio_open(&overflow) == RCOMP_AUDIO_BUSY);
  REQUIRE(overflow == UINT64_C(0xaabbccdd));
  REQUIRE(rcomp_audio_close(closing) == RCOMP_AUDIO_OK);
  REQUIRE(alive_ports() == 8);
  REQUIRE(rcomp_audio_submit(closing, input.data()) == RCOMP_AUDIO_STOPPED);
  for (auto s : streams) REQUIRE(rcomp_audio_submit(s, input.data()) == RCOMP_AUDIO_OK);
  gate_new_ports(false);
  reset();
  for (auto s : streams) REQUIRE(rcomp_audio_close(s) == RCOMP_AUDIO_OK);
  REQUIRE(alive_ports() == 0);
  pass("independent ports, capacity, generations and stale-token idempotence");

  // Concurrent producers / close / reuse exercise queue & registry ownership.
  for (unsigned cycle = 0; cycle < 32; ++cycle) {
    rcomp_audio_stream s = 0;
    REQUIRE(rcomp_audio_open(&s) == RCOMP_AUDIO_OK);
    std::atomic<bool> stop{false};
    std::array<std::thread, 4> producers;
    for (auto& thread : producers) thread = std::thread([&] {
      while (!stop.load()) {
        const int status = rcomp_audio_submit(s, input.data());
        REQUIRE(status == RCOMP_AUDIO_OK || status == RCOMP_AUDIO_BUSY || status == RCOMP_AUDIO_STOPPED);
        std::this_thread::yield();
      }
    });
    REQUIRE(rcomp_audio_close(s) == RCOMP_AUDIO_OK);
    stop.store(true);
    for (auto& thread : producers) thread.join();
    REQUIRE(rcomp_audio_close(s) == RCOMP_AUDIO_OK);
  }
  REQUIRE(alive_ports() == 0);
  pass("concurrent submit/close and 32 slot reuses");

  // Independent numerical checks on routing, headroom and finite extremes.
  constexpr double gain = 1.0 / (1.5 + 2 * 0.70710678118654752440);
  input.fill(0);
  for (unsigned channel = 0; channel < 6; ++channel) {
    input[channel] = 1.0f;
    rcomp::audio::downmix(input.data(), expected.data());
    const double coefficient = channel < 2 ? 1.0 : channel == 3 ? 0.5 : 0.70710678118654752440;
    const int wanted = static_cast<int>(coefficient * gain * 32767 + 0.5);
    const bool left = channel == 0 || channel == 2 || channel == 3 || channel == 4;
    const bool right = channel == 1 || channel == 2 || channel == 3 || channel == 5;
    REQUIRE(expected[0] == (left ? wanted : 0));
    REQUIRE(expected[1] == (right ? wanted : 0));
    input[channel] = 0;
  }
  input.fill(1.0f); rcomp::audio::downmix(input.data(), expected.data());
  REQUIRE(std::all_of(expected.begin(), expected.end(), [](int16_t v) { return v == INT16_MAX; }));
  input.fill(-1.0f); rcomp::audio::downmix(input.data(), expected.data());
  REQUIRE(std::all_of(expected.begin(), expected.end(), [](int16_t v) { return v == INT16_MIN; }));
  input.fill(std::numeric_limits<float>::max()); rcomp::audio::downmix(input.data(), expected.data());
  REQUIRE(std::all_of(expected.begin(), expected.end(), [](int16_t v) { return v == INT16_MAX; }));
  pass("six-channel routing, normalization, saturation and finite float extremes");
  std::printf("PASS audio_output %u/%u suites; PS5 NOT TESTED\n", tests, tests);
}
