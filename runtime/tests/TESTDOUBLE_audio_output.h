#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace TESTDOUBLE_audio {
void reset();
void set_open_result(int);
void set_submit_result(int);
void set_close_result(int);
void block_submit(bool);
bool wait_submit_entered(unsigned timeout_ms = 1000);
unsigned opened();
unsigned closed();
unsigned queued();
std::vector<std::array<float,1536>> frames();
void drain();
}
