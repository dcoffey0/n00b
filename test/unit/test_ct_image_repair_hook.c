#include <stdint.h>
#include <string.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "n00b_crt.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

typedef struct repair_node_t {
    struct repair_node_t *next;
    uint64_t              tag;
} repair_node_t;

typedef struct {
    int      calls;
    void    *image_base;
    size_t   image_len;
    uint64_t observed_tag;
} repair_ctx_t;

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
                           .name = "ct-image-repair");
    CHECK(n00b_result_is_ok(map_r));
    void *mapping = n00b_result_get(map_r);
    memcpy(mapping, image->data, image->byte_len);
    return mapping;
}

static n00b_result_t(bool)
repair_hook(void *image_base, size_t image_len, void *root, void *user)
{
    repair_ctx_t *ctx = user;
    repair_node_t *node = root;

    CHECK(ctx != nullptr);
    CHECK(image_base != nullptr);
    CHECK(image_len > sizeof(n00b_ct_image_header_t));
    CHECK(node != nullptr);
    CHECK(node->next == node);

    ctx->calls++;
    ctx->image_base = image_base;
    ctx->image_len = image_len;
    ctx->observed_tag = node->tag;
    node->tag = UINT64_C(0x5245504149524544);

    return n00b_result_ok(bool, true);
}

static n00b_buffer_t *
export_repair_image(void)
{
    repair_node_t *node = n00b_alloc(repair_node_t);
    set_ptr_words(node, 1);
    node->next = node;
    node->tag = UINT64_C(0x5245504149523031);

    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) export_r =
        n00b_ct_image_export(node);
    CHECK(n00b_result_is_ok(export_r));
    return n00b_result_get(export_r);
}

static void
test_repair_hook_fires_once_after_relocate(void)
{
    n00b_buffer_t *image = export_repair_image();
    void *mapping = map_image(image);
    repair_ctx_t ctx = {};
    n00b_ct_image_repair_hook_t hook = {
        .fn = repair_hook,
        .user = &ctx,
    };

    [[n00b::nomap]] n00b_result_t(void *) reloc_r =
        n00b_ct_image_relocate_inplace_ex(mapping, image->byte_len, &hook);
    CHECK(n00b_result_is_ok(reloc_r));

    repair_node_t *root = n00b_result_get(reloc_r);
    CHECK(ctx.calls == 1);
    CHECK(ctx.image_base == mapping);
    CHECK(ctx.image_len == image->byte_len);
    CHECK(ctx.observed_tag == UINT64_C(0x5245504149523031));
    CHECK(root->tag == UINT64_C(0x5245504149524544));
}

static void
test_absent_repair_hook_is_noop(void)
{
    n00b_buffer_t *image = export_repair_image();
    void *mapping = map_image(image);
    repair_ctx_t ctx = {};

    [[n00b::nomap]] n00b_result_t(void *) reloc_r =
        n00b_ct_image_relocate_inplace_ex(mapping, image->byte_len, nullptr);
    CHECK(n00b_result_is_ok(reloc_r));
    CHECK(ctx.calls == 0);
}

static void
test_registered_repair_hook_fires_from_crt_apply(void)
{
    n00b_buffer_t *image = export_repair_image();
    void *mapping = map_image(image);
    repair_ctx_t ctx = {};
    n00b_ct_image_repair_hook_t hook = {
        .fn = repair_hook,
        .user = &ctx,
    };

    [[n00b::nomap]] n00b_result_t(bool) set_r =
        n00b_ct_image_set_repair_hook(mapping, &hook);
    CHECK(n00b_result_is_ok(set_r));

    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_comptime_image_region(mapping,
                                             image->byte_len,
                                             n00b_page_align(image->byte_len));
    CHECK(n00b_result_is_ok(apply_r));

    repair_node_t *root = n00b_result_get(apply_r);
    CHECK(ctx.calls == 1);
    CHECK(ctx.image_base == mapping);
    CHECK(ctx.image_len == image->byte_len);
    CHECK(ctx.observed_tag == UINT64_C(0x5245504149523031));
    CHECK(root->tag == UINT64_C(0x5245504149524544));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_repair_hook_fires_once_after_relocate();
    test_absent_repair_hook_is_noop();
    test_registered_repair_hook_fires_from_crt_apply();

    n00b_shutdown();
    return 0;
}
