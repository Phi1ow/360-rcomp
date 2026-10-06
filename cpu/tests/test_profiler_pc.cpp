#include "profiler_pc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

static unsigned checks = 0;
static void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

int main() {
    const RcompPcRxBounds title{0x400000, 0x53b77e0};
    check(title.valid(), "actual phase19 title RX bounds");
    check(title.contains(title.begin), "RX start included");
    check(title.contains(title.end - 1), "RX last byte included");
    check(!title.contains(title.begin - 1), "below RX rejected");
    check(!title.contains(title.end), "RX end excluded");
    check(!title.contains(0x68bc7c8), "phase19 BSS sync_lock rejected");
    check(!title.contains(0x5400000), "rodata outside RX rejected");
    check(!RcompPcRxBounds{}.valid(), "absent weak bounds fail closed");
    check(!RcompPcRxBounds{0, title.end}.valid(), "absent begin fail closed");
    check(!RcompPcRxBounds{title.begin, 0}.valid(), "absent end fail closed");
    check(!RcompPcRxBounds{1, 1}.valid(), "empty bounds rejected");
    check(!RcompPcRxBounds{2, 1}.valid(), "reversed bounds rejected");
    check(!RcompPcRxBounds{1, uint64_t{1} << 63}.valid(), "noncanonical bounds rejected");
    check(!RcompPcRxBounds{uint64_t{1} << 63, ~uint64_t{0}}.valid(), "tagged bounds rejected");
    check(rcomp_pc_stack_word_limit(0) == 0, "null RSP rejected");
    check(rcomp_pc_stack_word_limit(0x400001) == 0, "unaligned RSP rejected");
    check(rcomp_pc_stack_word_limit(uint64_t{1} << 47) == 0, "noncanonical RSP rejected");
    check(rcomp_pc_stack_word_limit(0x400000) == 128, "stack scan capped at 128");
    check(rcomp_pc_stack_word_limit(0x400ff8) == 1, "last subpage word only");
    check(rcomp_pc_stack_word_limit(0x400ff0) == 2, "subpage boundary cap");

    alignas(4096) uint64_t stack[1024]{};
    const uint64_t rsp = reinterpret_cast<uint64_t>(stack);
    constexpr uint64_t library_rip = 0x800123456;
    stack[0] = 0x68bc7c8;
    stack[1] = 0x5400000;
    stack[2] = 0x9df220;  // A stored function pointer still is only a candidate.
    check(rcomp_pc_stack_candidate(library_rip, rsp, title) == stack[2],
          "skip BSS/rodata then accept unvalidated text candidate");
    check(rcomp_pc_stack_candidate(stack[2], rsp, title) == 0,
          "raw title RIP does not acquire a second stack attribution");
    check(rcomp_pc_stack_candidate(library_rip, 1, {}) == 0,
          "absent bounds suppress invalid stack read");
    check(rcomp_pc_stack_candidate(library_rip, 1, {2, 1}) == 0,
          "invalid bounds suppress invalid stack read");
    std::fill(std::begin(stack), std::end(stack), 0);
    stack[128] = title.begin;
    check(rcomp_pc_stack_candidate(library_rip, rsp, title) == 0,
          "129th stack word is never scanned");
    stack[127] = title.end - 1;
    check(rcomp_pc_stack_candidate(library_rip, rsp, title) == title.end - 1,
          "128th stack word remains eligible");
    stack[511] = title.end;
    stack[512] = title.begin;
    check(rcomp_pc_stack_candidate(library_rip, rsp + 511 * 8, title) == 0,
          "candidate in next subpage is never read");
    stack[511] = title.begin;
    check(rcomp_pc_stack_candidate(library_rip, rsp + 511 * 8, title) == title.begin,
          "last word of subpage eligible");

    RcompPcSamples<16, 16> samples;
    stack[0] = 0x68bc7c8; stack[1] = 0x9df220;
    samples.record(library_rip, rsp, title);
    samples.record(0x9df277, rsp, title);
    samples.record(library_rip, rsp, {});
    std::map<uint64_t, uint32_t> raw, candidate;
    const auto rs = samples.raw.drain([&](auto pc, auto n) { raw[pc] += n; });
    const auto cs = samples.candidates.drain([&](auto pc, auto n) { candidate[pc] += n; });
    check(rs.total == 3 && rs.recorded == 3 && rs.dropped == 0,
          "one raw denominator per signal, not raw plus candidate");
    check(raw[library_rip] == 2 && raw[0x9df277] == 1,
          "actual library RIP preserved separately");
    check(cs.total == 1 && cs.recorded == 1 && cs.dropped == 0 && candidate[0x9df220] == 1,
          "candidate channel subset counted independently");
    check(candidate.find(0x68bc7c8) == candidate.end(), "BSS absent from candidate histogram");
    const auto empty = samples.raw.drain([](auto, auto) {});
    check(empty.total == 0 && empty.recorded == 0, "drain does not replay samples");

    RcompPcHistogram<8, 2> collision;
    collision.record(0x400000); collision.record(0x400020); collision.record(0x400040);
    collision.record(0);
    const auto full = collision.drain([](auto, auto) {});
    check(full.total == 4 && full.recorded == 2 && full.dropped == 2,
          "bounded collisions/zero retain denominator and explicit drops");

    RcompPcSamples<16, 16> shared;
    stack[0] = 0x9df220;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (unsigned n = 0; n < 25000; ++n) shared.record(library_rip, rsp, title);
        });
    for (auto& t : threads) t.join();
    const auto parallel_raw = shared.raw.drain([](auto, auto) {});
    const auto parallel_candidate = shared.candidates.drain([](auto, auto) {});
    check(parallel_raw.total == 100000 && parallel_raw.recorded == 100000 &&
          parallel_raw.dropped == 0 && parallel_candidate.total == 100000 &&
          parallel_candidate.recorded == 100000 && parallel_candidate.dropped == 0,
          "100000 concurrent raw/candidate events remain distinct");
    std::printf("PASS profiler RX/candidate policy %u checks; PS5 NOT TESTED\n", checks);
}
