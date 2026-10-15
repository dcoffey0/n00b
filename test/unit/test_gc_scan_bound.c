/*
 * test_gc_scan_bound.c — n00b#275 and n00b#395.
 *
 * Both issues are about the same loop: the conservative backward guard scan in
 * _find_sentinal (alloc.c), which the mark loop reaches once per candidate
 * pointer that lands inside a heap mapping without being at an allocation
 * base. Two things decided how expensive that loop was, and each is asserted
 * here rather than timed, so the checks mean the same thing on a loaded CI
 * box as on an idle laptop.
 *
 * n00b#275 -- the loop's PER-PAGE COST. The scan asks
 * n00b_check_memory_perms once per page it walks. That answers from the
 * registry only when the mapping's record states its permissions; otherwise it
 * falls through to a pipe write() + poll() (plus, before n00b#384, a
 * process-global signal() per call). Arena segments registered as
 * perms_unknown, so every page of every backward walk over the GC heap paid
 * three syscalls. n00b#275 measured 84% of _n00b_find_alloc_info's samples
 * inside syscalls on a gateway that was stopped-the-world for 90.4% of wall
 * clock. Check: an arena segment's record states rw.
 *
 * n00b#395 -- the loop's LENGTH. The bound was a MONOTONIC all-time
 * high-water mark of a single allocation, process-global. One large
 * allocation therefore raised the cost of every later probe for the remaining
 * life of the process, including after that allocation was freed, and
 * including candidates in mappings that never held anything but small
 * objects. The bound is now taken per mapping. Check: after a large
 * allocation dies, a mapping holding only small live objects reports a small
 * bound while the global high-water is still large.
 *
 * The safety property the tighter bound must not break -- an interior pointer
 * must still resolve back to its own allocation, however far into it the
 * pointer lands -- is n00b#321, and test_alloc_interior_large.c is its
 * regression test. This file re-checks it against the per-mapping bound,
 * because that is the bound n00b#321's failure would now come from.
 */

#define N00B_USE_INTERNAL_API
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/stw.h"

#define MB (1024u * 1024u)

/* Comfortably past the 8 MB floor the bound used to sit on, so "the global
 * high-water is inflated" is unambiguous. */
#define BIG_ALLOC_BYTES (24u * MB)

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

static n00b_mmap_info_t *
record_for(void *p)
{
    auto opt = n00b_mmap_by_address(p);
    return n00b_option_is_set(opt) ? n00b_option_get(opt) : nullptr;
}

/* ---------------------------------------------------------------- n00b#275 */

static void
check_segment_perms_are_known(void *live)
{
    n00b_mmap_info_t *rec = record_for(live);

    if (rec == nullptr) {
        fail("a heap allocation has no registry record at all");
        return;
    }

    if (!n00b_mmap_is_arena_segment(rec) && rec->kind != n00b_mmap_pool) {
        printf("  [SKIP] heap allocation is in a kind-%d mapping, not an arena"
               " segment or pool page\n",
               rec->kind);
        return;
    }

    if (rec->perms == n00b_mmap_perms_unknown) {
        fail("the mapping holding a live allocation records perms_unknown:"
             " every page of every guard scan over it pays a pipe probe");
        return;
    }

    if (rec->perms == n00b_mmap_perms_no_access) {
        fail("the mapping holding a live allocation records no_access");
        return;
    }

    printf("  [PASS] the mapping holding a live allocation records its perms"
           " (%d): the guard scan reads it without a syscall\n",
           rec->perms);
}

/* ---------------------------------------------------------------- n00b#395 */

static void
check_bound_is_per_mapping(void *live)
{
    n00b_mmap_info_t *rec = record_for(live);

    if (rec == nullptr) {
        fail("a live allocation has no registry record");
        return;
    }

    uint64_t per_mapping = atomic_load(&rec->max_alloc_len);
    uint64_t global      = n00b_atomic_load(&n00b_max_inline_alloc_len);

    printf("  ... live object is in a kind-%d mapping of %llu bytes"
           " (pin-all policy: %s)\n",
           rec->kind,
           (unsigned long long)(rec->end - rec->start),
           n00b_gc_pin_all_policy() ? "on" : "off");

    if (global < BIG_ALLOC_BYTES) {
        fail("the global high-water never saw the large allocation, so this"
             " test cannot tell a tight bound from an inflated one");
        return;
    }

    printf("  ... global all-time high-water: %10llu bytes (%.2f MB)\n",
           (unsigned long long)global,
           global / (double)MB);
    printf("  ... this mapping's bound:       %10llu bytes (%.2f MB)\n",
           (unsigned long long)per_mapping,
           per_mapping / (double)MB);

    if (per_mapping == 0) {
        fail("the mapping holding the live set records no bound, so the scan"
             " falls back to the inflated global high-water");
        return;
    }

    if (per_mapping >= BIG_ALLOC_BYTES) {
        fail("the mapping holding only small live objects is still bounded by"
             " a large allocation that is dead: the hysteresis is intact");
        return;
    }

    pass("a mapping holding only small live objects is bounded by its own"
         " contents, not by a large allocation that has died");
}

