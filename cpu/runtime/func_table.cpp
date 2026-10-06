// Guest-address -> host-function registry. Plain C allocation and qsort so it
// links into the PS5 title, which carries no C++ standard library runtime.
//
// Concurrency: register_functions and clear_functions replace the whole table and run before guest
// threads start (they free the previous table at once). add_functions (rcomp/aot_modules.h) runs
// while guest threads call lookup_function: it builds a new sorted array and a new index next to
// the published ones, then publishes each with one atomic release store. Readers load the
// published word once (acquire) and never see a partly built table; they take no lock. The
// replaced array and index stay allocated (a reader may still be probing them) until the next
// register_functions or clear_functions, which require quiescence anyway. Writers are serialized
// by a spin lock (module loads are rare and short).
#include "rcomp/aot_modules.h"
#include "rcomp/func_table.h"
#include "rcomp/indirect_cache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace rcomp {

namespace {
// Index for the hot path: every indirect call of the recompiled code looks its target up
// here. Open addressing with linear probing over 16-byte slots {guest, host}. The table has at
// least twice as many slots as functions, so a probe always ends at an empty slot (guest == 0,
// which no registered function has). The word index of the address is scrambled with the
// golden-ratio multiplier and its top bits pick the slot: function starts are 4-byte aligned
// and clustered, which a plain mask would fold onto few slots. A binary search over the sorted
// array took about 16 dependent loads per call; a hit here is usually one cache line.
struct Slot {
    uint32_t guest;
    uint32_t pad;
    PPCFunc* host;
};
// The published index is one word: the 64-byte aligned slot array with log2(slot count) in its
// low 6 bits, so a lookup still makes a single load before the slot it probes.
constexpr uintptr_t kIndexAlign = 64;
uintptr_t g_index_word = 0;  // 0: no index; read with __atomic_load_n (acquire)

// Immutable sorted array (names and diagnostics), published like the index.
struct Sorted {
    size_t count;
    FuncEntry v[1];  // count entries (at least one allocated)
};
Sorted* g_sorted = nullptr;

// Writer-side ownership of the published allocations, and the replaced ones still readable.
struct Owned {
    void* index_raw = nullptr;
    Sorted* sorted = nullptr;
};
Owned g_owned;
struct Retired {
    Retired* next;
    void* allocation;
};
Retired* g_retired = nullptr;
bool g_writer = false;  // spin lock of the writers

void lock_writers() {
    while (__atomic_test_and_set(&g_writer, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&g_writer, __ATOMIC_RELAXED)) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
}
void unlock_writers() { __atomic_clear(&g_writer, __ATOMIC_RELEASE); }

inline uint32_t first_slot(uint32_t guest, uint32_t shift) { return ((guest >> 2) * 0x9E3779B1u) >> shift; }

int by_guest(const void* a, const void* b) {
    uint32_t x = ((const FuncEntry*)a)->guest, y = ((const FuncEntry*)b)->guest;
    return x < y ? -1 : x > y ? 1 : 0;
}

bool valid_entries(const FuncEntry* entries, size_t count) {
    if ((count && !entries) || count > (SIZE_MAX - sizeof(Sorted)) / sizeof(FuncEntry)) return false;
    for (size_t i = 0; i < count; ++i)
        if (!entries[i].guest || (entries[i].guest & 3u) || !entries[i].host) return false;
    return true;
}

Sorted* new_sorted(size_t count) {
    Sorted* s = (Sorted*)malloc(sizeof(Sorted) + (count ? count - 1 : 0) * sizeof(FuncEntry));
    if (s) s->count = count;
    return s;
}

bool has_duplicate(const Sorted* s) {
    for (size_t i = 1; i < s->count; ++i)
        if (s->v[i].guest == s->v[i - 1].guest) return true;
    return false;
}

// Indexes v[0..count) into *word / *raw; an empty index (word 0) for count == 0. False on
// allocation failure.
bool build_index(const FuncEntry* v, size_t count, uintptr_t* word, void** raw) {
    *word = 0;
    *raw = nullptr;
    if (!count) return true;
    uint32_t bits = 4;
    while (bits < 31 && (size_t(1) << bits) < 2 * count) ++bits;
    if ((size_t(1) << bits) < 2 * count) return false;
    const size_t size = size_t(1) << bits;
    void* allocation = calloc(size * sizeof(Slot) + kIndexAlign, 1);
    if (!allocation) return false;
    Slot* slots = (Slot*)(((uintptr_t)allocation + kIndexAlign - 1) & ~(kIndexAlign - 1));
    const uint32_t shift = 32 - bits, mask = uint32_t(size - 1);
    for (size_t i = 0; i < count; ++i) {
        uint32_t at = first_slot(v[i].guest, shift);
        while (slots[at].guest) at = (at + 1) & mask;
        slots[at].guest = v[i].guest;
        slots[at].host = v[i].host;
    }
    *word = (uintptr_t)slots | bits;
    *raw = allocation;
    return true;
}

void free_retired() {
    while (g_retired) {
        Retired* next = g_retired->next;
        free(g_retired->allocation);
        free(g_retired);
        g_retired = next;
    }
}

// Publishes a new table (writer lock held). `retire` keeps the previous allocations readable.
void publish(Sorted* sorted, uintptr_t word, void* raw, bool retire, Retired* nodes) {
    __atomic_store_n(&g_sorted, sorted, __ATOMIC_RELEASE);
    __atomic_store_n(&g_index_word, word, __ATOMIC_RELEASE);
    // The indirect-call cache (rcomp/indirect_cache.h) holds entries of the previous table: a replaced or removed function must
    // not stay callable through it. Entries filled after this point come from the table published above.
    indirect_cache_clear();
    void* previous[2] = {g_owned.sorted, g_owned.index_raw};
    g_owned.sorted = sorted;
    g_owned.index_raw = raw;
    for (void* p : previous) {
        if (!p) continue;
        if (retire) {
            Retired* node = nodes;
            nodes = nodes->next;
            node->allocation = p;
            node->next = g_retired;
            g_retired = node;
        } else {
            free(p);
        }
    }
    while (nodes) {  // unused retirement nodes
        Retired* next = nodes->next;
        free(nodes);
        nodes = next;
    }
}
}  // namespace

