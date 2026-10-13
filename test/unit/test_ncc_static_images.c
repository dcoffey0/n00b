#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "adt/array.h"
#include "adt/list.h"
#include "core/alloc.h"
#include "adt/dict_untyped.h"
#include "core/buffer.h"
#include "core/gc_map.h"
#include "core/hash.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/static_image.h"
#include "core/static_objects.h"
#include "core/type_info.h"
#include "text/strings/string_ops.h"

const n00b_buffer_t *readonly_buffer =
    b"phase-six";
const n00b_buffer_t *hex_buffer =
    b"hi";
const n00b_buffer_t *sized_buffer =
    b"\0\0\0";
const n00b_buffer_t *raw_buffer =
    b"raw";
const n00b_buffer_t *literal_buffer =
    b"literal";

// WP-011 Phase 5f regression: two content-equal standalone `b"..."`
// literal emissions at different call sites must populate the buffer
// object descriptor's `.cached_hash` slot with the same XXH3_128bits
// (matching `n00b_buffer_hash` exactly).  Before Phase 5f only the
// dict-key buffer-emission path populated this slot; standalone
// emissions left it at zero and the runtime `n00b_hash()` had to
// recompute on every call, producing the same value but via a
// different code path (vtable fallback) that elided the descriptor's
// cached-hash short-circuit.  Post-Phase 5f the descriptor itself
// carries the cached hash for every emission, so both lvalues see the
// short-circuit on first lookup.
const n00b_buffer_t *phase5f_buffer_a =
    b"phase5f";
const n00b_buffer_t *phase5f_buffer_b =
    b"phase5f";
const n00b_buffer_t *phase5f_empty_buffer =
    b"";

n00b_list_t(int) static_int_list =
    l{1, 2, 3};
typedef n00b_list_t(int) ncc_static_int_list_t;
typedef n00b_array_t(int) ncc_static_int_array_t;
ncc_static_int_array_t static_int_array =
    a{7, 8, 9};

static n00b_alloc_range_t *
range_for_address(const void *addr)
{
    auto found = n00b_mmap_range_by_address((void *)addr);
    assert(n00b_option_is_set(found));
    return n00b_option_get(found);
}

static void
assert_static_image_identity(n00b_alloc_range_t *range,
                             n00b_static_identity_kind_t kind,
                             const char *key_prefix)
{
    assert(range->identity != nullptr);
    assert(range->identity->version == N00B_STATIC_IDENTITY_VERSION);
    assert(range->identity->kind == kind);
    assert(range->identity->namespace_id != nullptr);
    assert(strcmp(range->identity->namespace_id, "n00b.tests") == 0);
    assert(range->identity->object_key != nullptr);
    assert(strncmp(range->identity->object_key,
                   key_prefix,
                   strlen(key_prefix)) == 0);
    assert(strstr(range->identity->object_key,
                  "test_ncc_static_images.c:") != nullptr);

    n00b_static_identity_query_t query = {
        .checks      = N00B_STATIC_IDENTITY_CHECK_LEN
                     | N00B_STATIC_IDENTITY_CHECK_SCAN_KIND
                     | N00B_STATIC_IDENTITY_CHECK_FLAGS,
        .len         = range->len,
        .scan_kind   = range->scan_kind,
        .flags_mask  = range->flags,
        .flags_value = range->flags,
    };
    n00b_alloc_range_t *resolved = nullptr;
    assert(n00b_static_identity_lookup(range->identity, &query, &resolved)
           == N00B_STATIC_IDENTITY_OK);
    assert(resolved == range);
}

static n00b_static_layout_info_t *
require_layout(uint64_t type_hash)
{
    auto layout_opt = n00b_type_static_layout(type_hash);
    assert(n00b_option_is_set(layout_opt));
    return n00b_option_get(layout_opt);
}

static uint64_t
expected_capacity(uint64_t len)
{
    if (len == 0) {
        return N00B_EMPTY_BUFFER_ALLOC;
    }

    uint64_t cap = 1;
    while (cap < len) {
        cap <<= 1;
    }
    return cap;
}

