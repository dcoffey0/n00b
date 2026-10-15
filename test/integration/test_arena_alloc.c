#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/alloc_mdata.h"
#include "core/arena.h"
#include "core/mmaps.h"

// ============================================================================
// 1. Basic alloc — returns non-null, 16-byte aligned pointer
// ============================================================================

static void
test_basic_alloc(void)
{
    uint64_t *p = n00b_alloc(uint64_t);

    assert(p != nullptr);
    assert(((uintptr_t)p & (N00B_ALIGN - 1)) == 0);

    printf("  [PASS] basic_alloc\n");
}

// ============================================================================
// 2. Zero filled — allocated memory is zeroed
// ============================================================================

static void
test_zero_filled(void)
{
    uint64_t *p = n00b_alloc_array(uint64_t, 16);

    assert(p != nullptr);

    for (int i = 0; i < 16; i++) {
        assert(p[i] == 0);
    }

    printf("  [PASS] zero_filled\n");
}

// ============================================================================
// 3. Array alloc — allocate array of 100 ints, use them
// ============================================================================

static void
test_array_alloc(void)
{
    int *arr = n00b_alloc_array(int, 100);

    assert(arr != nullptr);

    for (int i = 0; i < 100; i++) {
        arr[i] = i * 7;
    }

    for (int i = 0; i < 100; i++) {
        assert(arr[i] == i * 7);
    }

    printf("  [PASS] array_alloc\n");
}

// ============================================================================
// 4. Inline header — header has valid guard
// ============================================================================

static void
test_inline_header(void)
{
    uint64_t *p = n00b_alloc(uint64_t);

    assert(p != nullptr);

    n00b_option_t(n00b_inline_hdr_t *) opt = n00b_inline_alloc_header(p);
    assert(n00b_option_is_set(opt));

    n00b_inline_hdr_t *hdr = n00b_option_get(opt);
    assert(hdr->guard == n00b_gc_guard);

    printf("  [PASS] inline_header\n");
}

// ============================================================================
// 5. Alloc info — find_alloc_info returns info with kind != n00b_alloc_none
// ============================================================================

static void
test_alloc_info(void)
{
    uint64_t *p = n00b_alloc(uint64_t);

    assert(p != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p, .pin = &pin);
    n00b_alloc_hdr_unpin(pin);
    assert(info.kind != n00b_alloc_none);

    printf("  [PASS] alloc_info\n");
}

// ============================================================================
// 6. Multiple allocs — 50 sequential allocs return distinct pointers
// ============================================================================

static void
test_multiple_allocs(void)
{
    void *ptrs[50];

    for (int i = 0; i < 50; i++) {
        ptrs[i] = n00b_alloc(uint64_t);
        assert(ptrs[i] != nullptr);
    }

    // All pointers should be distinct
    for (int i = 0; i < 50; i++) {
        for (int j = i + 1; j < 50; j++) {
            assert(ptrs[i] != ptrs[j]);
        }
    }

    printf("  [PASS] multiple_allocs\n");
}

// ============================================================================
// 7. Arena metrics — used increases after alloc, size >= used
// ============================================================================

static void
test_arena_metrics(void)
{
    // Create a fresh non-GC arena to get clean metrics
    n00b_arena_t *arena = n00b_new_arena(.use_gc = false, .inline_headers = true);
    assert(arena != nullptr);

    n00b_allocator_t *alloc = (n00b_allocator_t *)arena;

    // Allocate some data through the arena
    void *p = n00b_alloc_with_opts(uint64_t, &(n00b_alloc_opts_t){.allocator = alloc});
    assert(p != nullptr);

    // The arena should have some used space
    n00b_segment_t *seg  = n00b_atomic_load(&arena->current_segment);
    char           *next = n00b_atomic_load(&arena->next_alloc);
    size_t          used = (size_t)(next - (char *)seg->data);

    assert(used > 0);
    assert(seg->size >= used);

    printf("  [PASS] arena_metrics\n");
}

// ============================================================================
// 8. Large alloc — allocating a big object on a non-GC arena works
// ============================================================================

static void
test_large_alloc(void)
{
    // Use a non-GC arena to avoid triggering collection (GC is being ported)
    n00b_arena_t     *arena = n00b_new_arena(.use_gc = false, .inline_headers = true);
    n00b_allocator_t *alloc = (n00b_allocator_t *)arena;

    // Allocate a large array (64 KB)
    char *p = n00b_alloc_array_with_opts(char, 64 * 1024, &(n00b_alloc_opts_t){.allocator = alloc});

    assert(p != nullptr);
    assert(((uintptr_t)p & (N00B_ALIGN - 1)) == 0);

    // Write to first and last byte to verify the range is usable
    p[0]                 = 'A';
    p[(64 * 1024) - 1]  = 'Z';
    assert(p[0] == 'A');
    assert(p[(64 * 1024) - 1] == 'Z');

    printf("  [PASS] large_alloc\n");
}

// ============================================================================
// 9. Flex alloc — struct with trailing flexible array member
// ============================================================================

typedef struct {
    uint64_t count;
    uint64_t flags;
} flex_header_t;

typedef struct {
    int value;
} flex_item_t;

static void
test_flex_alloc(void)
{
    flex_header_t *p = n00b_alloc_flex(flex_header_t, flex_item_t, 10);

    assert(p != nullptr);

    p->count = 10;
    p->flags = 0xFF;

    flex_item_t *items = (flex_item_t *)(p + 1);
    for (int i = 0; i < 10; i++) {
        items[i].value = i * 3;
    }

    assert(p->count == 10);
    assert(p->flags == 0xFF);
    for (int i = 0; i < 10; i++) {
        assert(items[i].value == i * 3);
    }

    printf("  [PASS] flex_alloc\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running arena alloc tests...\n");

    test_basic_alloc();
    test_zero_filled();
    test_array_alloc();
    test_inline_header();
    test_alloc_info();
    test_multiple_allocs();
    test_arena_metrics();
    test_large_alloc();
    test_flex_alloc();

    printf("All arena alloc tests passed.\n");
    n00b_shutdown();
    return 0;
}
