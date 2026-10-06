#include "rcomp/func_table.h"
#include "rcomp/aot_modules.h"
#include "rcomp/indirect_cache.h"
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <unordered_map>
#include <vector>

static void TESTDOUBLE_entry(PPCContext&, uint8_t*) {}
// Distinct non-null host pointers that are compared, never called.
static PPCFunc* TESTDOUBLE_host(uint32_t i) { return reinterpret_cast<PPCFunc*>(uintptr_t(0x10000) + uintptr_t(i) * 16); }

static uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Every registered address resolves to its own function; misaligned, zero, out-of-range and
// unregistered addresses resolve to nothing. Returns the number of wrong answers.
static int check_table(const std::vector<rcomp::FuncEntry>& table, uint32_t seed) {
    std::unordered_map<uint32_t, PPCFunc*> expected;
    for (const auto& e : table) expected[e.guest] = e.host;
    int wrong = 0;
    for (const auto& e : table) {
        if (rcomp::lookup_function(e.guest) != e.host) ++wrong;
        if (rcomp::lookup_function(e.guest + 1) || rcomp::lookup_function(e.guest + 2) ||
            rcomp::lookup_function(e.guest + 3))
            ++wrong;
    }
    if (rcomp::lookup_function(0)) ++wrong;
    if (rcomp::lookup_function(0xFFFFFFFCu)) ++wrong;
    uint32_t state = seed | 1u;
    for (int i = 0; i < 300000; ++i) {
        const uint32_t probe = next_random(state) & ~3u;
        const auto it = expected.find(probe);
        if (rcomp::lookup_function(probe) != (it == expected.end() ? nullptr : it->second)) ++wrong;
    }
    return wrong;
}

// rcomp::add_functions (rcomp/aot_modules.h): a secondary module's functions join the registered
// table; invalid input or an address registered already leaves the table unchanged.
static int test_add_functions() {
    int errors = 0;
    auto check = [&](bool value) { if (!value) ++errors; };
    std::vector<rcomp::FuncEntry> main_table;
    for (uint32_t i = 0; i < 1000; ++i) main_table.push_back({0x82000000u + 8 * i, TESTDOUBLE_host(i), nullptr});
    check(rcomp::register_functions(main_table.data(), main_table.size()));
    std::vector<rcomp::FuncEntry> module;
    for (uint32_t i = 0; i < 300; ++i)
        module.push_back({0x8A000000u + 12 * (299 - i), TESTDOUBLE_host(5000 + i), i == 0 ? "module_last" : nullptr});
    check(rcomp::add_functions(module.data(), module.size()));
    std::vector<rcomp::FuncEntry> all = main_table;
    all.insert(all.end(), module.begin(), module.end());
    check(check_table(all, 11) == 0);
    check(rcomp::function_name(0x8A000000u + 12 * 299) != nullptr);
    // Interleaved addresses (between existing ones) merge in order.
    const rcomp::FuncEntry between[] = {{0x82000004u, TESTDOUBLE_host(9001), nullptr},
                                        {0x81FFFFF0u, TESTDOUBLE_host(9002), nullptr}};
    check(rcomp::add_functions(between, 2));
    all.insert(all.end(), between, between + 2);
    check(check_table(all, 12) == 0);
    // Rejected: an address registered already, a duplicate inside the batch, invalid entries.
    const rcomp::FuncEntry again[] = {{0x8B000000u, TESTDOUBLE_host(9100), nullptr}, module[7]};
    check(!rcomp::add_functions(again, 2));
    check(!rcomp::lookup_function(0x8B000000u));
    const rcomp::FuncEntry twice[] = {{0x8C000000u, TESTDOUBLE_host(9200), nullptr},
                                      {0x8C000000u, TESTDOUBLE_host(9201), nullptr}};
    check(!rcomp::add_functions(twice, 2));
    check(!rcomp::lookup_function(0x8C000000u));
    for (auto bad : {rcomp::FuncEntry{0x8D000002u, TESTDOUBLE_host(1), nullptr},
                     rcomp::FuncEntry{0, TESTDOUBLE_host(1), nullptr},
                     rcomp::FuncEntry{0x8D000000u, nullptr, nullptr}})
        check(!rcomp::add_functions(&bad, 1));
    check(!rcomp::add_functions(nullptr, 1));
    check(!rcomp::add_functions(module.data(), SIZE_MAX / sizeof(rcomp::FuncEntry)));
    check(rcomp::add_functions(nullptr, 0));
    check(check_table(all, 13) == 0);
    // Without a registered table the batch becomes the table; register_functions still replaces all.
    rcomp::clear_functions();
    check(rcomp::add_functions(module.data(), module.size()));
    check(check_table(module, 14) == 0);
    check(rcomp::register_functions(main_table.data(), main_table.size()));
    check(check_table(main_table, 15) == 0);
    check(!rcomp::lookup_function(module[0].guest));
    rcomp::clear_functions();
    check(!rcomp::lookup_function(main_table[0].guest));
    return errors;
}

