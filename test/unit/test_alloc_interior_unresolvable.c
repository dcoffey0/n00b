/*
 * test_alloc_interior_unresolvable.c — n00b#327.
 *
 * n00b_find_alloc_info(.scan_for_header = true) promises to resolve an
 * interior pointer back to its allocation. It delivers that by scanning
 * backward for an inline header's sentinel, which an allocator with OOB
 * metadata and NO inline headers does not have. The OOB metadata dict is
 * keyed by the exact allocation base (core/oob_md_dict.h), so an interior
 * address just misses.
 *
 * That miss used to be reported as n00b_alloc_err — the identical answer to
 * "this address is in free space", which conservative stack scanning
 * produces constantly (gc.c's visit path treats it as nothing-to-do). The
 * two are not the same: free space means there is nothing to mark, whereas
 * an unresolvable interior pointer may be the only reference to a live
 * object, and dropping it lets the post-mark sweep reclaim that object
 * while a live root still points into it.
 *
 * This test pins the distinction. It does NOT assert that interior
 * resolution works for this allocator shape — it cannot, and #327 records
 * why: that needs a "greatest base <= addr" lookup and the metadata is an
 * unordered hash dict.
 *
 * .alloc_refcount is the way to get this shape: pool.c forces
 * external_metadata on and inline_headers off for it.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/pool.h"
#include "core/runtime.h"

/*
 * OOB metadata + no inline headers: interior pointers are unresolvable, and
 * must say so rather than looking like free space.
 */
static void
test_oob_without_inline_header(void)
{
    n00b_pool_t       pool;
    n00b_allocator_t *alloc = n00b_pool_init(&pool,
                                             .alloc_refcount = true,
                                             .name = "t327_oob_no_inline");
    assert(alloc != nullptr);

    /* Precondition: this really is the shape under test. */
    assert(pool.vtable.metadata_pool != nullptr);
    assert(!pool.vtable.add_inline_header);

    uint8_t *p = n00b_alloc_array_with_opts(uint8_t,
                                            64,
                                            &(n00b_alloc_opts_t){.allocator = alloc});
    assert(p != nullptr);
    memset(p, 0x5A, 64);

    /* The base address resolves normally — the exact-key lookup hits. */
    bool              pin  = false;
    n00b_alloc_info_t base = n00b_find_alloc_info(p, .allocator = alloc, .pin = &pin);
    assert(base.kind == n00b_alloc_oob);
    assert(n00b_alloc_info_is_heap(base));
    n00b_alloc_hdr_unpin(pin);
    printf("  [PASS] base pointer resolves (kind=oob)\n");

    /* An interior pointer cannot resolve — but must be distinguishable. */
    n00b_alloc_info_t interior = n00b_find_alloc_info(p + 16,
                                                     .allocator      = alloc,
                                                     .scan_for_header = true);
    if (interior.kind == n00b_alloc_err) {
        printf("  [FAIL] interior pointer reported as n00b_alloc_err, which is "
               "indistinguishable from free space (n00b#327)\n");
        assert(!"interior miss must not be conflated with a plain miss");
    }
    assert(interior.kind == n00b_alloc_interior_unresolvable);
    assert(n00b_alloc_info_is_interior_unresolvable(interior));

    /* Still not a heap answer: callers must not treat it as resolved. */
    assert(!n00b_alloc_info_is_heap(interior));
    printf("  [PASS] interior pointer reported as interior_unresolvable\n");

    n00b_alloc_unref(p);
}

/*
 * Control: the ordinary inline-header path still resolves interior pointers.
 * Without this, the test above would pass just as well if resolution were
 * broken everywhere.
 */
static void
test_inline_header_still_resolves_interior(void)
{
    n00b_pool_t       pool;
    n00b_allocator_t *alloc = n00b_pool_init(&pool,
                                             .inline_headers = true,
                                             .name = "t327_inline");
    assert(alloc != nullptr);
    assert(pool.vtable.add_inline_header);

    uint8_t *p = n00b_alloc_array_with_opts(uint8_t,
                                            64,
                                            &(n00b_alloc_opts_t){.allocator = alloc});
    assert(p != nullptr);

    n00b_alloc_info_t interior = n00b_find_alloc_info(p + 16,
                                                     .allocator      = alloc,
                                                     .scan_for_header = true);
    assert(n00b_alloc_info_is_heap(interior));
    assert(!n00b_alloc_info_is_interior_unresolvable(interior));
    printf("  [PASS] inline-header allocator still resolves interior pointers\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_oob_without_inline_header();
    test_inline_header_still_resolves_interior();

    printf("\nn00b#327: interior-pointer contract holds.\n");

    n00b_shutdown();
    return 0;
}