/* The check above reads the recorded bound. This one measures what the scan
 * actually WALKS, so a _find_sentinal that recorded the per-mapping bound and
 * then ignored it cannot pass.
 *
 * The probe address is deep in the arena's unallocated tail: far enough past
 * the last guard that the walk cannot reach one, so every call runs to its
 * bound and stops. Average words per call is then the bound itself. */
static void
check_scan_actually_walks_less(void)
{
    n00b_arena_t *arena = n00b_get_runtime()->default_arena;

    /* Put SOMETHING in the current segment first.  Under the pin-all policy
     * (no GC type map linked, n00b#309 -- Linux CI, wax) a collect copies
     * nothing into the new segment, so right after one its record has no
     * allocation to bound by and the walk is limited only by the mapping's
     * start.  That is correct, but it is not what this check measures. */
    uint8_t *seed = n00b_alloc_array(uint8_t, 4096);
    assert(seed != nullptr);
    memset(seed, 0x33, 4096);

    char         *next  = n00b_atomic_load(&arena->next_alloc);
    /* Well past the 8 MB floor's reach would need 8 MB of headroom; 1 MB is
     * plenty to clear the per-mapping bound, and clearing THAT is the point --
     * with the old bound the walk runs the full floor and never finds a guard
     * either. */
    char         *probe = next + 1024u * 1024u;

    if (probe + sizeof(uint64_t) >= arena->segment_end) {
        printf("  [SKIP] not enough unallocated tail to probe\n");
        return;
    }

    n00b_mmap_info_t *rec = record_for(probe);

    if (rec == nullptr) {
        fail("the arena's unallocated tail is outside its own registry record");
        return;
    }

    uint64_t bound_words
        = (atomic_load(&rec->max_alloc_len) + 7) / sizeof(uint64_t);

    if (bound_words == 0) {
        fail("an allocation into the current segment was not recorded"
             " against its mapping");
        return;
    }

    uint64_t calls0 = atomic_load(&n00b_sentinel_scan_calls);
    uint64_t words0 = atomic_load(&n00b_sentinel_scan_words);

    for (int i = 0; i < 64; i++) {
        bool pin = false;
        (void)n00b_find_alloc_info(probe + i * 8,
                                   .scan_for_header = true,
                                   .pin             = &pin);
        n00b_alloc_hdr_unpin(pin);
    }

    uint64_t calls = atomic_load(&n00b_sentinel_scan_calls) - calls0;
    uint64_t words = atomic_load(&n00b_sentinel_scan_words) - words0;

    if (calls == 0) {
        fail("the probes never reached the backward guard scan, so this check"
             " measured nothing");
        return;
    }

    uint64_t per_call = words / calls;

    printf("  ... %llu scans, %llu words walked, %llu words per scan\n",
           (unsigned long long)calls,
           (unsigned long long)words,
           (unsigned long long)per_call);
    printf("  ... the old floor alone was %u words per scan\n",
           1u << 20);

    /* One allocation's worth of slack over the recorded bound: the walk starts
     * one header below the probe and stops at the floor. */
    if (per_call > bound_words + 64) {
        printf("  [FAIL] each scan walks %llu words against a recorded bound"
               " of %llu: the per-mapping bound is recorded but not used\n",
               (unsigned long long)per_call,
               (unsigned long long)bound_words);
        failures++;
        return;
    }

    pass("each scan walks its mapping's own bound, not the 8 MB floor");
}

/* ------------------------------------------------- n00b#321, re-checked ---- */

