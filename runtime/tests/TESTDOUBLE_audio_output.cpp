// Test-owned bounded queues; no claim of host or PS5 audio output.
#include "TESTDOUBLE_audio_output.h"
#include "rcomp/audio_output.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <deque>
namespace {
struct TESTDOUBLE_State {
    std::mutex mutex;
    std::condition_variable changed;
    int open_result = RCOMP_AUDIO_UNSUPPORTED, submit_result = RCOMP_AUDIO_OK, close_result = RCOMP_AUDIO_OK;
    bool block = false, entered = false;
    unsigned opens = 0, closes = 0;
    uint64_t next = 1;
    std::map<uint64_t,std::deque<std::array<float,1536>>> streams;
    std::vector<std::array<float,1536>> accepted;
} state;
}
namespace TESTDOUBLE_audio {
void reset() {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.open_result = RCOMP_AUDIO_UNSUPPORTED; state.submit_result = state.close_result = RCOMP_AUDIO_OK;
    state.block = state.entered = false; state.opens = state.closes = 0;
    state.streams.clear(); state.accepted.clear();
}
void set_open_result(int v) { std::lock_guard<std::mutex> lock(state.mutex); state.open_result=v; }
void set_submit_result(int v) { std::lock_guard<std::mutex> lock(state.mutex); state.submit_result=v; }
void set_close_result(int v) { std::lock_guard<std::mutex> lock(state.mutex); state.close_result=v; }
void block_submit(bool v) { std::lock_guard<std::mutex> lock(state.mutex); state.block=v; if(v) state.entered=false; state.changed.notify_all(); }
bool wait_submit_entered(unsigned ms) { std::unique_lock<std::mutex> lock(state.mutex); return state.changed.wait_for(lock,std::chrono::milliseconds(ms),[] { return state.entered; }); }
unsigned opened() { std::lock_guard<std::mutex> lock(state.mutex); return state.opens; }
unsigned closed() { std::lock_guard<std::mutex> lock(state.mutex); return state.closes; }
unsigned queued() { std::lock_guard<std::mutex> lock(state.mutex); unsigned n=0; for(auto& s:state.streams)n+=s.second.size(); return n; }
std::vector<std::array<float,1536>> frames() { std::lock_guard<std::mutex> lock(state.mutex); return state.accepted; }
void drain() { std::lock_guard<std::mutex> lock(state.mutex); for(auto& s:state.streams)s.second.clear(); }
}
extern "C" int rcomp_audio_open(rcomp_audio_stream* out) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if(!out)return RCOMP_AUDIO_INVALID_ARGUMENT;
    if(state.open_result!=RCOMP_AUDIO_OK)return state.open_result;
    if(state.streams.size()>=8)return RCOMP_AUDIO_BUSY;
    const uint64_t stream=state.next++; state.streams[stream]={}; ++state.opens; *out=stream; return RCOMP_AUDIO_OK;
}
extern "C" int rcomp_audio_submit(rcomp_audio_stream stream,const float* samples) {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.entered=true; state.changed.notify_all();
    state.changed.wait(lock,[] { return !state.block; });
    auto found=state.streams.find(stream);
    if(!samples)return RCOMP_AUDIO_INVALID_ARGUMENT;
    if(found==state.streams.end())return RCOMP_AUDIO_STOPPED;
    if(state.submit_result!=RCOMP_AUDIO_OK)return state.submit_result;
    if(found->second.size()==8)return RCOMP_AUDIO_BUSY;
    std::array<float,1536> owned; std::memcpy(owned.data(),samples,sizeof(owned));
    found->second.push_back(owned); state.accepted.push_back(owned); return RCOMP_AUDIO_OK;
}
extern "C" int rcomp_audio_close(rcomp_audio_stream stream) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if(state.close_result!=RCOMP_AUDIO_OK)return state.close_result;
    const auto found=state.streams.find(stream);
    if(found==state.streams.end())return RCOMP_AUDIO_OK;
    state.streams.erase(found); ++state.closes; return RCOMP_AUDIO_OK;
}
