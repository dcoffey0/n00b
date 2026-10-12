/*
 * test_gc_mark_registry_lookups.c -- n00b#539.
 *
 * The mark answers "which heap holds this word" from its own per-collection
 * scan tree, which names the arena segment or pool page. Resolving the word's
 * allocation should then use that range's registry record, not search the
 * global mmap tree again for the same answer: a search visits about 26
 * registry nodes on a heap of a few thousand segments.
 *
 * The check counts registry point searches (n00b_mmap_point_searches, a
 * debug-build counter) over one collection, at two heap sizes, and bounds the
 * searches the larger heap adds per pointer word it adds. Half the words point
 * at another object's base and half into their own object, so both the direct
 * and the interior resolution are covered.
 *
 * The same holds one level down: the pin check and the page pin on a first
 * visit must take the segment the scan tree already found, not descend the
 * tree again. n00b_gc_last_segment_lookups counts those descents; it is bounded
 * per added object over a heap of many small objects.
 *
 * Registered under both collectors.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/stw.h"

#if defined(N00B_DEBUG)
extern _Atomic(uint64_t) n00b_mmap_point_searches;

#define N_OBJECTS 256

// Registry searches a mark may make per added pointer word. Resolving from
// the scan tree's record makes none; a search per word is 1.0.
#define SEARCHES_PER_WORD 0.05

extern _Atomic uint64_t n00b_gc_last_segment_lookups;

// Segment-tree descents a collection may make per added object.
#define LOOKUPS_PER_OBJECT 0.05

static void ***g_objs;

// A pool row builds its heap in user_pool and collects an empty arena, which
// traces through the pool; an arena row collects the arena holding the heap.
static uint64_t
run(bool pool, uint64_t words)
{
    n00b_arena_t *arena = n00b_new_arena(.size = (pool ? 0 : N_OBJECTS * words * 8 * 4)
                                                 + (1 << 24),
                                         .use_gc = true);
    n00b_alloc_opts_t opts = {
        .allocator = pool ? (n00b_allocator_t *)&n00b_get_runtime()->user_pool
                          : (n00b_allocator_t *)arena,
    };

    g_objs = n00b_alloc_array(void **, N_OBJECTS);
    for (uint64_t i = 0; i < N_OBJECTS; i++) {
        g_objs[i] = n00b_alloc_array_with_opts(void *, words, &opts);
    }
    for (uint64_t i = 0; i < N_OBJECTS; i++) {
        void **obj  = g_objs[i];
        void **next = g_objs[(i + 1) % N_OBJECTS];
        for (uint64_t k = 0; k < words; k++) {
            obj[k] = (k & 1) ? (void *)&obj[k] : (void *)next;
        }
    }

    uint64_t before = atomic_load(&n00b_mmap_point_searches);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t searches = atomic_load(&n00b_mmap_point_searches) - before;

    for (uint64_t i = 0; i < N_OBJECTS; i++) {
        void **obj  = g_objs[i];
        void **next = g_objs[(i + 1) % N_OBJECTS];
        for (uint64_t k = 0; k < words; k++) {
            void *want = (k & 1) ? (void *)&obj[k] : (void *)next;
            n00b_require(obj[k] == want, "a pointer word was lost across the collection");
        }
    }

    g_objs = nullptr;
    return searches;
}

// Segment lookups one collection makes over `n` four-word objects in a chain.
static uint64_t
segment_lookups(uint64_t n)
{
    n00b_arena_t     *arena = n00b_new_arena(.size = n * 64 + (1 << 24), .use_gc = true);
    n00b_alloc_opts_t opts  = {.allocator = (n00b_allocator_t *)arena};

    g_objs = n00b_alloc_array(void **, n);
    for (uint64_t i = 0; i < n; i++) {
        g_objs[i] = n00b_alloc_array_with_opts(void *, 4, &opts);
    }
    for (uint64_t i = 0; i < n; i++) {
        g_objs[i][0] = g_objs[(i + 1) % n];
    }

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t lookups = n00b_atomic_load(&n00b_gc_last_segment_lookups);

    for (uint64_t i = 0; i < n; i++) {
        n00b_require(g_objs[i][0] == g_objs[(i + 1) % n], "an object lost its link");
    }
    g_objs = nullptr;
    return lookups;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#539: registry searches per pointer word in the mark (pin-all: %s)\n",
           n00b_gc_pin_all_policy() ? "on" : "off");

    uint64_t small_words = 256;
    uint64_t big_words   = 1024;
    uint64_t added_words = N_OBJECTS * (big_words - small_words);
    int      failed      = 0;

    for (int pool = 0; pool <= 1; pool++) {
        const char *heap     = pool ? "user_pool" : "gc arena";
        uint64_t    small    = run(pool, small_words);
        uint64_t    big      = run(pool, big_words);
        double      per_word = big > small ? (double)(big - small) / (double)added_words
                                           : 0.0;

        printf("  ... %s: %llu searches for %llu pointer words, then %llu for"
               " %llu: %.3f per added word\n",
               heap,
               (unsigned long long)small,
               (unsigned long long)(N_OBJECTS * small_words),
               (unsigned long long)big,
               (unsigned long long)(N_OBJECTS * big_words),
               per_word);

        bool over = per_word > SEARCHES_PER_WORD;

        printf("  [%s] %s: the mark resolves pointer words from the scan tree's"
               " record, not a registry search (bound %.2f per word)\n",
               over ? "FAIL" : "PASS",
               heap,
               SEARCHES_PER_WORD);
        failed |= over;
    }

    uint64_t few      = segment_lookups(4096);
    uint64_t many     = segment_lookups(16384);
    double   per_obj  = many > few ? (double)(many - few) / (double)(16384 - 4096) : 0.0;
    bool     over_obj = per_obj > LOOKUPS_PER_OBJECT;

    printf("  ... %llu segment lookups for 4096 objects, then %llu for 16384:"
           " %.3f per added object\n",
           (unsigned long long)few,
           (unsigned long long)many,
           per_obj);
    printf("  [%s] a first visit pins with the segment the scan tree found"
           " (bound %.2f per object)\n",
           over_obj ? "FAIL" : "PASS",
           LOOKUPS_PER_OBJECT);
    failed |= over_obj;

    n00b_shutdown();
    return failed;
}
#else
int
main(void)
{
    printf("  [SKIP] n00b_mmap_point_searches is counted only in debug builds\n");
    return 0;
}
#endif
