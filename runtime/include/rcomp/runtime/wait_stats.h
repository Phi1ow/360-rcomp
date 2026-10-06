// Outcomes of the guest's waits (owner: Agent 3, runtime/).
//
// Hypothesis behind it: the guest polls with zero timeouts in loops, every poll takes the one
// runtime-wide dispatcher lock, and the system calls the polls used to make were an accidental
// throttle. The frame-rate counter prints the per-second difference of these counts.
#pragma once

#include <stdint.h>

namespace rcomp::rt {

struct WaitStats {
    uint64_t ready_at_once = 0;      // the object was already signalled
    uint64_t immediate_timeouts = 0; // a poll: the deadline had passed, the object was not ready
    uint64_t blocked = 0;            // sleeps on the condition variable (one per blocking pass)
};

// Counts of every guest thread since start. All zero unless the runtime was built with
// RCOMP_RUNTIME_WAIT_STATS=ON (counting costs an atomic add per wait).
WaitStats wait_stats();

// CPU time of the guest threads since the previous call, as text: " main:<ms>" for the title's
// main guest thread (after note_main_guest_thread), then " <tid>:<entry>:<ms>" for every live
// worker (pthread_getcpuclockid). The frame-rate counter prints it every few seconds (RCOMP-THREADS):
// which guest thread is near a full core in a scene names the thread that paces the frame.
// Returns the number of characters written (0 when nothing is known yet).
size_t guest_thread_cpu_report(char* out, size_t capacity);
// Wall milliseconds each guest thread spent blocked in the runtime's waits (object waits and non-zero sleeps) since the
// previous call, in the same text form (" main:<ms>" then " <tid>:<ms>"): with the CPU report it splits a thread's second
// into computing, blocked and the rest (yields, polls). Empty unless built with RCOMP_RUNTIME_WAIT_STATS=ON.
size_t guest_thread_wait_report(char* out, size_t capacity);
// Called by the thread that is about to run the title's main guest thread (app/src/title_runtime.cpp).
void note_main_guest_thread();
// True on the host thread note_main_guest_thread was called from (the title's main guest thread).
bool is_main_guest_host_thread();
// Called by a guest thread on its own behalf (every import dispatch of the RCOMP_RUNTIME_WAIT_STATS
// builds): samples its CPU time at most once a millisecond for the report above.
void thread_cpu_sample_tick();

}  // namespace rcomp::rt