// Guest threads keep calling lookup_function while a module load adds functions: a registered
// function always resolves to itself, an added one to nothing until it is published and to
// itself afterwards, never to anything else.
static int test_add_functions_concurrent_readers() {
    std::vector<rcomp::FuncEntry> main_table;
    for (uint32_t i = 0; i < 60000; ++i) main_table.push_back({0x82000000u + 4 * i, TESTDOUBLE_host(i), nullptr});
    if (!rcomp::register_functions(main_table.data(), main_table.size())) return 1;
    constexpr uint32_t kBatches = 16, kBatch = 4000;
    std::vector<std::vector<rcomp::FuncEntry>> batches(kBatches);
    for (uint32_t b = 0; b < kBatches; ++b)
        for (uint32_t i = 0; i < kBatch; ++i)
            batches[b].push_back({0x88000000u + 0x100000u * b + 8 * i, TESTDOUBLE_host(100000 + b * kBatch + i), nullptr});
    std::atomic<bool> done{false};
    std::atomic<int> wrong{0};
    std::atomic<uint64_t> probes{0};
    std::vector<std::thread> readers;
    for (uint32_t t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            uint32_t state = 0x9E3779B9u * (t + 1);
            uint64_t n = 0;
            int bad = 0;
            while (!done.load(std::memory_order_acquire) || n < 200000) {
                const uint32_t r = next_random(state);
                const auto& e = main_table[r % main_table.size()];
                if (rcomp::lookup_function(e.guest) != e.host) ++bad;
                const auto& a = batches[(r >> 8) % kBatches][(r >> 12) % kBatch];
                PPCFunc* found = rcomp::lookup_function(a.guest);
                if (found && found != a.host) ++bad;
                if (rcomp::lookup_function(a.guest + 4)) ++bad;  // never registered
                ++n;
            }
            wrong += bad;
            probes += n;
        });
    }
    int errors = 0;
    for (const auto& batch : batches)
        if (!rcomp::add_functions(batch.data(), batch.size())) ++errors;
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    std::vector<rcomp::FuncEntry> all = main_table;
    for (const auto& batch : batches) all.insert(all.end(), batch.begin(), batch.end());
    errors += check_table(all, 21);
    rcomp::clear_functions();
    std::printf("func_table/add_functions_concurrent_readers probes=%llu\n", (unsigned long long)probes.load());
    return errors + wrong.load();
}

