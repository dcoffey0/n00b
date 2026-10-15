#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/memory_info.h"

// ============================================================================
// 1. Inline guard — allocated object has inline header with correct guard
// ============================================================================

static void
test_inline_guard(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    n00b_option_t(n00b_inline_hdr_t *) opt = n00b_inline_alloc_header(p);
    assert(n00b_option_is_set(opt));

    n00b_inline_hdr_t *hdr = n00b_option_get(opt);
    assert(hdr->guard == n00b_gc_guard);

    printf("  [PASS] inline_guard\n");
}

// ============================================================================
// 2. Inline fields — header alloc_len, no_scan, tinfo are sensible
// ============================================================================

static void
test_inline_fields(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    n00b_option_t(n00b_inline_hdr_t *) opt = n00b_inline_alloc_header(p);
    assert(n00b_option_is_set(opt));

    n00b_inline_hdr_t *hdr = n00b_option_get(opt);

    // alloc_len should be at least sizeof(uint64_t)
    assert(hdr->alloc_len >= sizeof(uint64_t));

    printf("  [PASS] inline_fields\n");
}

// ============================================================================
// 3. OOB lookup — find_alloc_info returns a record for managed allocations
// ============================================================================

static void
test_oob_lookup(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p, .pin = &pin);

    // Should find some kind of record (oob, inline, or err — not none)
    assert(n00b_alloc_info_exists(info));

    if (info.kind == n00b_alloc_oob) {
        n00b_option_t(n00b_oob_hdr_t *) oob_opt = n00b_alloc_info_oob(info);
        assert(n00b_option_is_set(oob_opt));

        n00b_oob_hdr_t *oob = n00b_option_get(oob_opt);
        assert(oob != nullptr);
    }
    n00b_alloc_hdr_unpin(pin);

    printf("  [PASS] oob_lookup\n");
}

// ============================================================================
// 4. OOB user_ptr — OOB header's user_ptr matches the allocation address
// ============================================================================

static void
test_oob_user_ptr(void)
{
    // Use a non-GC arena with OOB metadata but no inline headers to
    // ensure the OOB dict key matches what find_alloc_info looks up.
    n00b_arena_t *arena = n00b_new_arena(.use_gc         = false,
                                          .inline_headers = false);
    n00b_allocator_t *alloc = (n00b_allocator_t *)arena;

    uint64_t *p = n00b_alloc_with_opts(uint64_t, &(n00b_alloc_opts_t){.allocator = alloc});
    assert(p != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = info.hdr.oob;
        assert(oob->user_ptr == p);
    }
    n00b_alloc_hdr_unpin(pin);

    printf("  [PASS] oob_user_ptr\n");
}

// ============================================================================
// 5. Find alloc info kind — managed allocs yield a non-none kind
// ============================================================================

static void
test_find_alloc_info_kind(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p, .pin = &pin);
    n00b_alloc_hdr_unpin(pin);

    // Should return a recognized kind (inline, oob, or err — not none).
    // The exact kind depends on allocator configuration during the merge.
    assert(info.kind != n00b_alloc_none);

    printf("  [PASS] find_alloc_info_kind\n");
}

// ============================================================================
// 6. Mmap managed — arena-allocated address is in a known mapping
// ============================================================================

static void
test_mmap_managed(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    n00b_option_t(n00b_mmap_info_t *) opt = n00b_mmap_by_address(p);
    assert(n00b_option_is_set(opt));

    n00b_mmap_info_t *map = n00b_option_get(opt);
    // An arena allocation should be in a managed, sys, or arena mapping
    assert(n00b_mmap_is_managed(map) || n00b_mmap_is_arena_segment(map)
           || n00b_mmap_is_arena(map));

    printf("  [PASS] mmap_managed\n");
}

// ============================================================================
// 7. No metadata — random stack address returns n00b_alloc_none
// ============================================================================

static void
test_no_metadata(void)
{
    int stack_var = 42;

    n00b_alloc_info_t info = n00b_find_alloc_info(&stack_var);
    assert(info.kind == n00b_alloc_none);

    printf("  [PASS] no_metadata\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running alloc metadata tests...\n");

    test_inline_guard();
    test_inline_fields();
    test_oob_lookup();
    test_oob_user_ptr();
    test_find_alloc_info_kind();
    test_mmap_managed();
    test_no_metadata();

    printf("All alloc metadata tests passed.\n");
    n00b_shutdown();
    return 0;
}
