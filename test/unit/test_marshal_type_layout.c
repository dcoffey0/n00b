#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/codegen_abi.h" // GC/marshal codegen-ABI structs used by value
#include "core/codegen_abi_inject.h" // GC/marshal descriptor structs / scan-cb externs used by value
#include "adt/list.h"
#include "core/alloc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/static_objects.h" // n00b_static_objects_register_all()
#include "util/assert.h"
#include "util/marshal.h"
#include "test_check.h"

#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}

typedef struct type_layout_child_t {
    uint64_t value;
} type_layout_child_t;

typedef struct type_layout_probe_t {
    type_layout_child_t *left;
    uint64_t             scalar_alias;
    type_layout_child_t *right;
} type_layout_probe_t;

typedef struct type_layout_flex_probe_t {
    uint64_t             count;
    type_layout_child_t *items[];
} type_layout_flex_probe_t;

static n00b_buffer_t *
marshal_checked(void *root, uint32_t base_address)
{
    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(.base_address = base_address);
    n00b_buffer_t      *buf = n00b_marshal_incremental(ctx, root);
    n00b_marshal_status_t status = n00b_marshal_ctx_status(ctx);

    CHECK(status == N00B_MARSHAL_OK);
    CHECK(buf != nullptr);
    n00b_marshal_ctx_destroy(ctx);

    return buf;
}

static void *
unmarshal_one_checked(n00b_buffer_t *buf)
{
    n00b_unmarshal_ctx_t *ctx   = n00b_unmarshal_ctx_new();
    n00b_list_t(void *)   roots = n00b_unmarshal_incremental(ctx, buf);
    n00b_marshal_status_t status = n00b_unmarshal_ctx_status(ctx);

    CHECK(status == N00B_MARSHAL_OK);
    CHECK(n00b_list_len(roots) == 1);
    void *root = n00b_list_get(roots, 0);
    CHECK(root != nullptr);
    n00b_unmarshal_ctx_destroy(ctx);

    return root;
}

static void
assert_type_layout_scan(void                         *ptr,
                        const n00b_gc_struct_layout_t *layout,
                        uint32_t                      expected_ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(ptr,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);

    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->tinfo == typehash(type_layout_probe_t *));
    CHECK(info.hdr.oob->ptr_words == expected_ptr_words);
    CHECK(info.hdr.oob->ptr_words_known);
    CHECK(info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(info.hdr.oob->scan_cb == n00b_gc_scan_cb_type_layout);
    CHECK(info.hdr.oob->scan_user == (void *)layout);
    n00b_alloc_hdr_unpin(pin);
}

static void
test_type_layout_unresolved_pointer_rejected(void)
{
    const n00b_gc_struct_layout_t *layout =
        n00b_gc_type_map_lookup(typehash(type_layout_probe_t *));
    CHECK(layout != nullptr);

    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);
    type_layout_probe_t *src = n00b_alloc_with_opts(type_layout_probe_t,
                                                    ARENA_OPTS(arena));
    assert_type_layout_scan(src, layout, 3);

    auto map_r = n00b_mmap(sizeof(uint64_t), .kind = n00b_mmap_api_mmap);
    CHECK(n00b_result_is_ok(map_r));
    void    *external = n00b_result_get(map_r);
    uint32_t ptr_high = (uint32_t)((uint64_t)(uintptr_t)external >> 32);
    CHECK(ptr_high != 0);
    uint32_t other_base = ptr_high + 1u;
    if (other_base == 0) {
        other_base = 1;
    }
    CHECK(other_base != ptr_high);

    src->left         = external;
    src->scalar_alias = 0;
    src->right        = nullptr;
    uint32_t bases[] = {other_base, ptr_high};
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(.base_address = bases[i]);
        CHECK(n00b_marshal_incremental(ctx, src) == nullptr);
        CHECK(n00b_marshal_ctx_status(ctx)
              == N00B_MARSHAL_ERR_UNSUPPORTED_ALLOCATION);
        n00b_marshal_ctx_destroy(ctx);
    }
    CHECK(n00b_result_is_ok(n00b_munmap(external)));
}

static void
test_flex_allocations_keep_default_scan(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);

    type_layout_flex_probe_t *flex =
        n00b_alloc_flex_with_opts(type_layout_flex_probe_t,
                                  type_layout_child_t *,
                                  2,
                                  ARENA_OPTS(arena));
    CHECK(flex != nullptr);
    flex->count = 2;

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(flex,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->tinfo == 0);
    CHECK(info.hdr.oob->ptr_words_known);
    CHECK(info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_DEFAULT);
    CHECK(info.hdr.oob->scan_cb == nullptr);
    CHECK(info.hdr.oob->scan_user == nullptr);
    n00b_alloc_hdr_unpin(pin);
}