static void
check_interior_still_resolves(uint8_t *base, size_t len)
{
    bool              base_pin  = false;
    n00b_alloc_info_t base_info = n00b_find_alloc_info(base, .pin = &base_pin);
    n00b_alloc_hdr_unpin(base_pin);

    if (!n00b_alloc_info_is_heap(base_info)) {
        fail("the base of the probe allocation does not resolve");
        return;
    }

    /* Walk the whole allocation, not just a couple of offsets: the failure
     * the bound can cause is "resolution stops working past some distance". */
    static const size_t fractions[] = {1, 2, 4, 8, 16};

    for (size_t i = 0; i < sizeof(fractions) / sizeof(fractions[0]); i++) {
        size_t off = len - (len / fractions[i]);

        if (off >= len) {
            off = len - 8;
        }

        bool              pin = false;
        n00b_alloc_info_t got = n00b_find_alloc_info(base + off,
                                                     .scan_for_header = true,
                                                     .pin             = &pin);
        n00b_alloc_hdr_unpin(pin);

        if (!n00b_alloc_info_is_heap(got) || got.kind != base_info.kind
            || got.hdr.in_line != base_info.hdr.in_line) {
            printf("  [FAIL] interior +%zu of %zu does not resolve to its own"
                   " allocation: a root there would be dropped\n",
                   off,
                   len);
            failures++;
            return;
        }
    }

    pass("interior pointers across the whole allocation still resolve to it"
         " under the per-mapping bound");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#275 / n00b#395: the conservative guard scan's cost and bound\n");

    /* The guard scan only runs for allocators that write inline headers.
     * Without that, everything below is vacuous. */
    assert(n00b_default_allocator()->add_inline_header);

    /* --- inflate the global high-water, exactly as a transient spike does. */
    {
        uint8_t *big = n00b_alloc_array(uint8_t, BIG_ALLOC_BYTES);
        assert(big != nullptr);
        memset(big, 0xA5, BIG_ALLOC_BYTES);

        /* Interior resolution has to survive at the size that sets the bound,
         * before the allocation dies -- that is n00b#321's case. */
        check_interior_still_resolves(big, BIG_ALLOC_BYTES);

        /* Drop the only reference. Zeroing the slot matters: a conservative
         * collector keeps anything a live stack word still points at. */
        memset(big, 0, n00b_page_size);
        big = nullptr;
    }

    /* --- a small live set that must outlive the collection.  Held in a
     * stack array so the conservative root scan keeps it without any root
     * registration. */
#define KEEP_COUNT 256
#define KEEP_BYTES 512
    uint8_t *keep[KEEP_COUNT];

    for (int i = 0; i < KEEP_COUNT; i++) {
        keep[i] = n00b_alloc_array(uint8_t, KEEP_BYTES);
        memset(keep[i], (uint8_t)i, KEEP_BYTES);
    }

    n00b_stop_the_world();
    n00b_collect(n00b_get_runtime()->default_arena);
    n00b_restart_the_world();

    for (int i = 0; i < KEEP_COUNT; i++) {
        assert(keep[i][0] == (uint8_t)i);
    }

    void *live = keep[0];

    printf("\n-- n00b#275: per-page cost of the guard scan\n");
    check_segment_perms_are_known(live);

    printf("\n-- n00b#395: length of the guard scan\n");
    check_bound_is_per_mapping(live);
    check_scan_actually_walks_less();

    printf("\n-- n00b#321: the safety property the bound must not break\n");
    {
        size_t   len   = 512u * 1024u;
        uint8_t *probe = n00b_alloc_array(uint8_t, len);
        assert(probe != nullptr);
        memset(probe, 0x5A, len);
        check_interior_still_resolves(probe, len);

        /* And the bound must still REJECT a candidate that is not inside any
         * allocation -- a scan "fixed" by accepting whatever it lands on
         * would pass every check above. */
        bool              bogus_pin = false;
        bool              probe_pin = false;
        n00b_alloc_info_t bogus
            = n00b_find_alloc_info((void *)((uintptr_t)probe - 64),
                                   .scan_for_header = true,
                                   .pin             = &bogus_pin);
        n00b_alloc_hdr_unpin(bogus_pin);
        n00b_alloc_info_t probe_info = n00b_find_alloc_info(probe,
                                                            .pin = &probe_pin);
        n00b_alloc_hdr_unpin(probe_pin);

        if (n00b_alloc_info_is_heap(bogus)
            && bogus.kind == probe_info.kind
            && bogus.hdr.in_line == probe_info.hdr.in_line) {
            fail("an address BEFORE the allocation resolved to it");
        }
        else {
            pass("an address outside the allocation still does not resolve"
                 " to it");
        }
    }

    if (failures) {
        printf("\n%d check(s) failed.\n", failures);
        return 1;
    }

    printf("\nn00b#275 / n00b#395: the guard scan is bounded by the mapping it"
           " walks, and reads that mapping's perms without a syscall.\n");
    return 0;
}