// rcomp/indirect_cache.h: the direct-mapped cache the generated code tries before rcomp_call_indirect (entries hold a host pointer
// below 4 GiB). Wrong answers here would call the wrong function, so every property is checked: empty entries never match (not
// even the zero address), an entry answers for its own guest address only, a colliding fill replaces the entry without mixing
// halves, null and 64-bit pointers are never cached, and publishing a function table clears the cache.
static int test_indirect_cache() {
    int errors = 0;
    auto check = [&](bool value) { if (!value) ++errors; };
    rcomp::indirect_cache_clear();
    check(!rcomp::indirect_cache_lookup(0));
    check(!rcomp::indirect_cache_lookup(0x82000000u));
    void* const low = reinterpret_cast<void*>(uintptr_t(0x400100));
    rcomp::indirect_cache_fill(0x82000000u, low);
    check(rcomp::indirect_cache_lookup(0x82000000u) == low);
    check(!rcomp::indirect_cache_lookup(0x82000004u));
    const uint32_t other = 0x82000000u + (uint32_t(rcomp::kIndirectCacheEntries) << 2);
    check(rcomp::indirect_cache_index(other) == rcomp::indirect_cache_index(0x82000000u));
    rcomp::indirect_cache_fill(other, reinterpret_cast<void*>(uintptr_t(0x400200)));
    check(!rcomp::indirect_cache_lookup(0x82000000u));
    check(rcomp::indirect_cache_lookup(other) == reinterpret_cast<void*>(uintptr_t(0x400200)));
    rcomp::indirect_cache_fill(0x82000008u, nullptr);
    check(!rcomp::indirect_cache_lookup(0x82000008u));
    if (sizeof(void*) == 8) {
        rcomp::indirect_cache_fill(0x8200000Cu, reinterpret_cast<void*>(uintptr_t(0x800000000ull)));
        check(!rcomp::indirect_cache_lookup(0x8200000Cu));
    }
    rcomp::indirect_cache_fill(0x82000010u, low);
    check(!rcomp::indirect_cache_lookup(0x82000011u) && !rcomp::indirect_cache_lookup(0x82000012u) &&
          !rcomp::indirect_cache_lookup(0x82000013u));
    // Every kind of publication clears it: a replaced function is never called through a stale entry.
    const rcomp::FuncEntry first[] = {{0x82000000u, TESTDOUBLE_host(1), nullptr}};
    check(rcomp::register_functions(first, 1));
    rcomp::indirect_cache_fill(0x82000000u, low);
    const rcomp::FuncEntry second[] = {{0x82000000u, TESTDOUBLE_host(2), nullptr}};
    check(rcomp::register_functions(second, 1));
    check(!rcomp::indirect_cache_lookup(0x82000000u));
    check(rcomp::lookup_function(0x82000000u) == TESTDOUBLE_host(2));
    rcomp::indirect_cache_fill(0x82000000u, low);
    const rcomp::FuncEntry added[] = {{0x82000100u, TESTDOUBLE_host(3), nullptr}};
    check(rcomp::add_functions(added, 1));
    check(!rcomp::indirect_cache_lookup(0x82000000u));
    check(rcomp::lookup_function(0x82000100u) == TESTDOUBLE_host(3));
    rcomp::indirect_cache_fill(0x82000000u, low);
    rcomp::clear_functions();
    check(!rcomp::indirect_cache_lookup(0x82000000u));
    check(!rcomp::lookup_function(0x82000000u));
    return errors;
}

