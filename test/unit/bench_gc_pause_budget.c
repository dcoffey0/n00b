/*
 * n00b#539: a stop-the-world pause must not grow without bound with the heap.
 *
 * runtime.h documents a budget and nothing enforces it:
 *
 *     "The 0.25s pause budget is enforced against gc_max_pause_ns under a
 *      full-build workload"                      (include/core/runtime.h:138)
 *
 * Grepping the tree finds that sentence, the identical one at gc.c:370, and no
 * test. The budget has never been an assertion. Measured on a shipping
 * crayon-gw (crashappsec/wax#1229): gc_max_pause_ns = 189.1 s, 756x the stated
 * budget, with the process frozen for the duration -- the kernel completes the
 * TCP handshake and no handler thread exists to reply, which its watchdog
 * records as "accepted-no-response".
 *
 * WHY THE BUDGET HELD IN CI AND NOT THERE. The sentence names its own
 * precondition: "under a full-build workload". A full build is short-lived and
 * links a GC type map. A gateway is neither. With no type map the collector
 * takes the pin-all path (n00b_gc_pin_all_policy, gc.c:3814), and gc.c:4054 is
 * explicit about what that costs:
 *
 *     "Under the pin-all policy (a binary with no GC type map, n00b#309)
 *      EVERY survivor lives in a run like this"
 *
 * "A run like this" is a RETAINED RUN: a fresh n00b_segment_t allocated from
 * the system pool and spliced into the arena's segment chain for each pinned
 * run that survives (gc.c:4030-4057). So under pin-all the chain gains a
 * segment per surviving run, per collection -- and the from-space lookup walks
 * a tree built from that chain, which n00b_from_segment_for says in as many
 * words: "the from-space chain gains a segment for every retained page run, so
 * walking it per lookup grows with the heap's fragmentation" (gc.c:3051).
 * The shipped gateway measured 12532 retained segments of 12533.
 *
 * WHAT THIS MEASURES. Two dimensions, because one alone proves nothing:
 * survivor count (how fragmented the retained heap is) and collection count
 * (whether cost accumulates across passes). Per point it records the pause and
 * the collector's own segment-walk counters, which are independent readings of
 * the same effect -- wall-clock pause is what a watchdog sees,
 * n00b_gc_last_segment_lookup_steps is what the collector actually did.
 *
 * Growth EXPONENTS are fitted between adjacent points rather than reporting raw
 * times, because the absolute numbers are machine-specific and the shape is
 * not. Linear (~1.0) in survivors is the cost of having more live data to
 * trace and is expected.
 *
 * WHAT IT REPRODUCES, AND WHAT IT DOES NOT (measured, macOS arm64, 2026-10-09).
 * The segment shape reproduces exactly: segments^1.00 with survivors, one
 * retained run per survivor -- 16002 segments of which 16001 retained, the same
 * ratio the shipping gateway showed at 12533/12532. Per-lookup cost does grow
 * with it (steps/lookup 10.2 -> 13.8 as segments go 2k -> 16k), and the pause
 * exponent crosses 1.0 (1.00 then 1.22) as the heap grows.
 *
 * It does NOT yet reproduce the unbounded pause. At 16000 survivors a
 * collection takes ~178 ms -- inside the 250 ms budget, not 189 s. Two things
 * this rules out: cost does not accumulate across collections at a fixed live
 * set (the last pass is consistently FASTER than the first, 0.59-0.86x), and
 * the segment lookup is tree-structured rather than a linear walk, so segment
 * count alone buys only a log factor. The gateway carried a 429 MB live set
 * against this harness's few MB, so live-set SIZE is the remaining untested
 * variable and the next thing to sweep. Recorded here rather than in a comment
 * on the issue so the next person does not re-derive it.
 *
 * REGISTERED AS A TEST at a small size, where it asserts the documented 0.25 s
 * budget directly. That assertion is the thing this file exists to add: today
 * nothing in the tree would notice a pause regression at all. Run the binary
 * for the sweep. N00B_GC_PIN_ALL=1 is set by the harness itself so the policy
 * under test is the one production runs, independent of how this binary was
 * linked -- without it a type-map build silently measures the copying
 * collector instead, and reports a pass that means nothing.
 *
 * N00B_BENCH_SURVIVORS (comma-separated counts), N00B_BENCH_COLLECTIONS and
 * N00B_BENCH_PAUSE_BUDGET_MS size it.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/align.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/stw.h"

// Published at collect cleanup (gc.c:4071). Defined in gc.c but not declared
// in any header; the pause counters live on the runtime, these do not.
extern _Atomic uint64_t n00b_gc_last_segment_lookups;
extern _Atomic uint64_t n00b_gc_last_segment_lookup_steps;

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "gc pause budget check failed: " #expr);          \
    } while (0)

#define MAX_POINTS 16

// The documented budget (runtime.h:138), in ms.
#define DEFAULT_PAUSE_BUDGET_MS 250

#define ARENA_OPTS(a)                                                          \
    &(n00b_alloc_opts_t)                                                       \
    {                                                                          \
        .allocator = (n00b_allocator_t *)(a)                                   \
    }

// A survivor with a pointer field, so the object is scanned rather than
// treated as a leaf. Each one is reached from the roots array below and so
// survives every collection.
typedef struct survivor_t {
    uint64_t           magic;
    uint64_t           index;
    struct survivor_t *link;
} survivor_t;

#define SURVIVOR_MAGIC UINT64_C(0x5339C0115E7A1100)

// The root set. File-scope and strongly typed so the objects stay reachable
// without depending on stack scanning, and so the test's own liveness is not
// what is under measurement.
static survivor_t **g_roots;
static uint64_t     g_nroots;

static uint64_t pause_budget_ms = DEFAULT_PAUSE_BUDGET_MS;

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

// Same shape the rocs benches use (test/unit/rocs_test_support.h): a raw
// monotonic clock, so the measurement does not depend on runtime state that a
// collection perturbs.
static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

typedef struct {
    uint64_t survivors;
    uint64_t collections;    // how many collections preceded the measured one
    double   first_ms;       // the first collection, for reference
    double   collect_ms;     // wall clock around the MEASURED collection
    uint64_t lookups;        // from-space lookups the collector made
    uint64_t lookup_steps;   // scan-tree nodes it visited doing them
    double   steps_per_lookup;
    uint64_t segments;       // arena segment chain length after the run
    uint64_t retained;       // of which retained (pin-all survivor runs)
} point_t;

// Walk the arena's segment chain. This is the quantity the whole issue turns
// on: under pin-all each surviving run is retained in place and splices a
// fresh descriptor in here (gc.c:4030-4057), so if the chain grows per
// collection then every later collection has more to walk. Same traversal
// crayon-gw's status endpoint does, which is where 12532/12533 came from.
static void
segment_stats(n00b_arena_t *arena, uint64_t *total, uint64_t *retained)
{
    uint64_t        n = 0;
    uint64_t        r = 0;
    n00b_segment_t *seg;

    if (arena == nullptr) {
        *total    = 0;
        *retained = 0;
        return;
    }
    seg = n00b_atomic_load(&arena->current_segment);
    while (seg != nullptr) {
        n++;
        if (seg->retained) {
            r++;
        }
        seg = seg->next_segment;
    }
    *total    = n;
    *retained = r;
}

// Allocate `n` survivors and keep every one reachable. Interleaving garbage
// between them is what makes the survivors land in separate page runs: under
// pin-all each surviving run is retained in place and contributes its own
// segment, which is the growth being measured. A dense block of survivors
// would pin a handful of runs regardless of count and show nothing.
static void
build_heap(n00b_arena_t *arena, uint64_t n)
{
    g_roots  = n00b_alloc_array(survivor_t *, n);
    CHECK(g_roots != nullptr);
    g_nroots = n;

    for (uint64_t i = 0; i < n; i++) {
        survivor_t *s = n00b_alloc_with_opts(survivor_t, ARENA_OPTS(arena));
        CHECK(s != nullptr);
        s->magic   = SURVIVOR_MAGIC;
        s->index   = i;
        s->link    = nullptr;
        g_roots[i] = s;

        // Garbage between survivors, sized to push the NEXT survivor onto a
        // different page. This is load-bearing, not padding: pin-all retains
        // whole PAGE RUNS, so survivors packed into one run are retained as
        // one segment no matter how many there are. Only when each survivor
        // sits in its own run does the chain gain a segment per survivor --
        // which is the shape the shipping gateway was in at 12532 retained
        // runs of 12533 segments. A smaller gap here measures a coalesced
        // heap and shows nothing, which is exactly what an earlier version of
        // this bench did.
        uint64_t gap    = (uint64_t)n00b_page_size * 2;
        char    *filler = n00b_alloc_array_with_opts(char,
                                                     gap,
                                                     ARENA_OPTS(arena));
        CHECK(filler != nullptr);
        filler[0]       = (char)i;
        filler[gap - 1] = (char)i;
    }
}

// Every survivor must still be intact afterwards, or the pause number is
// describing a collection that lost data rather than one that did the work.
static void
verify_heap(void)
{
    for (uint64_t i = 0; i < g_nroots; i++) {
        CHECK(g_roots[i] != nullptr);
        CHECK(g_roots[i]->magic == SURVIVOR_MAGIC);
        CHECK(g_roots[i]->index == i);
    }
}

// One point: build a heap of `survivors`, then collect `collections` times,
// timing the FIRST and the LAST.
//
// The repeat count is the dimension that matters and the one a single-shot
// measurement misses. A gateway does not take a long pause because one
// collection has a lot of survivors -- it takes one because it has collected
// thousands of times already. If retained segments accumulate across
// collections, the last pause exceeds the first at fixed live-set size, and
// the segment counts below say so directly.
static point_t
run_point(uint64_t survivors, uint64_t collections)
{
    point_t       p     = {.survivors = survivors, .collections = collections};
    n00b_arena_t *arena = n00b_new_arena(.size = 1 << 26, .use_gc = true);
    CHECK(arena != nullptr);

    build_heap(arena, survivors);

    for (uint64_t i = 0; i < collections; i++) {
        uint64_t start = now_ns();
        n00b_stop_the_world();
        n00b_collect(arena);
        n00b_restart_the_world();
        double ms = (double)(now_ns() - start) / 1e6;

        if (i == 0) {
            p.first_ms = ms;
        }
        p.collect_ms = ms;

        // Churn between collections: fresh garbage, so each pass has
        // something to reclaim and the survivors get re-pinned rather than
        // the collector finding an untouched heap and short-circuiting.
        for (int j = 0; j < 64; j++) {
            survivor_t *dead = n00b_alloc_with_opts(survivor_t,
                                                    ARENA_OPTS(arena));
            CHECK(dead != nullptr);
            dead->magic = 0;
            dead->link  = nullptr;
        }
    }

    verify_heap();

    p.lookups      = n00b_atomic_load(&n00b_gc_last_segment_lookups);
    p.lookup_steps = n00b_atomic_load(&n00b_gc_last_segment_lookup_steps);
    p.steps_per_lookup = p.lookups ? (double)p.lookup_steps / (double)p.lookups
                                   : 0.0;
    segment_stats(arena, &p.segments, &p.retained);

    g_roots  = nullptr;
    g_nroots = 0;
    return p;
}

// log(y1/y0) / log(x1/x0): 1.0 = linear in survivors, 2.0 = quadratic.
static double
exponent(double x0, double y0, double x1, double y1)
{
    if (x0 <= 0 || y0 <= 0 || x1 <= 0 || y1 <= 0 || x1 == x0) {
        return 0.0;
    }
    return log(y1 / y0) / log(x1 / x0);
}

int
main(int argc, char **argv)
{
    // Before n00b_init: the policy is decided once, at the first collection,
    // and a process never mixes modes (gc.c:3814). Production runs pin-all
    // because it links no type map; forcing it here makes this measurement
    // independent of how the test binary happens to be linked.
    setenv("N00B_GC_PIN_ALL", "1", 0);

    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    pause_budget_ms = env_u64("N00B_BENCH_PAUSE_BUDGET_MS",
                              DEFAULT_PAUSE_BUDGET_MS);

    uint64_t sizes[MAX_POINTS];
    uint64_t nsizes = 0;

    const char *spec = getenv("N00B_BENCH_SURVIVORS");
    if (spec != nullptr && *spec != '\0') {
        // Both of these are block items rather than for-init declarations:
        // ncc's GC stack-map transform rejects a root declared in a statement
        // context ("declare the root as a block item before the statement").
        char *copy = strdup(spec);
        CHECK(copy != nullptr);
        char *tok = strtok(copy, ",");
        while (tok != nullptr && nsizes < MAX_POINTS) {
            uint64_t v = (uint64_t)strtoull(tok, nullptr, 10);
            if (v > 0) {
                sizes[nsizes++] = v;
            }
            tok = strtok(nullptr, ",");
        }
        free(copy);
    }
    if (nsizes == 0) {
        // Registered-test size: small enough to stay well inside the budget on
        // a healthy collector, large enough that a growth defect shows.
        sizes[nsizes++] = 2000;
        sizes[nsizes++] = 4000;
    }

    uint64_t collections = env_u64("N00B_BENCH_COLLECTIONS", 20);

    printf("n00b#539: stop-the-world pause vs survivors and collection count"
           " (pin-all)\n");
    printf("budget: %llu ms (runtime.h:138);  %llu collections per point\n\n",
           (unsigned long long)pause_budget_ms,
           (unsigned long long)collections);
    printf("  %10s  %9s  %9s  %12s  %9s  %8s  %8s\n",
           "survivors",
           "first ms",
           "last ms",
           "lookup steps",
           "steps/lk",
           "segments",
           "retained");

    point_t points[MAX_POINTS];
    for (uint64_t i = 0; i < nsizes; i++) {
        points[i] = run_point(sizes[i], collections);
        printf("  %10llu  %9.2f  %9.2f  %12llu  %9.2f  %8llu  %8llu\n",
               (unsigned long long)points[i].survivors,
               points[i].first_ms,
               points[i].collect_ms,
               (unsigned long long)points[i].lookup_steps,
               points[i].steps_per_lookup,
               (unsigned long long)points[i].segments,
               (unsigned long long)points[i].retained);
    }

    // Does the pause grow with COLLECTION COUNT at a fixed live set? That is
    // the unbounded term. Growth with survivor count alone is just more data
    // to trace.
    printf("\n  drift across %llu collections at fixed live set"
           " (last/first):\n",
           (unsigned long long)collections);
    for (uint64_t i = 0; i < nsizes; i++) {
        double ratio = points[i].first_ms > 0.0
                         ? points[i].collect_ms / points[i].first_ms
                         : 0.0;
        printf("    %llu survivors:  %.2fx\n",
               (unsigned long long)points[i].survivors,
               ratio);
    }

    if (nsizes > 1) {
        printf("\n  growth between adjacent points (1.0 = linear in"
               " survivors):\n");
        for (uint64_t i = 1; i < nsizes; i++) {
            double e_pause = exponent((double)points[i - 1].survivors,
                                      points[i - 1].collect_ms,
                                      (double)points[i].survivors,
                                      points[i].collect_ms);
            double e_steps = exponent((double)points[i - 1].survivors,
                                      (double)points[i - 1].lookup_steps,
                                      (double)points[i].survivors,
                                      (double)points[i].lookup_steps);
            double e_seg   = exponent((double)points[i - 1].survivors,
                                    (double)points[i - 1].segments,
                                    (double)points[i].survivors,
                                    (double)points[i].segments);
            printf("    %llu -> %llu:  pause^%.2f  lookup_steps^%.2f"
                   "  segments^%.2f\n",
                   (unsigned long long)points[i - 1].survivors,
                   (unsigned long long)points[i].survivors,
                   e_pause,
                   e_steps,
                   e_seg);
        }
    }

    // The assertion the tree has been missing. Deliberately the documented
    // budget and not a fitted curve: a threshold is a gate a regression trips,
    // whereas an exponent needs a human to read it. The sweep output above is
    // for diagnosis once this fails.
    int failures = 0;
    for (uint64_t i = 0; i < nsizes; i++) {
        if (points[i].collect_ms > (double)pause_budget_ms) {
            printf("\n  [FAIL] %llu survivors: a single collection paused"
                   " %.1f ms, over the documented %llu ms budget\n",
                   (unsigned long long)points[i].survivors,
                   points[i].collect_ms,
                   (unsigned long long)pause_budget_ms);
            failures++;
        }
    }

    if (failures) {
        printf("\nn00b#539: %d point(s) exceeded the pause budget.\n",
               failures);
        n00b_shutdown();
        return 1;
    }

    printf("\nn00b#539: every collection stayed inside the %llu ms budget.\n",
           (unsigned long long)pause_budget_ms);
    n00b_shutdown();
    return 0;
}
