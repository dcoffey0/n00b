/*
 * test_gc_interior_scan_linear.c -- n00b#539.
 *
 * Every scanned word that lands inside a heap object, but not at its base, is
 * resolved to its allocation by the backward guard scan in _find_sentinal
 * (alloc.c), from the pointer down to the object's header. Walked one word at
 * a time for each pointer, an object of S bytes holding pointers into itself
 * costs O(S^2) per collection, and so does an array of slices into one large
 * buffer. The heaps are a GC arena, which the collection moves or pins, and
 * user_pool, which it traces through.
 *
 * The check is counted work, not time: n00b_sentinel_scan_words over one
 * collection. Each heap is built at two object sizes, and the words walked
 * that the larger heap adds are bounded per heap word it adds. A linear mark
 * stays at a small constant there, whatever the object size; the per-word walk
 * is in the thousands. Taking the difference cancels the walks every
 * collection makes outside the heap under test.
 *
 * Registered twice, with N00B_GC_PIN_ALL=1 and =0, because the pin-all and
 * copying collectors reach the resolution through different paths after it
 * (pin in place vs. forward and rewrite), and both must leave every interior
 * pointer pointing into its own object.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/stw.h"

#define N_SELF        8
#define N_SLICES      4096
#define SLOT_STRIDE   8
#define ADDR_MASK     ((uintptr_t)0x5a5a5a5a5a5a5a5aULL)

// Words a linear mark may walk per heap word. The memoized walk stays under 3;
// the unbounded walk is above 1000 at the sizes below.
#define WALK_PER_HEAP_WORD 16

static int failures = 0;

static void
fail(const char *what)
{
    printf("  [FAIL] %s\n", what);
    failures++;
}

static void
pass(const char *what)
{
    printf("  [PASS] %s\n", what);
}

// Rooted from static data so liveness does not depend on the stack scan.
static void ***g_self;
static void  **g_slices;
static char   *g_blob;

#define ALLOC_OPTS(a) &(n00b_alloc_opts_t){.allocator = (a)}

// Where the heap under test lives. An arena row is the from-space of the
// collection; a pool row is traced through by a collection of an empty arena.
typedef struct {
    const char *name;
    bool        pool;
} heap_kind_t;

static const heap_kind_t heap_kinds[] = {
    {"gc arena", false},
    {"user_pool", true},
};

// N_SELF arrays of `words` slots, every SLOT_STRIDE-th slot pointing at itself.
static void
build_self(n00b_allocator_t *al, uint64_t words)
{
    g_self = n00b_alloc_array(void **, N_SELF);

    for (uint64_t i = 0; i < N_SELF; i++) {
        void **obj = n00b_alloc_array_with_opts(void *, words, ALLOC_OPTS(al));
        for (uint64_t k = SLOT_STRIDE; k < words; k += SLOT_STRIDE) {
            obj[k] = (void *)&obj[k];
        }
        g_self[i] = obj;
    }
}

// N_SLICES pointers, spread evenly over a blob of `bytes`.
static void
build_slices(n00b_allocator_t *al, uint64_t bytes)
{
    g_blob   = n00b_alloc_array_with_opts(char,
                                        bytes,
                                        &(n00b_alloc_opts_t){
                                              .allocator = al,
                                              .no_scan   = true,
                                        });
    g_slices = n00b_alloc_array_with_opts(void *, N_SLICES, ALLOC_OPTS(al));

    for (uint64_t j = 0; j < N_SLICES; j++) {
        g_slices[j] = g_blob + 8 + (j * (bytes - 16)) / N_SLICES;
    }
}

static bool
self_intact(uint64_t words)
{
    for (uint64_t i = 0; i < N_SELF; i++) {
        void **obj = g_self[i];
        for (uint64_t k = SLOT_STRIDE; k < words; k += SLOT_STRIDE) {
            if (obj[k] != (void *)&obj[k]) {
                return false;
            }
        }
    }
    return true;
}

static bool
slices_intact(uint64_t bytes)
{
    for (uint64_t j = 0; j < N_SLICES; j++) {
        if ((char *)g_slices[j] != g_blob + 8 + (j * (bytes - 16)) / N_SLICES) {
            return false;
        }
    }
    return true;
}

// Words the guard scan walked during one collection of `arena`.
static uint64_t
walked_by_collect(n00b_arena_t *arena)
{
    uint64_t before = atomic_load(&n00b_sentinel_scan_words);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    return atomic_load(&n00b_sentinel_scan_words) - before;
}

typedef struct {
    uint64_t heap_words; // scanned words plus the words of what they point into
    uint64_t walked;
} cost_t;

// The allocator a row builds its heap in, and the arena its collection runs on.
static n00b_allocator_t *
row_heap(heap_kind_t kind, uint64_t bytes, n00b_arena_t **collected)
{
    if (kind.pool) {
        *collected = n00b_new_arena(.size = 1 << 24, .use_gc = true);
        return (n00b_allocator_t *)&n00b_get_runtime()->user_pool;
    }
    *collected = n00b_new_arena(.size = bytes * 4 + (1 << 24), .use_gc = true);
    return (n00b_allocator_t *)*collected;
}

static cost_t
run_self(heap_kind_t kind, uint64_t words)
{
    n00b_arena_t     *arena;
    n00b_allocator_t *al = row_heap(kind, N_SELF * words * 8, &arena);
    build_self(al, words);

    // Masked so the conservative scan does not rewrite the snapshot.
    volatile uintptr_t before = (uintptr_t)g_self[0] ^ ADDR_MASK;

    cost_t c = {.heap_words = N_SELF * words, .walked = walked_by_collect(arena)};

    printf("  ... %llu-word arrays %s across the collection\n",
           (unsigned long long)words,
           ((uintptr_t)g_self[0] ^ ADDR_MASK) == before ? "stayed in place"
                                                         : "were moved");

    if (!self_intact(words)) {
        fail("a self-referencing array lost an interior pointer across the"
             " collection");
    }
    g_self = nullptr;
    return c;
}

static cost_t
run_slices(heap_kind_t kind, uint64_t bytes)
{
    n00b_arena_t     *arena;
    n00b_allocator_t *al = row_heap(kind, bytes, &arena);
    build_slices(al, bytes);

    cost_t c = {.heap_words = N_SLICES + bytes / sizeof(void *),
                .walked     = walked_by_collect(arena)};

    if (!slices_intact(bytes)) {
        fail("a slice stopped pointing at its offset in the blob across the"
             " collection");
    }
    g_slices = nullptr;
    g_blob   = nullptr;
    return c;
}

static void
check(const char *shape, cost_t small, cost_t big)
{
    uint64_t added_words  = big.heap_words - small.heap_words;
    uint64_t added_walked = big.walked > small.walked ? big.walked - small.walked : 0;
    double   per_word     = (double)added_walked / (double)added_words;

    printf("  ... %s: %llu words walked over %llu heap words, then %llu over"
           " %llu: %.2f walked per added heap word\n",
           shape,
           (unsigned long long)small.walked,
           (unsigned long long)small.heap_words,
           (unsigned long long)big.walked,
           (unsigned long long)big.heap_words,
           per_word);

    char msg[256];

    if (per_word > WALK_PER_HEAP_WORD) {
        snprintf(msg,
                 sizeof msg,
                 "%s: the guard scan walked %.1f words per added heap word,"
                 " over the linear bound of %d",
                 shape,
                 per_word,
                 WALK_PER_HEAP_WORD);
        fail(msg);
        return;
    }
    snprintf(msg,
             sizeof msg,
             "%s: interior-pointer resolution stays within %d words walked"
             " per heap word",
             shape,
             WALK_PER_HEAP_WORD);
    pass(msg);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#539: interior-pointer resolution is linear in heap words"
           " (pin-all: %s)\n",
           n00b_gc_pin_all_policy() ? "on" : "off");

    for (size_t h = 0; h < sizeof heap_kinds / sizeof heap_kinds[0]; h++) {
        heap_kind_t kind = heap_kinds[h];
        char        shape[96];

        // 32 KB and 128 KB arrays: 4K and 16K words, a pointer every 8.
        cost_t self_small = run_self(kind, 4096);
        cost_t self_big   = run_self(kind, 16384);
        snprintf(shape, sizeof shape, "%s, self-referencing arrays", kind.name);
        check(shape, self_small, self_big);

        // 4K slices into a 64 KB blob, then into a 256 KB one.
        cost_t sl_small = run_slices(kind, 64 * 1024);
        cost_t sl_big   = run_slices(kind, 256 * 1024);
        snprintf(shape, sizeof shape, "%s, slices into one blob", kind.name);
        check(shape, sl_small, sl_big);
    }

    n00b_shutdown();
    return failures ? 1 : 0;
}
