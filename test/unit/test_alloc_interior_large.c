/*
 * test_alloc_interior_large.c — n00b#321.
 *
 * A GC stack root may hold an INTERIOR pointer: n00b_visit_possible_pointer
 * (gc.c) explicitly retries n00b_find_alloc_info with .scan_for_header = true
 * when the exact lookup misses, and n00b_translate_pointer forwards
 * offset-preserving. So interior roots are a supported contract, and the
 * collector's ability to resolve one back to its allocation is what makes
 * them safe.
 *
 * That resolution is a backward scan for the allocation's guard word, and it
 * has to be bounded -- a conservative candidate that is NOT a pointer has no
 * guard behind it, and an unbounded walk to the segment start would livelock
 * the collector on a large arena.
 *
 * The bound used to be a flat 8 MB. Past that the scan gave up SHORT of a
 * live object's guard, and a failed resolution is indistinguishable from
 * "this address is free space": the visit path drops the root, so the object
 * is neither marked nor forwarded while a live root still points into it.
 * The next dereference reads a stale address -- non-null, heap-shaped, and
 * fatal. Downstream (crashappsec/wax#674) that surfaced as a SIGSEGV on the
 * first load after a safepoint.
 *
 * This test walks interior offsets across that old boundary. It is a
 * regression test for the bound, not for the 8 MB number: the point is that
 * resolution must not depend on how far into an allocation a pointer lands.
 */

#define N00B_USE_INTERNAL_API
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"

#define MB (1024u * 1024u)

/* Comfortably past the old 8 MB bound, so the sweep covers both sides. */
#define BIG_ALLOC_BYTES (24u * MB)

static int failures = 0;

static void
check_interior(uint8_t *base, n00b_alloc_info_t base_info, size_t off)
{
    bool              pin = false;
    n00b_alloc_info_t got = n00b_find_alloc_info(base + off,
                                                 .scan_for_header = true,
                                                 .pin             = &pin);
    n00b_alloc_hdr_unpin(pin);

    if (!n00b_alloc_info_is_heap(got)) {
        printf("  [FAIL] interior +%-12zu B (%6.2f MB): unresolved (kind=%d)"
               " — a root here would be dropped\n",
               off,
               off / (double)MB,
               got.kind);
        failures++;
        return;
    }

    /* Resolving to SOME allocation is not enough: a back-scan that stopped on
     * the wrong word would also report heap. It must be the same allocation
     * the base address resolves to. */
    if (got.kind != base_info.kind
        || got.hdr.in_line != base_info.hdr.in_line) {
        printf("  [FAIL] interior +%-12zu B (%6.2f MB): resolved to the WRONG"
               " allocation\n",
               off,
               off / (double)MB);
        failures++;
        return;
    }

    printf("  [PASS] interior +%-12zu B (%6.2f MB) resolves to its allocation\n",
           off,
           off / (double)MB);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#321: interior-pointer resolution vs allocation size\n");

    uint8_t *p = n00b_alloc_array(uint8_t, BIG_ALLOC_BYTES);
    assert(p != nullptr);
    memset(p, 0xA5, BIG_ALLOC_BYTES);

    /* The base must resolve exactly, or the rest of the test proves nothing. */
    bool              base_pin  = false;
    n00b_alloc_info_t base_info = n00b_find_alloc_info(p, .pin = &base_pin);
    n00b_alloc_hdr_unpin(base_pin);
    assert(n00b_alloc_info_is_heap(base_info));

    /* The guard back-scan only runs when the allocator writes inline headers,
     * so assert that shape -- otherwise this test would be vacuous. Note the
     * default allocator sets BOTH inline_headers and external_metadata: the
     * back-scan normalizes an interior address to the allocation base, and the
     * OOB dict lookup then answers from it, which is why the resolved kind
     * here is n00b_alloc_oob rather than n00b_alloc_inline. The back-scan is
     * still the load-bearing step. */
    assert(n00b_default_allocator()->add_inline_header);
    printf("  [PASS] base of a %u MB allocation resolves\n", BIG_ALLOC_BYTES / MB);

    /* Straddle the old 8 MB cliff: inside, exactly at it, and well past. */
    static const size_t offsets[] = {
        1024,             /* trivially inside */
        4u * MB,          /* inside the old bound */
        8u * MB - 8,      /* last word that used to work */
        8u * MB,          /* the old boundary itself */
        8u * MB + 8,      /* first word that used to be DROPPED */
        12u * MB,
        23u * MB,         /* near the end of the allocation */
    };

    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        check_interior(p, base_info, offsets[i]);
    }

    /* A pointer into a segment but NOT into any live allocation must still be
     * rejected -- the bound must not have been "fixed" by making the scan
     * accept anything it happens to land on. */
    bool              bogus_pin = false;
    n00b_alloc_info_t bogus     = n00b_find_alloc_info((void *)((uintptr_t)p - 64),
                                                       .scan_for_header = true,
                                                       .pin             = &bogus_pin);
    n00b_alloc_hdr_unpin(bogus_pin);
    if (n00b_alloc_info_is_heap(bogus)
        && bogus.kind == base_info.kind
        && bogus.hdr.in_line == base_info.hdr.in_line) {
        printf("  [FAIL] an address BEFORE the allocation resolved to it\n");
        failures++;
    }
    else {
        printf("  [PASS] an address outside the allocation does not resolve to it\n");
    }

    if (failures) {
        printf("\n%d check(s) failed: an interior GC root would be silently"
               " dropped.\n",
               failures);
        return 1;
    }

    printf("\nn00b#321: interior resolution is independent of allocation size.\n");

    /* n00b#527. Without this the process returns from main with the runtime
     * still up -- MEASURED here as 5 live threads, because n00b_init starts
     * the default conduit's IO service -- and libc then tears the process
     * down underneath them. That raced as an intermittent SIGBUS on the macOS
     * CI runner, AFTER every assertion above had printed PASS and with no
     * crash-handler output, which is what a fault during exit teardown looks
     * like.
     *
     * n00b_shutdown stops the conduit service, drains to a single thread
     * (measured: 5 -> 1) and clears the runtime, so exit runs against a
     * quiesced process. Every other test in this subsystem already did this;
     * this one was the omission. */
    n00b_shutdown();
    return 0;
}
