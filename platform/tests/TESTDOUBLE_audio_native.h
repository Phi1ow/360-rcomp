#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace TESTDOUBLE_audio {
using Samples = std::array<int16_t, 512>;
void reset();
void fail_init(bool);
void fail_open(bool);
void fail_prime(bool);
void fail_output(int32_t port, bool);
void fail_close(int32_t port, bool);
void fail_any_close(bool);
void gate(int32_t port, bool);
void gate_new_ports(bool);
int32_t last_port();
unsigned alive_ports();
unsigned close_calls(int32_t port);
bool wait_calls(int32_t port, unsigned count);
bool wait_captures(int32_t port, unsigned count);
std::vector<Samples> captures(int32_t port);
bool config_valid();
}  // namespace TESTDOUBLE_audio