static void
test_type_layout_zero_array_round_trip(void)
{
    const n00b_gc_struct_layout_t *layout =
        n00b_gc_type_map_lookup(typehash(type_layout_probe_t *));
    CHECK(layout != nullptr);

    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);
    type_layout_probe_t *items =
        n00b_alloc_array_with_opts(type_layout_probe_t, 0, ARENA_OPTS(arena));
    CHECK(items != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(items,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->ptr_words_known);
    CHECK(info.hdr.oob->ptr_words == 0);
    CHECK(info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(info.hdr.oob->scan_cb == n00b_gc_scan_cb_type_layout);
    n00b_alloc_hdr_unpin(pin);

    uint32_t       base = 0x30313233u;
    n00b_buffer_t *buf  = marshal_checked(items, base);
    type_layout_probe_t *copy = unmarshal_one_checked(buf);
    CHECK(copy != items);
    assert_type_layout_scan(copy, layout, 0);
}

static void
test_type_layout_large_array_records_logical_words(void)
{
    const n00b_gc_struct_layout_t *layout =
        n00b_gc_type_map_lookup(typehash(type_layout_probe_t *));
    CHECK(layout != nullptr);

    size_t words_per_item = sizeof(type_layout_probe_t) / sizeof(void *);
    size_t count          = (UINT32_C(1) << 20) / words_per_item + 1;
    uint32_t expected     = (uint32_t)(count * words_per_item);

    n00b_arena_t *arena = n00b_new_arena(.size = 24 * 1024 * 1024,
                                         .use_gc = true);
    type_layout_probe_t *items =
        n00b_alloc_array_with_opts(type_layout_probe_t, count, ARENA_OPTS(arena));
    CHECK(items != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(items,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->ptr_words_known);
    CHECK(info.hdr.oob->ptr_words == expected);
    CHECK(info.hdr.oob->ptr_words > (UINT32_C(1) << 20));
    CHECK(info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(info.hdr.oob->scan_cb == n00b_gc_scan_cb_type_layout);
    n00b_alloc_hdr_unpin(pin);

    uint32_t       base = 0x34353637u;
    n00b_buffer_t *buf  = marshal_checked(items, base);
    type_layout_probe_t *copy = unmarshal_one_checked(buf);
    CHECK(copy != items);
    assert_type_layout_scan(copy, layout, expected);
}

static void
test_type_layout_grown_backing_round_trip(void)
{
    const n00b_gc_struct_layout_t *layout =
        n00b_gc_type_map_lookup(typehash(type_layout_probe_t *));
    CHECK(layout != nullptr);
    CHECK(n00b_gc_type_map_hash_for_layout(layout) == typehash(type_layout_probe_t *));

    n00b_arena_t *arena = n00b_new_arena(.size = 16384, .use_gc = true);

    type_layout_child_t *left = n00b_alloc_with_opts(type_layout_child_t,
                                                     ARENA_OPTS(arena));
    type_layout_child_t *right = n00b_alloc_with_opts(type_layout_child_t,
                                                      ARENA_OPTS(arena));
    left->value  = UINT64_C(0x1111222233334444);
    right->value = UINT64_C(0x5555666677778888);

    n00b_list_t(type_layout_probe_t) list =
        n00b_list_new_private(type_layout_probe_t,
                              .allocator = (n00b_allocator_t *)arena);

    CHECK(list.scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(list.scan_cb == n00b_gc_scan_cb_type_layout);
    CHECK(list.scan_user == (void *)layout);

    for (int i = 0; i < 20; i++) {
        type_layout_probe_t item = {
            .left         = (i & 1) ? right : left,
            .scalar_alias = (uint64_t)(uintptr_t)((i & 1) ? left : right),
            .right        = (i & 1) ? left : right,
        };
        n00b_list_push(list, item);
    }
    CHECK(list.cap > N00B_DEFAULT_LIST_SZ);

    bool              backing_pin  = false;
    n00b_alloc_info_t backing_info = n00b_find_alloc_info(list.data,
                                                          .scan_for_header = true,
                                                          .pin             = &backing_pin);
    CHECK(backing_info.kind == n00b_alloc_oob);
    // The grown backing array must keep the type-layout scan callback and
    // descriptor. Its raw backing-store tinfo may be a compiler-generated
    // array identity; marshal canonicalizes TYPE_LAYOUT objects to the
    // descriptor hash before writing the stream.
    CHECK(backing_info.hdr.oob->tinfo != 0);
    CHECK(backing_info.hdr.oob->ptr_words_known);
    CHECK(backing_info.hdr.oob->ptr_words
          == (list.cap * sizeof(type_layout_probe_t)) / sizeof(void *));
    CHECK(backing_info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(backing_info.hdr.oob->scan_cb == n00b_gc_scan_cb_type_layout);
    CHECK(backing_info.hdr.oob->scan_user == (void *)layout);
    n00b_alloc_hdr_unpin(backing_pin);

    uint32_t       base = 0x61626364u;
    n00b_buffer_t *buf  = marshal_checked(list.data, base);

    type_layout_probe_t *copy = unmarshal_one_checked(buf);
    CHECK(copy != list.data);
    CHECK(copy[0].left != left);
    CHECK(copy[0].right != right);
    CHECK(copy[0].left->value == left->value);
    CHECK(copy[0].right->value == right->value);
    CHECK(copy[0].scalar_alias == (uint64_t)(uintptr_t)right);
    CHECK(copy[0].scalar_alias != (uint64_t)(uintptr_t)copy[0].right);
    assert_type_layout_scan(copy, layout, backing_info.hdr.oob->ptr_words);

    n00b_buffer_t *buf2 = marshal_checked(copy, base);
    CHECK(buf->byte_len == buf2->byte_len);
    CHECK(memcmp(buf->data, buf2->data, buf->byte_len) == 0);

}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // Load ncc-emitted static GC-map descriptors (incl. this TU's typed
    // structs, aggregated into the gen_table by the --ncc-gcmap-prelink link)
    // into the runtime type map so typehash() lookups resolve.
    n00b_static_objects_register_all();

    test_type_layout_unresolved_pointer_rejected();
    test_type_layout_grown_backing_round_trip();
    test_flex_allocations_keep_default_scan();
    test_type_layout_zero_array_round_trip();
    test_type_layout_large_array_records_logical_words();

    n00b_shutdown();
    return 0;
}
