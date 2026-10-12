/*
 * test_marshal_interior_scan_linear.c -- n00b#539.
 *
 * scan_node (marshal.c) resolves every pointer word to the graph node it
 * points into. Resolved by walking back to its object's guard, an interior
 * pointer costs its offset into the object, so marshaling an object of S bytes
 * that holds pointers into itself costs O(S^2), and so does an array of slices
 * into one large buffer. Each such walk starts with a search of the global
 * mmap registry.
 *
 * The checks are counted work, not time, over one marshal: words the guard
 * scan walked (n00b_sentinel_scan_words) and, in debug builds, registry
 * searches (n00b_mmap_point_searches). Each graph is marshaled at two sizes,
 * and the work the larger graph adds is bounded per heap word or pointer it
 * adds, which cancels the marshal's fixed costs. Every image is unmarshaled
 * and each pointer checked, so a bound that skipped resolution cannot pass.
 *
 * The table crosses the heaps a marshaled graph lives in (a GC arena,
 * user_pool, and a hidden inline-header pool like the rocs hot shard) with
 * both marshal modes: the default stop-the-world, and N00B_MARSHAL_F_NO_STW,
 * which rocs seal uses while other threads run.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "util/marshal.h"

#define N_SELF      8
#define N_SLICES    4096
#define SLOT_STRIDE 8

// Words the guard scan may walk per added heap word. A linear marshal walks
// well under one; the per-pointer walk is above 1000 at the sizes below.
#define WALK_PER_HEAP_WORD 16

// Registry searches the marshal may make per added pointer. Resolving a word
// inside a node the marshal already holds makes none.
#define SEARCHES_PER_POINTER 0.05

#if defined(N00B_DEBUG)
extern _Atomic(uint64_t) n00b_mmap_point_searches;
#define REGISTRY_SEARCHES() atomic_load(&n00b_mmap_point_searches)
#else
#define REGISTRY_SEARCHES() UINT64_C(0)
#endif

typedef enum {
    HEAP_ARENA,
    HEAP_USER_POOL,
    HEAP_HIDDEN_POOL,
} heap_t;

static const char *heap_names[] = {"arena", "user_pool", "hidden inline pool"};

static int         failures = 0;
// Behind a pointer so neither the GC root scan nor a copying collection
// touches the pool header.
static n00b_pool_t *hidden_pool;

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

#define ALLOC_OPTS(a) &(n00b_alloc_opts_t){.allocator = (a)}

typedef struct {
    uint64_t heap_words; // words of every object in the graph
    uint64_t walked;
    uint64_t searches;
} cost_t;

static n00b_allocator_t *
row_heap(heap_t heap, uint64_t bytes)
{
    switch (heap) {
    case HEAP_USER_POOL:
        return (n00b_allocator_t *)&n00b_get_runtime()->user_pool;
    case HEAP_HIDDEN_POOL:
        return (n00b_allocator_t *)hidden_pool;
    default:
        return (n00b_allocator_t *)n00b_new_arena(.size = bytes * 2 + (1 << 24));
    }
}

static n00b_buffer_t *
marshal_counted(void *root, bool no_stw, cost_t *c)
{
    uint64_t walked   = atomic_load(&n00b_sentinel_scan_words);
    uint64_t searches = REGISTRY_SEARCHES();

    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(
        .flags = no_stw ? N00B_MARSHAL_F_NO_STW : 0);
    n00b_buffer_t *image = n00b_marshal_incremental(ctx, root, .close = true);
    n00b_marshal_ctx_destroy(ctx);

    c->walked   = atomic_load(&n00b_sentinel_scan_words) - walked;
    c->searches = REGISTRY_SEARCHES() - searches;
    return image;
}

// N_SELF arrays of `words` slots, every SLOT_STRIDE-th slot pointing at itself,
// reached from one root array.
static cost_t
run_self(heap_t heap, bool no_stw, uint64_t words)
{
    n00b_allocator_t *al   = row_heap(heap, N_SELF * words * 8);
    void           ***root = n00b_alloc_array_with_opts(void **, N_SELF, ALLOC_OPTS(al));

    for (uint64_t i = 0; i < N_SELF; i++) {
        void **obj = n00b_alloc_array_with_opts(void *, words, ALLOC_OPTS(al));
        for (uint64_t k = SLOT_STRIDE; k < words; k += SLOT_STRIDE) {
            obj[k] = (void *)&obj[k];
        }
        root[i] = obj;
    }

    cost_t         c     = {.heap_words = N_SELF + N_SELF * words};
    n00b_buffer_t *image = marshal_counted(root, no_stw, &c);

    if (image == nullptr) {
        fail("self-referencing arrays did not marshal");
        return c;
    }

    void ***back = n00b_unmarshal_one(image);
    bool    ok   = back != nullptr;

    for (uint64_t i = 0; ok && i < N_SELF; i++) {
        void **obj = back[i];
        for (uint64_t k = SLOT_STRIDE; k < words; k += SLOT_STRIDE) {
            if (obj[k] != (void *)&obj[k]) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        fail("an unmarshaled array slot does not point at itself");
    }
    return c;
}

// N_SLICES pointers spread evenly over an unscanned blob of `bytes`.
static cost_t
run_slices(heap_t heap, bool no_stw, uint64_t bytes)
{
    n00b_allocator_t *al     = row_heap(heap, bytes);
    char             *blob   = n00b_alloc_array_with_opts(char,
                                                  bytes,
                                                  &(n00b_alloc_opts_t){
                                                      .allocator = al,
                                                      .no_scan   = true,
                                                  });
    void            **slices = n00b_alloc_array_with_opts(void *, N_SLICES, ALLOC_OPTS(al));

    for (uint64_t j = 0; j < N_SLICES; j++) {
        slices[j] = blob + 8 + (j * (bytes - 16)) / N_SLICES;
    }

    cost_t         c     = {.heap_words = N_SLICES + bytes / sizeof(void *)};
    n00b_buffer_t *image = marshal_counted(slices, no_stw, &c);

    if (image == nullptr) {
        fail("slices into a blob did not marshal");
        return c;
    }

    char **back = n00b_unmarshal_one(image);
    bool   ok   = back != nullptr;

    for (uint64_t j = 0; ok && j < N_SLICES; j++) {
        if (back[j] - (back[0] - 8) != (int64_t)(8 + (j * (bytes - 16)) / N_SLICES)) {
            ok = false;
        }
    }
    if (!ok) {
        fail("an unmarshaled slice is not at its offset in the blob");
    }
    return c;
}

static void
check_walk(const char *shape, cost_t small, cost_t big)
{
    uint64_t added_words  = big.heap_words - small.heap_words;
    uint64_t added_walked = big.walked > small.walked ? big.walked - small.walked : 0;
    double   per_word     = (double)added_walked / (double)added_words;
    char     msg[256];

    printf("  ... %s: %llu words walked over %llu heap words, then %llu over"
           " %llu: %.2f walked per added heap word\n",
           shape,
           (unsigned long long)small.walked,
           (unsigned long long)small.heap_words,
           (unsigned long long)big.walked,
           (unsigned long long)big.heap_words,
           per_word);

    snprintf(msg,
             sizeof msg,
             "%s: the marshal walked %.1f words per added heap word (bound %d)",
             shape,
             per_word,
             WALK_PER_HEAP_WORD);
    if (per_word > WALK_PER_HEAP_WORD) {
        fail(msg);
        return;
    }
    pass(msg);
}

// Registry searches per pointer the larger graph adds. Counted only in debug
// builds, where n00b_mmap_point_searches exists.
static void
check_searches(const char *shape, cost_t small, cost_t big, uint64_t added_pointers)
{
#if defined(N00B_DEBUG)
    uint64_t added    = big.searches > small.searches ? big.searches - small.searches : 0;
    double   per_word = (double)added / (double)added_pointers;
    char     msg[256];

    printf("  ... %s: %llu registry searches, then %llu: %.3f per added pointer\n",
           shape,
           (unsigned long long)small.searches,
           (unsigned long long)big.searches,
           per_word);
    snprintf(msg,
             sizeof msg,
             "%s: %.3f registry searches per added pointer (bound %.2f)",
             shape,
             per_word,
             SEARCHES_PER_POINTER);
    if (per_word > SEARCHES_PER_POINTER) {
        fail(msg);
        return;
    }
    pass(msg);
#else
    (void)shape;
    (void)small;
    (void)big;
    (void)added_pointers;
#endif
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    hidden_pool = calloc(1, sizeof(*hidden_pool));
    n00b_pool_init(hidden_pool,
                   .hidden            = true,
                   .external_metadata = false,
                   .inline_headers    = true,
                   .name              = "test_hidden_inline_pool");

    printf("n00b#539: marshal interior-pointer resolution is linear in heap words\n");

    for (int heap = HEAP_ARENA; heap <= HEAP_HIDDEN_POOL; heap++) {
        for (int no_stw = 0; no_stw <= 1; no_stw++) {
            char shape[128];

            // 32 KB and 128 KB arrays: 4K and 16K words, a pointer every 8.
            cost_t self_small = run_self(heap, no_stw, 4096);
            cost_t self_big   = run_self(heap, no_stw, 16384);
            snprintf(shape,
                     sizeof shape,
                     "%s, %s, self-referencing arrays",
                     heap_names[heap],
                     no_stw ? "no-stw" : "stw");
            check_walk(shape, self_small, self_big);
            check_searches(shape,
                           self_small,
                           self_big,
                           N_SELF * (16384 - 4096) / SLOT_STRIDE);

            // 4K slices into a 64 KB blob, then into a 256 KB one.
            cost_t sl_small = run_slices(heap, no_stw, 64 * 1024);
            cost_t sl_big   = run_slices(heap, no_stw, 256 * 1024);
            snprintf(shape,
                     sizeof shape,
                     "%s, %s, slices into one blob",
                     heap_names[heap],
                     no_stw ? "no-stw" : "stw");
            check_walk(shape, sl_small, sl_big);
        }
    }

    n00b_shutdown();
    return failures ? 1 : 0;
}
