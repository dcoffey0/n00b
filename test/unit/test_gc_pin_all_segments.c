// Under the pin-all policy every page holding a survivor is retained in place
// and chained onto the arena as its own segment, so a fragmented heap carries
// one from-space segment per live page run into the next collection. Finding
// the segment of each reached object must not cost a walk of that chain.

#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"

#define REQUIRE(c)                                                             \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define LIVE_N 256

extern _Atomic uint64_t n00b_gc_last_retained_runs;
extern _Atomic uint64_t n00b_gc_last_segment_lookups;
extern _Atomic uint64_t n00b_gc_last_segment_lookup_steps;

typedef struct {
    uint64_t value;
} live_t;

int
main(int argc, char **argv)
{
    setenv("N00B_GC_PIN_ALL", "1", 1);

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    uint64_t         filler = 2 * n00b_page_size;
    n00b_arena_t    *arena  = n00b_new_arena(.size = (LIVE_N + 8) * (filler + 4096), .use_gc = true);
    n00b_alloc_opts_t opts  = {.allocator = (n00b_allocator_t *)arena};
    n00b_alloc_opts_t raw   = {.allocator = (n00b_allocator_t *)arena,
                               .scan_kind = N00B_GC_SCAN_KIND_NONE};

    // Each survivor is followed by two pages of garbage, so every one ends up
    // on a page run of its own.
    live_t *volatile live[LIVE_N];
    for (int i = 0; i < LIVE_N; i++) {
        live[i]        = n00b_alloc_with_opts(live_t, &opts);
        live[i]->value = (uint64_t)i;
        (void)n00b_alloc_array_with_opts(uint8_t, filler, &raw);
    }

    n00b_collect(arena);
    uint64_t runs = n00b_atomic_load(&n00b_gc_last_retained_runs);
    REQUIRE(runs >= LIVE_N);

    // This collection's from-space is the new primary segment plus every one
    // of those runs, and it reaches all LIVE_N survivors again.
    n00b_collect(arena);

    uint64_t lookups = n00b_atomic_load(&n00b_gc_last_segment_lookups);
    uint64_t steps   = n00b_atomic_load(&n00b_gc_last_segment_lookup_steps);
    printf("  retained runs %llu, segment lookups %llu, steps %llu\n",
           (unsigned long long)runs,
           (unsigned long long)lookups,
           (unsigned long long)steps);

    // A balanced search over ~LIVE_N segments is about 9 levels deep; a chain
    // walk averages LIVE_N / 2 per lookup. Reached objects reuse the segment the
    // scan already found, so the lookups left are few, not one per object.
    REQUIRE(lookups > 0);
    REQUIRE(steps <= 16 * lookups);

    for (int i = 0; i < LIVE_N; i++) {
        REQUIRE(live[i]->value == (uint64_t)i);
    }
    printf("  [PASS] pin-all segment lookups do not walk the segment chain\n");

    n00b_shutdown();
    return 0;
}