int main() {
    int errors = 0;
    const rcomp::FuncEntry good[] = {{0x82000004, TESTDOUBLE_entry, "entry"},
                                   {0x82000000, TESTDOUBLE_entry, "start"}};
    auto check = [&](bool value) { if (!value) ++errors; };
    check(rcomp::register_functions(good, 2));
    check(rcomp::lookup_function(0x82000000) == TESTDOUBLE_entry);
    check(!rcomp::register_functions(nullptr, 1));
    check(!rcomp::register_functions(good, SIZE_MAX / sizeof(rcomp::FuncEntry) + 1));
    for (auto bad : {rcomp::FuncEntry{0x82000001, TESTDOUBLE_entry, nullptr},
                     rcomp::FuncEntry{0, TESTDOUBLE_entry, nullptr},
                     rcomp::FuncEntry{0x82000000, nullptr, nullptr}}) {
        check(!rcomp::register_functions(&bad, 1));
        check(rcomp::lookup_function(0x82000004) == TESTDOUBLE_entry);
    }
    const rcomp::FuncEntry duplicate[] = {good[0], good[0]};
    check(!rcomp::register_functions(duplicate, 2));
    check(rcomp::lookup_function(0x82000000) == TESTDOUBLE_entry);
    rcomp::clear_functions();
    check(!rcomp::lookup_function(0x82000000));
    check(rcomp::register_functions(nullptr, 0));
    check(!rcomp::lookup_function(0x82000000));
    std::printf("func_table/transactional_validation %s errors=%d\n", errors ? "FAIL" : "PASS", errors);

    // Lookups over tables of the sizes and shapes a title registers (the GTA IV image has
    // 55,695 functions): dense consecutive starts, a page stride that a plain mask would fold
    // onto a few slots, scattered starts, and the smallest tables (probe wrap-around).
    int shape_errors = 0;
    std::vector<rcomp::FuncEntry> table;
    for (uint32_t i = 0; i < 60000; ++i) table.push_back({0x82000000u + 4 * i, TESTDOUBLE_host(i), nullptr});
    check(rcomp::register_functions(table.data(), table.size()));
    shape_errors += check_table(table, 1);
    table.clear();
    for (uint32_t i = 0; i < 4000; ++i) table.push_back({0x80000000u + 0x10000u * i, TESTDOUBLE_host(i), nullptr});
    check(rcomp::register_functions(table.data(), table.size()));
    shape_errors += check_table(table, 2);
    table.clear();
    {
        std::unordered_map<uint32_t, bool> seen;
        uint32_t state = 12345;
        while (table.size() < 55695) {
            const uint32_t guest = 0x82000000u + ((next_random(state) % 0x900000u) & ~3u);
            if (seen.emplace(guest, true).second) table.push_back({guest, TESTDOUBLE_host(uint32_t(table.size())), nullptr});
        }
    }
    check(rcomp::register_functions(table.data(), table.size()));
    shape_errors += check_table(table, 3);
    for (size_t count : {size_t(1), size_t(2), size_t(7), size_t(8), size_t(9), size_t(33)}) {
        table.clear();
        for (uint32_t i = 0; i < count; ++i) table.push_back({0xFFFFFF00u - 0x40u * i, TESTDOUBLE_host(i), nullptr});
        check(rcomp::register_functions(table.data(), table.size()));
        shape_errors += check_table(table, 4 + uint32_t(count));
    }
    // A new registration replaces the old table completely.
    table.assign({{0x83000000u, TESTDOUBLE_host(1), "replacement"}});
    check(rcomp::register_functions(table.data(), table.size()));
    check(rcomp::lookup_function(0x83000000u) == TESTDOUBLE_host(1));
    check(!rcomp::lookup_function(0xFFFFFF00u));
    check(!rcomp::lookup_function(0x82000000u));
    check(rcomp::function_name(0x83000000u) != nullptr);
    rcomp::clear_functions();
    check(!rcomp::lookup_function(0x83000000u));
    errors += shape_errors;
    std::printf("func_table/lookup_shapes %s wrong_answers=%d\n", shape_errors ? "FAIL" : "PASS", shape_errors);
    const int add_errors = test_add_functions();
    std::printf("func_table/add_functions %s errors=%d\n", add_errors ? "FAIL" : "PASS", add_errors);
    const int concurrent_errors = test_add_functions_concurrent_readers();
    std::printf("func_table/add_functions_concurrent_readers %s wrong_answers=%d\n",
                concurrent_errors ? "FAIL" : "PASS", concurrent_errors);
    errors += add_errors + concurrent_errors;
    const int cache_errors = test_indirect_cache();
    std::printf("func_table/indirect_cache %s errors=%d\n", cache_errors ? "FAIL" : "PASS", cache_errors);
    errors += cache_errors;
    std::printf("func_table/all %s errors=%d\n", errors ? "FAIL" : "PASS", errors);
    return errors ? 1 : 0;
}
