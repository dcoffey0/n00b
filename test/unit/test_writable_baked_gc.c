#include <stdint.h>
#include <string.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/buffer.h"
#include "core/gc.h"
#include "core/gc_baked.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "n00b_crt.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

typedef struct writable_gc_heap_node_t {
    uint64_t tag;
} writable_gc_heap_node_t;

typedef struct writable_gc_image_root_t {
    writable_gc_heap_node_t *heap;
    uint64_t                 tag;
} writable_gc_image_root_t;

/* Describe the object as a prefix of `ptr_words` pointer words.
 *
 * The scan kind has to be reset alongside the count. ncc's gc-typemap gives
 * this struct a TYPE_LAYOUT descriptor (a CALLBACK scan whose stride is the
 * whole struct), and the collector takes `ptr_words` as the number of words to
 * scan when `ptr_words_known` is set. Leaving both in place describes one
 * object two incompatible ways: the layout callback is handed a length of
 * `ptr_words`, and n00b_gc_map_mark_type_layout rejects a length that is not a
 * whole number of strides. Writing the prefix description means writing all of
 * it. */
static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words       = ptr_words;
        info.hdr.oob->ptr_words_known = 1;
        info.hdr.oob->scan_kind       = N00B_GC_SCAN_KIND_DEFAULT;
        info.hdr.oob->scan_cb         = nullptr;
        info.hdr.oob->scan_user       = nullptr;
        if (info.hdr.oob->hcur != nullptr) {
            info.hdr.oob->hcur->ptr_words       = ptr_words;
            info.hdr.oob->hcur->ptr_words_known = 1;
            info.hdr.oob->hcur->scan_kind       = N00B_GC_SCAN_KIND_DEFAULT;
            info.hdr.oob->hcur->scan_cb         = nullptr;
            info.hdr.oob->hcur->scan_user       = nullptr;
        }
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        info.hdr.in_line->ptr_words       = ptr_words;
        info.hdr.in_line->ptr_words_known = 1;
        info.hdr.in_line->scan_kind       = N00B_GC_SCAN_KIND_DEFAULT;
        info.hdr.in_line->scan_cb         = nullptr;
        info.hdr.in_line->scan_user       = nullptr;
    }
    n00b_alloc_hdr_unpin(pin);
}

static void *
map_image(n00b_buffer_t *image)
{
    size_t map_len = n00b_page_align(image->byte_len);
    auto map_r = n00b_mmap(map_len,
                           .kind = n00b_mmap_api_mmap,
                           .name = "ct-writable-gc");
    CHECK(n00b_result_is_ok(map_r));
    void *mapping = n00b_result_get(map_r);
    memcpy(mapping, image->data, image->byte_len);
    return mapping;
}

static writable_gc_image_root_t *
apply_writable_root(void)
{
    writable_gc_image_root_t *root = n00b_alloc(writable_gc_image_root_t);
    set_ptr_words(root, 1);
    root->heap = nullptr;
    root->tag  = UINT64_C(0x57424743524f4f54);

    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) export_r =
        n00b_ct_image_export_writable(root);
    CHECK(n00b_result_is_ok(export_r));
    n00b_buffer_t *image = n00b_result_get(export_r);
    void *mapping = map_image(image);

    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_writable_image_region(mapping, image->byte_len, nullptr);
    CHECK(n00b_result_is_ok(apply_r));
    return n00b_result_get(apply_r);
}

/* Allocate the heap node, publish it through the baked root, and hand back
 * only a masked copy of its address.
 *
 * noinline so the raw pointer never exists in the caller's frame. Assigning
 * nullptr to a local afterwards does not retract it: the value can still sit
 * in a spill slot or a callee-saved register, and the conservative scan reads
 * both, which pins the node and defeats the relocation this test exists to
 * observe. The mask keeps the caller's surviving copy from looking like an
 * address. Same idiom as test_gc.c::allocate_waste and
 * test_gc_auto_roots_e2e::populate_unrooted_singleton. */
static __attribute__((noinline)) uintptr_t
stash_heap_node(n00b_arena_t             *arena,
                writable_gc_image_root_t *root,
                uint64_t                  tag,
                uintptr_t                 mask)
{
    writable_gc_heap_node_t *heap = n00b_alloc_with_opts(
        writable_gc_heap_node_t,
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)arena,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });
    heap->tag  = tag;
    root->heap = heap;

    return (uintptr_t)heap ^ mask;
}

static void
test_post_relocation_write_is_tracked_by_gc(void)
{
    writable_gc_image_root_t *root = apply_writable_root();
    uintptr_t root_addr_xor = (uintptr_t)root ^ UINT64_C(0xfeedfacecafebeef);
    CHECK(n00b_gc_addr_in_baked_region(root));

    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);
    uintptr_t heap_addr_xor = stash_heap_node(arena,
                                              root,
                                              UINT64_C(0x5742474348454150),
                                              UINT64_C(0x9e3779b97f4a7c15));

    n00b_collect(arena);

    CHECK(((uintptr_t)root ^ UINT64_C(0xfeedfacecafebeef)) == root_addr_xor);
    CHECK(root->heap != nullptr);
    if (!n00b_gc_pin_all_policy()) { // pin-all: heap object kept in place
        CHECK(((uintptr_t)root->heap ^ UINT64_C(0x9e3779b97f4a7c15)) != heap_addr_xor);
    }
    CHECK(root->heap->tag == UINT64_C(0x5742474348454150));
    CHECK(n00b_gc_addr_in_baked_region(root));
    CHECK(!n00b_gc_addr_in_baked_region(root->heap));

    uintptr_t heap2_addr_xor = stash_heap_node(arena,
                                               root,
                                               UINT64_C(0x5742474348454132),
                                               UINT64_C(0xd1b54a32d192ed03));

    n00b_collect(arena);

    CHECK(((uintptr_t)root ^ UINT64_C(0xfeedfacecafebeef)) == root_addr_xor);
    CHECK(root->heap != nullptr);
    if (!n00b_gc_pin_all_policy()) { // pin-all: heap object kept in place
        CHECK(((uintptr_t)root->heap ^ UINT64_C(0xd1b54a32d192ed03)) != heap2_addr_xor);
    }
    CHECK(root->heap->tag == UINT64_C(0x5742474348454132));
    CHECK(n00b_gc_addr_in_baked_region(root));
    CHECK(!n00b_gc_addr_in_baked_region(root->heap));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_post_relocation_write_is_tracked_by_gc();

    n00b_shutdown();
    return 0;
}
