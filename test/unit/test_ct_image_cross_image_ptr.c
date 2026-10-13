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
#include "util/marshal.h"
#include "test_check.h"

typedef struct ct_cross_target_t {
    uint64_t tag;
} ct_cross_target_t;

typedef struct ct_cross_owner_t {
    ct_cross_target_t *target;
    uint64_t           tag;
} ct_cross_owner_t;

static const n00b_static_identity_t target_identity_a_first = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.cross-image",
    .object_key   = "target-a-first",
};

static const n00b_static_identity_t target_identity_b_first = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.cross-image",
    .object_key   = "target-b-first",
};

static const n00b_static_identity_t target_identity_crt = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.cross-image",
    .object_key   = "target-crt",
};

static const n00b_static_identity_t target_identity_crt_b_first = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.cross-image",
    .object_key   = "target-crt-b-first",
};

static const n00b_static_identity_t target_identity_crt_writable = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.cross-image",
    .object_key   = "target-crt-writable",
};

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

static n00b_alloc_type_info_t
alloc_tinfo(void *obj)
{
    bool                   pin   = false;
    n00b_alloc_info_t      info  = n00b_find_alloc_info(obj, .pin = &pin);
    n00b_alloc_type_info_t tinfo;
    if (info.kind == n00b_alloc_oob) {
        tinfo = info.hdr.oob->tinfo;
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        tinfo = info.hdr.in_line->tinfo;
    }
    n00b_alloc_hdr_unpin(pin);
    return tinfo;
}

static size_t
alloc_user_len(void *obj)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);
    size_t            len;
    if (info.kind == n00b_alloc_oob) {
        len = info.hdr.oob->hcur ? info.hdr.oob->alloc_len - N00B_ALLOC_HDR_SZ
                                 : info.hdr.oob->alloc_len;
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        len = info.hdr.in_line->alloc_len - N00B_ALLOC_HDR_SZ;
    }
    n00b_alloc_hdr_unpin(pin);
    return len;
}

static void
register_source_target_with_flags(ct_cross_target_t *target,
                                  const n00b_static_identity_t *identity,
                                  uint32_t flags)
{
    size_t user_len = alloc_user_len(target);
    n00b_alloc_range_t *range = n00b_mmap_register_range(
        target,
        (char *)target + user_len,
        n00b_mmap_static,
        .file      = "test.ct-image.cross-image.source",
        .tinfo     = alloc_tinfo(target),
        .scan_kind = N00B_GC_SCAN_KIND_NONE,
        .identity  = identity,
        .flags     = flags);
    CHECK(range != nullptr);
}

static void
register_source_target(ct_cross_target_t *target,
                       const n00b_static_identity_t *identity)
{
    register_source_target_with_flags(target,
                                      identity,
                                      N00B_STATIC_OBJECT_F_READONLY);
}

static void
unregister_source_target(ct_cross_target_t *target)
{
    size_t user_len = alloc_user_len(target);
    n00b_mmap_delete_ranges(n00b_global_mem_map(n00b_get_runtime()),
                            (uint64_t)(uintptr_t)target,
                            (uint64_t)(uintptr_t)((char *)target + user_len));
}

static n00b_buffer_t *
export_checked(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) export_r =
        n00b_ct_image_export(root);
    CHECK(n00b_result_is_ok(export_r));
    return n00b_result_get(export_r);
}

static n00b_buffer_t *
export_writable_checked(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) export_r =
        n00b_ct_image_export_writable(root);
    CHECK(n00b_result_is_ok(export_r));
    return n00b_result_get(export_r);
}

static void *
map_image(n00b_buffer_t *image)
{
    size_t map_len = n00b_page_align(image->byte_len);
    auto map_r = n00b_mmap(map_len,
                           .kind = n00b_mmap_api_mmap,
                           .name = "ct-cross-image");
    CHECK(n00b_result_is_ok(map_r));
    void *mapping = n00b_result_get(map_r);
    memcpy(mapping, image->data, image->byte_len);
    return mapping;
}

static void *
relocate_checked(void *mapping, n00b_buffer_t *image)
{
    [[n00b::nomap]] n00b_result_t(void *) reloc_r =
        n00b_ct_image_relocate_inplace(mapping, image->byte_len);
    CHECK(n00b_result_is_ok(reloc_r));
    return n00b_result_get(reloc_r);
}