bool register_functions(const FuncEntry* entries, size_t count) {
    if (!valid_entries(entries, count)) return false;
    Sorted* v = new_sorted(count);
    if (!v) return false;
    if (count) memcpy(v->v, entries, count * sizeof(FuncEntry));
    if (count > 1) qsort(v->v, count, sizeof(FuncEntry), by_guest);
    uintptr_t word;
    void* raw;
    if (has_duplicate(v) || !build_index(v->v, count, &word, &raw)) {
        free(v);
        return false;
    }
    // Nothing above changed the registered table: a failure leaves it as it was.
    lock_writers();
    publish(v, word, raw, false, nullptr);
    free_retired();
    unlock_writers();
    return true;
}

bool add_functions(const FuncEntry* entries, size_t count) {
    if (!valid_entries(entries, count)) return false;
    if (!count) return true;
    Sorted* added = new_sorted(count);
    if (!added) return false;
    memcpy(added->v, entries, count * sizeof(FuncEntry));
    if (count > 1) qsort(added->v, count, sizeof(FuncEntry), by_guest);
    if (has_duplicate(added)) {
        free(added);
        return false;
    }
    // Two retirement nodes allocated up front: publishing cannot fail half way.
    Retired* nodes = (Retired*)malloc(sizeof(Retired));
    if (nodes) {
        nodes->next = (Retired*)malloc(sizeof(Retired));
        if (nodes->next) nodes->next->next = nullptr;
    }
    if (!nodes || !nodes->next) {
        free(nodes);
        free(added);
        return false;
    }
    lock_writers();
    const Sorted* old = g_owned.sorted;
    const size_t old_count = old ? old->count : 0;
    bool ok = old_count <= (SIZE_MAX - sizeof(Sorted)) / sizeof(FuncEntry) - count;
    Sorted* merged = ok ? new_sorted(old_count + count) : nullptr;
    ok = merged != nullptr;
    if (ok) {
        size_t i = 0, j = 0, k = 0;
        while (i < old_count || j < count) {
            if (j == count || (i < old_count && old->v[i].guest < added->v[j].guest)) {
                merged->v[k++] = old->v[i++];
            } else if (i == old_count || added->v[j].guest < old->v[i].guest) {
                merged->v[k++] = added->v[j++];
            } else {
                ok = false;  // the address is registered already
                break;
            }
        }
    }
    uintptr_t word = 0;
    void* raw = nullptr;
    ok = ok && build_index(merged->v, merged->count, &word, &raw);
    if (ok) {
        publish(merged, word, raw, true, nodes);
    } else {
        free(merged);
        free(nodes->next);
        free(nodes);
    }
    unlock_writers();
    free(added);
    return ok;
}

static const FuncEntry* find(uint32_t guest) {
    const Sorted* s = __atomic_load_n(&g_sorted, __ATOMIC_ACQUIRE);
    if (!s) return nullptr;
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->v[mid].guest < guest) lo = mid + 1;
        else hi = mid;
    }
    return (lo < s->count && s->v[lo].guest == guest) ? &s->v[lo] : nullptr;
}

PPCFunc* lookup_function(uint32_t guest) {
    const uintptr_t word = __atomic_load_n(&g_index_word, __ATOMIC_ACQUIRE);
    if (!word) return nullptr;
    const Slot* slots = (const Slot*)(word & ~(kIndexAlign - 1));
    const uint32_t bits = uint32_t(word & (kIndexAlign - 1));
    const uint32_t shift = 32 - bits, mask = (1u << bits) - 1;
    for (uint32_t at = first_slot(guest, shift);; at = (at + 1) & mask) {
        const Slot& slot = slots[at];
        if (slot.guest == guest) return slot.host;  // an empty slot for guest == 0: null host
        if (!slot.guest) return nullptr;
    }
}

const char* function_name(uint32_t guest) {
    const FuncEntry* e = find(guest);
    return e ? e->name : nullptr;
}

void clear_functions() {
    lock_writers();
    publish(nullptr, 0, nullptr, false, nullptr);
    free_retired();
    unlock_writers();
}

}  // namespace rcomp
