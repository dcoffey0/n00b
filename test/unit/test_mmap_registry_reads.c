// Address lookups through the mmap registry must give a correct answer when a
// collection on another thread runs between the registry lookup and the
// caller's read of its result. Every collection unregisters the from-space
// segments and frees their records, so no caller may read a record after the
// lookup releases the registry lock.
//
// Each entry point runs on a helper thread. A debug hook holds that thread
// just after the registry lookup returns, the main thread collects with freed
// records poisoned, and the helper then finishes the call.

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/gc.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/thread.h"

#if defined(N00B_DEBUG)

typedef struct {
    uint64_t tag;
    uint64_t words[30];
} probe_obj_t;

typedef enum {
    ENTRY_FIND_ALLOC_INFO,
    ENTRY_FIND_ALLOC_INFO_INTERIOR,
    ENTRY_MEM_GET_ALLOCATOR,
    ENTRY_COUNT,
} entry_t;

static const char *entry_names[ENTRY_COUNT] = {
    "n00b_find_alloc_info",
    "n00b_find_alloc_info(.scan_for_header) on an interior pointer",
    "n00b_mem_get_allocator",
};

typedef struct {
    entry_t           entry;
    probe_obj_t      *obj;
    void *_Atomic     armed;
    _Atomic bool      held;
    _Atomic bool      collected;
    bool              resolved;
} race_t;

static race_t *race;

static void
hold_after_lookup(void *addr)
{
    void *expected = addr;
    if (addr == nullptr
        || !atomic_compare_exchange_strong(&race->armed, &expected, nullptr)) {
        return;
    }
    atomic_store(&race->held, true);
    while (!atomic_load(&race->collected)) {
        __asm__ volatile("" ::: "memory");
    }
}

static void *
race_fn(void *arg)
{
    race_t           *r   = arg;
    probe_obj_t      *obj = r->obj;
    n00b_allocator_t *arena
        = (n00b_allocator_t *)n00b_get_runtime()->default_arena;

    switch (r->entry) {
    case ENTRY_FIND_ALLOC_INFO: {
        bool pin    = false;
        r->resolved = n00b_alloc_info_is_heap(n00b_find_alloc_info(obj, .pin = &pin));
        n00b_alloc_hdr_unpin(pin);
        break;
    }
    case ENTRY_FIND_ALLOC_INFO_INTERIOR: {
        bool pin    = false;
        r->resolved = n00b_alloc_info_is_heap(
            n00b_find_alloc_info(&obj->words[7], .scan_for_header = true, .pin = &pin));
        n00b_alloc_hdr_unpin(pin);
        break;
    }
    case ENTRY_MEM_GET_ALLOCATOR: {
        n00b_allocator_opt_t a = n00b_mem_get_allocator(obj);
        r->resolved = n00b_option_is_set(a) && n00b_option_get(a) == arena;
        break;
    }
    default:
        break;
    }
    return nullptr;
}

static void
run_entry(entry_t entry)
{
    probe_obj_t *obj = n00b_alloc(probe_obj_t);
    obj->tag         = 0x5eed0000u + entry;
    void *interior   = &obj->words[7];

    race_t r = {.entry = entry, .obj = obj};
    atomic_store(&r.armed, entry == ENTRY_FIND_ALLOC_INFO_INTERIOR ? interior : (void *)obj);
    race                                 = &r;
    n00b_mmap_debug_poison_freed_records = true;
    n00b_mmap_debug_lookup_hook          = hold_after_lookup;

    auto sp = n00b_thread_spawn(race_fn, &r);
    assert(n00b_result_is_ok(sp));
    while (!atomic_load(&r.held)) {
        __asm__ volatile("" ::: "memory");
    }
    n00b_collect(n00b_get_runtime()->default_arena);
    atomic_store(&r.collected, true);
    n00b_thread_join(n00b_result_get(sp));
    n00b_mmap_debug_lookup_hook          = nullptr;
    n00b_mmap_debug_poison_freed_records = false;

    printf("  %s across a collection: %s\n",
           entry_names[entry],
           r.resolved ? "resolved" : "NOT resolved");
    assert(obj->tag == 0x5eed0000u + entry);
    assert(r.resolved);
}

#endif

int
main(int argc, char **argv)
{
    // Objects stay where they are, so a correct lookup resolves the same
    // object after the collection as before it.
    setenv("N00B_GC_PIN_ALL", "1", 1);

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

#if defined(N00B_DEBUG)
    int only = argc > 1 ? atoi(argv[1]) : -1;
    for (int e = 0; e < ENTRY_COUNT; e++) {
        if (only < 0 || only == e) {
            run_entry((entry_t)e);
        }
    }
    printf("  [PASS] registry lookups across a collection\n");
#else
    printf("  [SKIP] needs N00B_DEBUG for the lookup hook\n");
#endif

    n00b_shutdown();
    return 0;
}