static void
register_baked_image(void *mapping, n00b_buffer_t *image, void *root)
{
    n00b_ct_image_header_t *hdr = mapping;
    auto identity_r = n00b_ct_image_root_identity(mapping, image->byte_len);
    CHECK(n00b_result_is_ok(identity_r));
    auto identity_opt = n00b_result_get(identity_r);
    n00b_gc_baked_region_t region = {
        .base           = mapping,
        .len            = image->byte_len,
        .marshal_stream = (char *)mapping + hdr->marshal_off,
        .marshal_len    = hdr->marshal_len,
        .root           = root,
        .root_identity  = n00b_option_is_set(identity_opt)
                              ? n00b_option_get(identity_opt)
                              : nullptr,
        .writable       = false,
    };
    [[n00b::nomap]] n00b_result_t(bool) register_r =
        n00b_gc_register_baked_region(&region);
    CHECK(n00b_result_is_ok(register_r));
}

static void
apply_baked_image(void *mapping, n00b_buffer_t *image, void **root_out)
{
    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_comptime_image_region(mapping,
                                             image->byte_len,
                                             n00b_page_align(image->byte_len));
    CHECK(n00b_result_is_ok(apply_r));
    *root_out = n00b_result_get(apply_r);
}

static void
apply_writable_image(void *mapping, n00b_buffer_t *image, void **root_out)
{
    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_writable_image_region(mapping, image->byte_len, nullptr);
    CHECK(n00b_result_is_ok(apply_r));
    *root_out = n00b_result_get(apply_r);
}

static void
run_cross_image_case(const n00b_static_identity_t *identity, bool a_first)
{
    ct_cross_target_t *target = n00b_alloc_with_opts(
        ct_cross_target_t,
        &(n00b_alloc_opts_t){ .scan_kind = N00B_GC_SCAN_KIND_NONE });
    target->tag = a_first ? UINT64_C(0xc1055001) : UINT64_C(0xc1055002);
    register_source_target(target, identity);

    ct_cross_owner_t *owner = n00b_alloc(ct_cross_owner_t);
    set_ptr_words(owner, 1);
    owner->target = target;
    owner->tag = a_first ? UINT64_C(0xa0000001) : UINT64_C(0xb0000001);

    n00b_buffer_t *target_image = export_checked(target);
    n00b_buffer_t *owner_image  = export_checked(owner);
    unregister_source_target(target);

    void *target_mapping = map_image(target_image);
    void *owner_mapping  = map_image(owner_image);

    if (a_first) {
        ct_cross_owner_t *owner_copy = relocate_checked(owner_mapping, owner_image);
        CHECK(owner_copy->target == nullptr);
        CHECK(n00b_marshal_deferred_static_patch_count() > 0);
        register_baked_image(owner_mapping, owner_image, owner_copy);

        ct_cross_target_t *target_copy = relocate_checked(target_mapping, target_image);
        CHECK(target_copy->tag == target->tag);
        register_baked_image(target_mapping, target_image, target_copy);

        CHECK(owner_copy->target == target_copy);
        CHECK(owner_copy->target->tag == target->tag);
        CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    }
    else {
        ct_cross_target_t *target_copy = relocate_checked(target_mapping, target_image);
        CHECK(target_copy->tag == target->tag);
        register_baked_image(target_mapping, target_image, target_copy);

        ct_cross_owner_t *owner_copy = relocate_checked(owner_mapping, owner_image);
        register_baked_image(owner_mapping, owner_image, owner_copy);

        CHECK(owner_copy->target == target_copy);
        CHECK(owner_copy->target->tag == target->tag);
        CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    }
}