static void
assert_buffer_payload(const n00b_buffer_t *buf, const char *expected,
                      uint64_t len)
{
    assert(buf != nullptr);
    assert(buf->byte_len == len);
    assert(buf->alloc_len == expected_capacity(len));
    assert(buf->lock == nullptr);
    assert(buf->allocator == nullptr);
    assert(buf->flags & N00B_BUF_F_BORROWED);
    assert(buf->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(buf->scan_cb == nullptr);
    assert(buf->scan_user == nullptr);

    if (len != 0) {
        assert(memcmp(buf->data, expected, (size_t)len) == 0);
    }
    for (uint64_t i = len; i < buf->alloc_len; i++) {
        assert(buf->data[i] == 0);
    }
}

static void
assert_static_buffer_range(const n00b_buffer_t *buf)
{
    n00b_alloc_range_t *object_range = range_for_address(buf);
    assert(object_range->kind == n00b_mmap_static);
    assert(object_range->len == sizeof(*buf));
    assert(object_range->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(object_range->scan_cb != nullptr);
    assert(object_range->scan_user != nullptr);
    assert(object_range->flags & N00B_STATIC_OBJECT_F_READONLY);
    assert(object_range->flags & N00B_STATIC_OBJECT_F_BAKED);

    uint64_t word_count  = sizeof(*buf) / sizeof(void *);
    uint64_t bitmap[1]   = {0};
    n00b_gc_map_t gc_map = {
        .user_ptr  = (void *)buf,
        .num_words = word_count,
        .bitmap    = bitmap,
    };
    object_range->scan_cb(&gc_map, object_range->scan_user);
    for (uint64_t i = 0; i < word_count; i++) {
        uint64_t expected = i == offsetof(n00b_buffer_t, data) / sizeof(void *);
        assert(n00b_gc_map_is_set(&gc_map, i) == expected);
    }

    n00b_alloc_range_t *payload_range = range_for_address(buf->data);
    assert(payload_range->kind == n00b_mmap_static);
    assert(payload_range->len >= buf->alloc_len);
    assert(payload_range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(payload_range->flags & N00B_STATIC_OBJECT_F_READONLY);
    assert(payload_range->flags & N00B_STATIC_OBJECT_F_BAKED);
}

typedef struct {
    n00b_alloc_type_info_t tinfo;
    uint32_t               alloc_len;
    n00b_gc_scan_kind_t    scan_kind;
    n00b_uint128_t         cached_hash;
} test_alloc_fields_t;

static test_alloc_fields_t
heap_alloc_fields(void *ptr)
{
    bool                pin  = false;
    n00b_alloc_info_t   info = n00b_find_alloc_info(ptr, .pin = &pin);
    test_alloc_fields_t fields;
    assert(n00b_alloc_info_is_heap(info));

    if (info.kind == n00b_alloc_inline) {
        fields = (test_alloc_fields_t){
            .tinfo       = info.hdr.in_line->tinfo,
            .alloc_len   = info.hdr.in_line->alloc_len,
            .scan_kind   = (n00b_gc_scan_kind_t)info.hdr.in_line->scan_kind,
            .cached_hash = info.hdr.in_line->cached_hash,
        };
    }
    else {
        fields = (test_alloc_fields_t){
            .tinfo       = info.hdr.oob->tinfo,
            .alloc_len   = info.hdr.oob->alloc_len,
            .scan_kind   = (n00b_gc_scan_kind_t)info.hdr.oob->scan_kind,
            .cached_hash = info.hdr.oob->cached_hash,
        };
    }
    n00b_alloc_hdr_unpin(pin);
    return fields;
}

static void
assert_migrated_buffer_allocation(const n00b_buffer_t *buf)
{
    test_alloc_fields_t obj_info =
        heap_alloc_fields((void *)buf);
    assert(obj_info.tinfo == typehash(n00b_buffer_t *));
    assert(obj_info.alloc_len >= sizeof(*buf));

    test_alloc_fields_t payload_info =
        heap_alloc_fields((void *)buf->data);
    assert(payload_info.alloc_len >= buf->alloc_len);
    assert(payload_info.scan_kind == N00B_GC_SCAN_KIND_NONE);

    if (buf->byte_len != 0) {
        assert(obj_info.cached_hash != (n00b_uint128_t)0);
    }

    assert(n00b_hash((void *)buf, nullptr)
           == n00b_buffer_hash((n00b_buffer_t *)buf));
}

static void
assert_static_list_lock_range(n00b_rwlock_t *lock)
{
    assert(lock != nullptr);
    n00b_alloc_range_t *range = range_for_address(lock);
    assert(range->kind == n00b_mmap_static);
    assert(range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(range->flags & N00B_STATIC_OBJECT_F_INIT_RWLOCK);
}

static void
assert_static_int_list_data_range(const ncc_static_int_list_t *list)
{
    assert(list->data != nullptr);
    n00b_alloc_range_t *range = range_for_address(list->data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == list->cap * sizeof(*list->data));
    assert(range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
}

static void
assert_static_int_array_data_range(const ncc_static_int_array_t *array)
{
    assert(array->data != nullptr);
    n00b_alloc_range_t *range = range_for_address(array->data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == array->cap * sizeof(*array->data));
    assert(range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert_static_image_identity(range,
                                 N00B_STATIC_IDENTITY_NCC_ARRAY_DATA,
                                 "ncc-array-data:");
}

static void
assert_int_list_contents(const ncc_static_int_list_t *list,
                         const int *expected,
                         size_t expected_len)
{
    assert(list != nullptr);
    assert(list->len == expected_len);
    assert(list->cap == N00B_DEFAULT_LIST_SZ);
    assert(list->lock != nullptr);
    assert(list->allocator == nullptr);
    assert(list->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(list->scan_cb == nullptr);
    assert(list->scan_user == nullptr);

    for (size_t i = 0; i < expected_len; i++) {
        assert(list->data[i] == expected[i]);
    }
}

static void
test_generated_static_image_registration(void)
{
    n00b_static_objects_register_all();

    assert_buffer_payload(readonly_buffer, "phase-six", 9);
    assert_buffer_payload(hex_buffer, "hi", 2);
    assert_buffer_payload(sized_buffer, "\0\0\0", 3);
    assert_buffer_payload(raw_buffer, "raw", 3);
    assert_buffer_payload(literal_buffer, "literal", 7);

    assert_migrated_buffer_allocation(readonly_buffer);
    assert_migrated_buffer_allocation(hex_buffer);
    assert_migrated_buffer_allocation(sized_buffer);
    assert_migrated_buffer_allocation(raw_buffer);
    assert_migrated_buffer_allocation(literal_buffer);

    n00b_buffer_t *heap_buffer = n00b_buffer_from_bytes("heap", 4);
    assert(heap_buffer->byte_len == 4);
    assert(memcmp(heap_buffer->data, "heap", 4) == 0);
    assert((heap_buffer->flags & N00B_BUF_F_BORROWED) == 0);

    printf("  [PASS] generated static image registration\n");
}

static void
test_generated_static_list_literals(void)
{
    n00b_static_objects_register_all();

    int value_expected[] = {1, 2, 3};
    assert_int_list_contents(&static_int_list,
                             value_expected,
                             sizeof(value_expected) / sizeof(value_expected[0]));
    n00b_list_push(static_int_list, 99);
    assert(static_int_list.len == 4);
    assert(n00b_list_get(static_int_list, 3) == 99);


    printf("  [PASS] generated static list literals\n");
}

static void
test_generated_static_array_literals(void)
{
    n00b_static_objects_register_all();

    assert(static_int_array.len == 3);
    assert(static_int_array.cap == 3);
    assert(static_int_array.lock == nullptr);
    assert(static_int_array.allocator == nullptr);
    assert(static_int_array.scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(static_int_array.scan_cb == nullptr);
    assert(static_int_array.scan_user == nullptr);
    assert(static_int_array.data[0] == 7);
    assert(static_int_array.data[1] == 8);
    assert(static_int_array.data[2] == 9);

    static_int_array.data[1] = 88;
    assert(static_int_array.data[1] == 88);

    printf("  [PASS] generated static array literals\n");
}

static void
test_phase5f_buffer_cached_hash_uniformity(void)
{
    n00b_static_objects_register_all();

    // Sanity: both emissions point to distinct buffer descriptors
    // (each `b"..."` produces its own static-image record) but the
    // payload bytes are content-equal.
    assert(phase5f_buffer_a != phase5f_buffer_b);
    assert(phase5f_buffer_a->byte_len == 7);
    assert(phase5f_buffer_b->byte_len == 7);
    assert(memcmp(phase5f_buffer_a->data, "phase5f", 7) == 0);
    assert(memcmp(phase5f_buffer_b->data, "phase5f", 7) == 0);

    // The runtime contract: `n00b_hash(buffer_ptr, nullptr)` must
    // return the same value for any two content-equal `b"..."`
    // emissions.  Pre-Phase-5f this was already true at the algorithm
    // level (the vtable fallback hashes raw bytes), but the
    // descriptor's `.cached_hash` slot was zero for non-dict-key
    // emissions, so the short-circuit in `n00b_hash` was a no-op for
    // those sites.  Post-Phase-5f the slot is populated, so first-
    // lookup hash returns the cached value and matches across sites.
    n00b_uint128_t ha = n00b_hash((void *)phase5f_buffer_a, nullptr);
    n00b_uint128_t hb = n00b_hash((void *)phase5f_buffer_b, nullptr);
    assert(ha == hb);
    assert(ha != 0);

    // Equivalence with the direct `n00b_buffer_hash`:
    n00b_uint128_t ref =
        n00b_buffer_hash((n00b_buffer_t *)phase5f_buffer_a);
    assert(ha == ref);

    // Empty buffer caveat: an empty `b""` literal keeps
    // cached_hash = 0 by design (algorithm parity with
    // `n00b_buffer_hash`'s `n00b_hash_word(0ULL)` fallback we cannot
    // reproduce at ncc compile time without depending on
    // libn00b's `n00b_word_t` layout).  The runtime still returns the
    // correct value because `n00b_hash` falls back to the vtable
    // (recompute) when the cached slot is zero.
    assert(phase5f_empty_buffer->byte_len == 0);
    n00b_uint128_t empty_hash =
        n00b_hash((void *)phase5f_empty_buffer, nullptr);
    n00b_uint128_t empty_ref =
        n00b_buffer_hash((n00b_buffer_t *)phase5f_empty_buffer);
    assert(empty_hash == empty_ref);

    printf("  [PASS] phase 5f buffer cached_hash uniformity\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_static_layout_info_t *layout =
        require_layout(typehash(n00b_buffer_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);

    printf("Running ncc static image tests...\n");
    test_generated_static_image_registration();
    test_generated_static_list_literals();
    test_generated_static_array_literals();
    test_phase5f_buffer_cached_hash_uniformity();
    printf("All ncc static image tests passed.\n");

    n00b_shutdown();

    return 0;
}
