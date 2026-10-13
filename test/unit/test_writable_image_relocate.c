#include <stdint.h>
#include <string.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/gc_baked.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "n00b_crt.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

typedef struct writable_image_node_t {
    struct writable_image_node_t *next;
    uint64_t                      tag;
} writable_image_node_t;

static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words = ptr_words;
        if (info.hdr.oob->hcur != nullptr) {
            info.hdr.oob->hcur->ptr_words = ptr_words;
        }
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        info.hdr.in_line->ptr_words = ptr_words;
    }
    n00b_alloc_hdr_unpin(pin);
}

static void *
map_image(n00b_buffer_t *image)
{
    size_t map_len = n00b_page_align(image->byte_len);
    auto map_r = n00b_mmap(map_len,
                           .kind = n00b_mmap_api_mmap,
                           .name = "ct-writable-image");
    CHECK(n00b_result_is_ok(map_r));
    void *mapping = n00b_result_get(map_r);
    memcpy(mapping, image->data, image->byte_len);
    return mapping;
}

static void
test_writable_image_relocates_and_stays_writable(void)
{
    writable_image_node_t *left  = n00b_alloc(writable_image_node_t);
    writable_image_node_t *right = n00b_alloc(writable_image_node_t);
    set_ptr_words(left, 1);
    set_ptr_words(right, 1);
    left->next  = right;
    right->next = left;
    left->tag   = UINT64_C(0x5752495445303031);
    right->tag  = UINT64_C(0x5752495445303032);

    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) export_r =
        n00b_ct_image_export_writable(left);
    CHECK(n00b_result_is_ok(export_r));
    n00b_buffer_t *image = n00b_result_get(export_r);
    n00b_ct_image_header_t *hdr = (void *)image->data;
    CHECK((hdr->flags & N00B_CT_IMAGE_FLAG_WRITABLE) != 0);

    void *mapping = map_image(image);
    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_writable_image_region(mapping, image->byte_len, nullptr);
    CHECK(n00b_result_is_ok(apply_r));

    writable_image_node_t *copy = n00b_result_get(apply_r);
    CHECK(copy != left);
    CHECK(copy->next != right);
    CHECK(copy->next->next == copy);
    CHECK(n00b_gc_addr_in_baked_region(copy));
    CHECK(n00b_gc_addr_in_baked_region(copy->next));

    auto range_opt = n00b_mmap_range_by_address(copy);
    CHECK(n00b_option_is_set(range_opt));
    n00b_alloc_range_t *range = n00b_option_get(range_opt);
    CHECK((range->flags & N00B_STATIC_OBJECT_F_BAKED) != 0);
    CHECK((range->flags & N00B_STATIC_OBJECT_F_MUTABLE) != 0);
    CHECK((range->flags & N00B_STATIC_OBJECT_F_READONLY) == 0);

    copy->tag = UINT64_C(0x57524954454d5554);
    CHECK(copy->tag == UINT64_C(0x57524954454d5554));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_writable_image_relocates_and_stays_writable();

    n00b_shutdown();
    return 0;
}