static void
run_crt_cross_image_case(void)
{
    ct_cross_target_t *target = n00b_alloc_with_opts(
        ct_cross_target_t,
        &(n00b_alloc_opts_t){ .scan_kind = N00B_GC_SCAN_KIND_NONE });
    target->tag = UINT64_C(0xc1055c47);
    register_source_target(target, &target_identity_crt);

    ct_cross_owner_t *owner = n00b_alloc(ct_cross_owner_t);
    set_ptr_words(owner, 1);
    owner->target = target;
    owner->tag = UINT64_C(0xc47a0001);

    n00b_buffer_t *target_image = export_checked(target);
    n00b_buffer_t *owner_image  = export_checked(owner);
    unregister_source_target(target);

    void *target_mapping = map_image(target_image);
    void *owner_mapping  = map_image(owner_image);

    void *owner_root = nullptr;
    apply_baked_image(owner_mapping, owner_image, &owner_root);
    ct_cross_owner_t *owner_copy = owner_root;
    CHECK(owner_copy->target == nullptr);
    CHECK(n00b_marshal_deferred_static_patch_count() > 0);
    CHECK(n00b_get_runtime()->crt_pending_protects != nullptr);

    void *target_root = nullptr;
    apply_baked_image(target_mapping, target_image, &target_root);
    ct_cross_target_t *target_copy = target_root;

    CHECK(owner_copy->target == target_copy);
    CHECK(owner_copy->target->tag == target->tag);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    CHECK(n00b_get_runtime()->crt_pending_protects == nullptr);
}

static void
run_crt_cross_image_target_first_case(void)
{
    ct_cross_target_t *target = n00b_alloc_with_opts(
        ct_cross_target_t,
        &(n00b_alloc_opts_t){ .scan_kind = N00B_GC_SCAN_KIND_NONE });
    target->tag = UINT64_C(0xc1055b17);
    register_source_target(target, &target_identity_crt_b_first);

    ct_cross_owner_t *owner = n00b_alloc(ct_cross_owner_t);
    set_ptr_words(owner, 1);
    owner->target = target;
    owner->tag = UINT64_C(0xc47b0001);

    n00b_buffer_t *target_image = export_checked(target);
    n00b_buffer_t *owner_image  = export_checked(owner);
    unregister_source_target(target);

    void *target_mapping = map_image(target_image);
    void *owner_mapping  = map_image(owner_image);

    void *target_root = nullptr;
    apply_baked_image(target_mapping, target_image, &target_root);
    ct_cross_target_t *target_copy = target_root;
    CHECK(target_copy->tag == target->tag);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    CHECK(n00b_get_runtime()->crt_pending_protects == nullptr);

    void *owner_root = nullptr;
    apply_baked_image(owner_mapping, owner_image, &owner_root);
    ct_cross_owner_t *owner_copy = owner_root;

    CHECK(owner_copy->target == target_copy);
    CHECK(owner_copy->target->tag == target->tag);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    CHECK(n00b_get_runtime()->crt_pending_protects == nullptr);
}

static void
run_crt_cross_image_writable_target_case(void)
{
    ct_cross_target_t *target = n00b_alloc_with_opts(
        ct_cross_target_t,
        &(n00b_alloc_opts_t){ .scan_kind = N00B_GC_SCAN_KIND_NONE });
    target->tag = UINT64_C(0xc1055a71);
    register_source_target_with_flags(target,
                                      &target_identity_crt_writable,
                                      N00B_STATIC_OBJECT_F_MUTABLE);

    ct_cross_owner_t *owner = n00b_alloc(ct_cross_owner_t);
    set_ptr_words(owner, 1);
    owner->target = target;
    owner->tag = UINT64_C(0xc47a0002);

    n00b_buffer_t *target_image = export_writable_checked(target);
    n00b_buffer_t *owner_image  = export_checked(owner);
    unregister_source_target(target);

    void *target_mapping = map_image(target_image);
    void *owner_mapping  = map_image(owner_image);

    void *owner_root = nullptr;
    apply_baked_image(owner_mapping, owner_image, &owner_root);
    ct_cross_owner_t *owner_copy = owner_root;
    CHECK(owner_copy->target == nullptr);
    CHECK(n00b_marshal_deferred_static_patch_count() > 0);
    CHECK(n00b_get_runtime()->crt_pending_protects != nullptr);

    void *target_root = nullptr;
    apply_writable_image(target_mapping, target_image, &target_root);
    ct_cross_target_t *target_copy = target_root;

    CHECK(owner_copy->target == target_copy);
    CHECK(owner_copy->target->tag == target->tag);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    CHECK(n00b_get_runtime()->crt_pending_protects == nullptr);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    run_cross_image_case(&target_identity_a_first, true);
    run_cross_image_case(&target_identity_b_first, false);
    run_crt_cross_image_case();
    run_crt_cross_image_target_first_case();
    run_crt_cross_image_writable_target_case();

    n00b_shutdown();
    return 0;
}
