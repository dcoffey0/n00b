#include <stdint.h>
#include <string.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/buffer.h"
#include "core/gc_baked.h"
#include "core/hash.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "n00b_crt.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

static void
set_cached_hash(void *obj, n00b_uint128_t hash)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);
    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->cached_hash = hash;
        if (info.hdr.oob->hcur != nullptr) {
            info.hdr.oob->hcur->cached_hash = hash;
        }
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        info.hdr.in_line->cached_hash = hash;
    }
    n00b_alloc_hdr_unpin(pin);
}

static n00b_buffer_t *
make_legacy_shape_buffer(const char *payload, int64_t len)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(buf,
                     .raw = (char *)payload,
                     .length = len,
                     .no_lock = true,
                     .scan_kind = N00B_GC_SCAN_KIND_NONE);
    buf->flags = N00B_BUF_F_BORROWED;

    n00b_uint128_t hash = n00b_buffer_hash(buf);
    CHECK(hash != (n00b_uint128_t)0);
    set_cached_hash(buf, hash);
    return buf;
}

static n00b_buffer_t *
export_one(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) r =
        n00b_ct_image_export(root);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static void *
map_image(n00b_buffer_t *image, size_t *protect_len_out)
{
    size_t protect_len = n00b_page_align(image->byte_len);
    auto map_r = n00b_mmap(protect_len,
                           .kind = n00b_mmap_api_mmap,
                           .name = "buffer-baked-equiv");
    CHECK(n00b_result_is_ok(map_r));
    void *mapping = n00b_result_get(map_r);
    memcpy(mapping, image->data, image->byte_len);
    *protect_len_out = protect_len;
    return mapping;
}

static void
test_buffer_baked_equivalence(void)
{
    static const char payload[] = "wp006-buffer";
    n00b_buffer_t *src = make_legacy_shape_buffer(payload,
                                                  (int64_t)(sizeof(payload) - 1));
    n00b_uint128_t expected_hash = n00b_buffer_hash(src);

    CHECK(n00b_hash(src, nullptr) == expected_hash);
    CHECK(src->byte_len == sizeof(payload) - 1);
    CHECK(src->alloc_len == 16);
    CHECK(src->lock == nullptr);
    CHECK(src->allocator == nullptr);
    CHECK(src->flags == N00B_BUF_F_BORROWED);
    CHECK(src->scan_kind == N00B_GC_SCAN_KIND_NONE);
    CHECK(src->scan_cb == nullptr);
    CHECK(src->scan_user == nullptr);

    n00b_buffer_t *image = export_one(src);
    size_t protect_len = 0;
    void *mapping = map_image(image, &protect_len);

    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_comptime_image_region(mapping, image->byte_len,
                                             protect_len);
    CHECK(n00b_result_is_ok(apply_r));

    n00b_buffer_t *copy = n00b_result_get(apply_r);
    CHECK(copy != src);
    CHECK(copy->data != src->data);
    CHECK(copy->data != nullptr);
    CHECK(copy->byte_len == src->byte_len);
    CHECK(copy->alloc_len == src->alloc_len);
    CHECK(memcmp(copy->data, payload, sizeof(payload) - 1) == 0);
    CHECK(copy->lock == nullptr);
    CHECK(copy->allocator == nullptr);
    CHECK(copy->flags == N00B_BUF_F_BORROWED);
    CHECK(copy->scan_kind == N00B_GC_SCAN_KIND_NONE);
    CHECK(copy->scan_cb == nullptr);
    CHECK(copy->scan_user == nullptr);

    CHECK(n00b_gc_addr_in_baked_region(copy));
    CHECK(n00b_gc_addr_in_baked_region(copy->data));
    auto range_opt = n00b_mmap_range_by_address(copy);
    CHECK(n00b_option_is_set(range_opt));
    n00b_alloc_range_t *range = n00b_option_get(range_opt);
    CHECK(range->cached_hash == expected_hash);
    CHECK(n00b_hash(copy, nullptr) == expected_hash);
    CHECK(n00b_buffer_hash(copy) == expected_hash);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_buffer_baked_equivalence();

    n00b_shutdown();
    return 0;
}
