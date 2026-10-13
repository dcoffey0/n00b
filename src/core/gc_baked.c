#define N00B_USE_INTERNAL_API

#include "core/gc_baked.h"

#include "adt/option.h"
#include "core/alloc.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "util/marshal.h"

typedef struct {
    uint64_t *bitmap;
    uint64_t  bitmap_words;
} n00b_gc_baked_bitmap_t;

typedef struct {
    n00b_err_t                   err;
    uintptr_t                    base;
    uintptr_t                    end;
    void                        *root;
    const n00b_static_identity_t *root_identity;
    bool                         writable;
} n00b_gc_baked_register_ctx_t;

n00b_string_t *
n00b_gc_baked_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_GC_BAKED_ERR_ARG:
        return r"invalid baked GC region argument";
    case N00B_GC_BAKED_ERR_MARSHAL:
        return r"invalid baked marshal stream";
    case N00B_GC_BAKED_ERR_RANGE:
        return r"baked GC range registration failed";
    case N00B_GC_BAKED_ERR_EXTERNAL_POINTER:
        return r"baked image contains a pointer to mutable runtime memory";
    default:
        return r"unknown baked GC error";
    }
}

static n00b_allocator_t *
n00b_gc_baked_registry_allocator(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
}

static n00b_gc_scan_kind_t
n00b_gc_baked_effective_scan_kind(const n00b_marshal_relocated_alloc_t *alloc)
{
    if (alloc->scan_kind != N00B_GC_SCAN_KIND_DEFAULT) {
        return alloc->scan_kind;
    }
    return alloc->no_scan ? N00B_GC_SCAN_KIND_NONE : N00B_GC_SCAN_KIND_ALL;
}

static n00b_result_t(uint64_t)
n00b_gc_baked_scan_words(const n00b_marshal_relocated_alloc_t *alloc)
{
    uint64_t range_words = alloc->len / sizeof(void *);

    if (alloc->ptr_words_known) {
        if ((uint64_t)alloc->ptr_words > range_words) {
            return n00b_result_err(uint64_t, N00B_GC_BAKED_ERR_MARSHAL);
        }
        return n00b_result_ok(uint64_t, (uint64_t)alloc->ptr_words);
    }

    return n00b_result_ok(uint64_t, range_words);
}

static void
n00b_gc_baked_scan_bitmap(n00b_gc_map_t *map, void *user)
{
    n00b_gc_baked_bitmap_t *desc = user;
    if (map == nullptr || desc == nullptr || desc->bitmap == nullptr) {
        return;
    }

    uint64_t map_words = n00b_gc_map_word_count(map->num_words);
    uint64_t n         = desc->bitmap_words < map_words ? desc->bitmap_words
                                                        : map_words;
    for (uint64_t i = 0; i < n; i++) {
        map->bitmap[i] = desc->bitmap[i];
    }
}

static n00b_result_t(n00b_gc_baked_bitmap_t *)
n00b_gc_baked_build_bitmap(const n00b_marshal_relocated_alloc_t *alloc)
{
    auto scan_words_r = n00b_gc_baked_scan_words(alloc);
    if (n00b_result_is_err(scan_words_r)) {
        return n00b_result_err(n00b_gc_baked_bitmap_t *,
                               n00b_result_get_err(scan_words_r));
    }

    uint64_t scan_words = n00b_result_get(scan_words_r);
    if (scan_words == 0) {
        return n00b_result_ok(n00b_gc_baked_bitmap_t *, nullptr);
    }

    n00b_allocator_t *allocator = n00b_gc_baked_registry_allocator();
    n00b_gc_baked_bitmap_t *desc = n00b_alloc_with_opts(
        n00b_gc_baked_bitmap_t,
        &(n00b_alloc_opts_t){.allocator = allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_ALL});
    desc->bitmap_words = n00b_gc_map_word_count(scan_words);
    desc->bitmap       = n00b_alloc_array_with_opts(
        uint64_t,
        desc->bitmap_words,
        &(n00b_alloc_opts_t){.allocator = allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});

    for (uint64_t i = 0; i < desc->bitmap_words; i++) {
        desc->bitmap[i] = 0;
    }
    n00b_gc_map_t map = {
        .user_ptr  = alloc->start,
        .num_words = scan_words,
        .bitmap    = desc->bitmap,
    };

    switch (n00b_gc_baked_effective_scan_kind(alloc)) {
    case N00B_GC_SCAN_KIND_NONE:
        break;
    case N00B_GC_SCAN_KIND_EVERY_OTHER:
        for (uint64_t i = 0; i < scan_words; i += 2) {
            n00b_gc_map_mark(&map, i);
        }
        break;
    case N00B_GC_SCAN_KIND_CALLBACK:
        if (alloc->callback_bitmap == nullptr
            || alloc->callback_bitmap_words == 0) {
            return n00b_result_err(n00b_gc_baked_bitmap_t *,
                                   N00B_GC_BAKED_ERR_MARSHAL);
        }
        for (uint64_t i = 0; i < desc->bitmap_words
                             && i < alloc->callback_bitmap_words; i++) {
            desc->bitmap[i] = alloc->callback_bitmap[i];
        }
        if ((scan_words & 63u) != 0) {
            uint64_t last = desc->bitmap_words - 1;
            uint64_t keep = scan_words & 63u;
            desc->bitmap[last] &= (UINT64_C(1) << keep) - 1u;
        }
        break;
    case N00B_GC_SCAN_KIND_DEFAULT:
    case N00B_GC_SCAN_KIND_ALL:
    default:
        for (uint64_t i = 0; i < scan_words; i++) {
            n00b_gc_map_mark(&map, i);
        }
        break;
    }

    return n00b_result_ok(n00b_gc_baked_bitmap_t *, desc);
}

static bool
n00b_gc_baked_pointer_is_closed(n00b_gc_baked_register_ctx_t *ctx,
                                void                        *ptr)
{
    if (ptr == nullptr) {
        return true;
    }

    uintptr_t p = (uintptr_t)ptr;
    if (p >= ctx->base && p < ctx->end) {
        return true;
    }

    auto range_opt = n00b_mmap_range_by_address(ptr);
    if (n00b_option_is_set(range_opt)
        && n00b_option_get(range_opt)->kind == n00b_mmap_static) {
        return true;
    }

    n00b_mmap_view_t map;
    if (!n00b_mmap_view_by_address(ptr, &map)) {
        return true;
    }

    return map.kind == n00b_mmap_static || map.kind == n00b_mmap_zero_page;
}

static bool
n00b_gc_baked_slot_scanned(const n00b_marshal_relocated_alloc_t *alloc,
                           n00b_gc_scan_kind_t                   scan_kind,
                           uint64_t                              word_ix)
{
    switch (scan_kind) {
    case N00B_GC_SCAN_KIND_NONE:
        return false;
    case N00B_GC_SCAN_KIND_EVERY_OTHER:
        return (word_ix & 1u) == 0;
    case N00B_GC_SCAN_KIND_CALLBACK:
        if (alloc->callback_bitmap == nullptr
            || (word_ix >> 6) >= alloc->callback_bitmap_words) {
            return false;
        }
        return ((alloc->callback_bitmap[word_ix >> 6] >> (word_ix & 63u))
                & UINT64_C(1)) != 0;
    case N00B_GC_SCAN_KIND_DEFAULT:
    case N00B_GC_SCAN_KIND_ALL:
    default:
        return true;
    }
}

static n00b_result_t(bool)
n00b_gc_baked_validate_alloc(const n00b_marshal_relocated_alloc_t *alloc,
                             void                                *user)
{
    n00b_gc_baked_register_ctx_t *ctx = user;
    if (ctx == nullptr || alloc == nullptr || alloc->start == nullptr
        || alloc->len == 0) {
        if (ctx != nullptr) {
            ctx->err = N00B_GC_BAKED_ERR_ARG;
        }
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }

    uintptr_t start = (uintptr_t)alloc->start;
    uintptr_t end   = start + alloc->len;
    if (alloc->len > (size_t)(UINTPTR_MAX - start)
        || start < ctx->base || end > ctx->end) {
        ctx->err = N00B_GC_BAKED_ERR_ARG;
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }

    auto scan_words_r = n00b_gc_baked_scan_words(alloc);
    if (n00b_result_is_err(scan_words_r)) {
        ctx->err = n00b_result_get_err(scan_words_r);
        return n00b_result_err(bool, ctx->err);
    }

    uint64_t scan_words = n00b_result_get(scan_words_r);
    if (scan_words == 0) {
        return n00b_result_ok(bool, true);
    }

    n00b_gc_scan_kind_t scan_kind = n00b_gc_baked_effective_scan_kind(alloc);
    if (scan_kind == N00B_GC_SCAN_KIND_CALLBACK
        && (alloc->callback_bitmap == nullptr
            || alloc->callback_bitmap_words == 0)) {
        ctx->err = N00B_GC_BAKED_ERR_MARSHAL;
        return n00b_result_err(bool, ctx->err);
    }

    if (!ctx->writable) {
        void **slots = alloc->start;
        for (uint64_t i = 0; i < scan_words; i++) {
            if (n00b_gc_baked_slot_scanned(alloc, scan_kind, i)
                && !n00b_gc_baked_pointer_is_closed(ctx, slots[i])) {
                ctx->err = N00B_GC_BAKED_ERR_EXTERNAL_POINTER;
                return n00b_result_err(bool, ctx->err);
            }
        }
    }

    return n00b_result_ok(bool, true);
}

static void
n00b_gc_baked_register_writable_root_word(void *start, uint64_t word_ix)
{
    _n00b_gc_register_root((char *)start + word_ix * sizeof(void *), 1);
}

static n00b_result_t(bool)
n00b_gc_baked_register_writable_roots(const n00b_marshal_relocated_alloc_t *alloc)
{
    auto scan_words_r = n00b_gc_baked_scan_words(alloc);
    if (n00b_result_is_err(scan_words_r)) {
        return n00b_result_err(bool, n00b_result_get_err(scan_words_r));
    }

    uint64_t scan_words = n00b_result_get(scan_words_r);
    if (scan_words == 0) {
        return n00b_result_ok(bool, true);
    }

    switch (n00b_gc_baked_effective_scan_kind(alloc)) {
    case N00B_GC_SCAN_KIND_NONE:
        return n00b_result_ok(bool, true);
    case N00B_GC_SCAN_KIND_EVERY_OTHER:
        for (uint64_t i = 0; i < scan_words; i += 2) {
            n00b_gc_baked_register_writable_root_word(alloc->start, i);
        }
        return n00b_result_ok(bool, true);
    case N00B_GC_SCAN_KIND_CALLBACK:
        if (alloc->callback_bitmap == nullptr
            || alloc->callback_bitmap_words == 0) {
            return n00b_result_err(bool, N00B_GC_BAKED_ERR_MARSHAL);
        }
        for (uint64_t i = 0; i < scan_words; i++) {
            uint64_t word = i >> 6;
            uint64_t bit  = i & 63u;
            if (word < alloc->callback_bitmap_words
                && ((alloc->callback_bitmap[word] >> bit) & UINT64_C(1)) != 0) {
                n00b_gc_baked_register_writable_root_word(alloc->start, i);
            }
        }
        return n00b_result_ok(bool, true);
    case N00B_GC_SCAN_KIND_DEFAULT:
    case N00B_GC_SCAN_KIND_ALL:
    default:
        _n00b_gc_register_root(alloc->start, (size_t)scan_words);
        return n00b_result_ok(bool, true);
    }
}

static n00b_result_t(bool)
n00b_gc_baked_register_alloc(const n00b_marshal_relocated_alloc_t *alloc,
                             void                                *user)
{
    n00b_gc_baked_register_ctx_t *ctx = user;
    if (ctx == nullptr || alloc == nullptr || alloc->start == nullptr
        || alloc->len == 0) {
        if (ctx != nullptr) {
            ctx->err = N00B_GC_BAKED_ERR_ARG;
        }
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }

    uintptr_t start = (uintptr_t)alloc->start;
    if (alloc->len > (size_t)(UINTPTR_MAX - start)) {
        ctx->err = N00B_GC_BAKED_ERR_ARG;
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }

    n00b_gc_scan_kind_t scan_kind = n00b_gc_baked_effective_scan_kind(alloc);
    n00b_gc_scan_cb_t   scan_cb   = nullptr;
    void               *scan_user = nullptr;

    if (scan_kind != N00B_GC_SCAN_KIND_NONE) {
        auto bitmap_r = n00b_gc_baked_build_bitmap(alloc);
        if (n00b_result_is_err(bitmap_r)) {
            ctx->err = n00b_result_get_err(bitmap_r);
            return n00b_result_err(bool, ctx->err);
        }
        scan_user = n00b_result_get(bitmap_r);
        if (scan_user == nullptr) {
            scan_kind = N00B_GC_SCAN_KIND_NONE;
        }
        else {
            scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
            scan_cb   = n00b_gc_baked_scan_bitmap;
        }
    }

    n00b_alloc_range_t *range = n00b_mmap_register_range(
        alloc->start,
        (void *)(start + alloc->len),
        n00b_mmap_static,
        .allocator = n00b_gc_baked_registry_allocator(),
        .file      = "comptime image",
        .tinfo     = alloc->tinfo,
        .scan_kind = scan_kind,
        .scan_cb   = scan_cb,
        .scan_user = scan_user,
        .identity  = (alloc->start == ctx->root) ? ctx->root_identity : nullptr,
        .flags     = ctx->writable
                         ? (N00B_STATIC_OBJECT_F_MUTABLE | N00B_STATIC_OBJECT_F_BAKED)
                         : (N00B_STATIC_OBJECT_F_READONLY | N00B_STATIC_OBJECT_F_BAKED));
    if (range == nullptr) {
        ctx->err = N00B_GC_BAKED_ERR_RANGE;
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_RANGE);
    }
    range->cached_hash = alloc->cached_hash;

    if (ctx->writable) {
        auto roots_r = n00b_gc_baked_register_writable_roots(alloc);
        if (n00b_result_is_err(roots_r)) {
            ctx->err = n00b_result_get_err(roots_r);
            return n00b_result_err(bool, ctx->err);
        }
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
n00b_gc_register_baked_region_impl(const n00b_gc_baked_region_t *region,
                                   bool writable)
{
    if (region == nullptr || region->base == nullptr || region->len == 0
        || region->marshal_stream == nullptr || region->marshal_len == 0
        || region->root == nullptr) {
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }

    uintptr_t start = (uintptr_t)region->base;
    if (region->len > (size_t)(UINTPTR_MAX - start)) {
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_ARG);
    }
    uintptr_t end = start + region->len;

    if (n00b_gc_addr_in_baked_region(region->root)) {
        return n00b_result_ok(bool, true);
    }

    n00b_gc_baked_register_ctx_t ctx = {
        .err           = N00B_GC_BAKED_ERR_RANGE,
        .base          = start,
        .end           = end,
        .root          = region->root,
        .root_identity = region->root_identity,
        .writable      = writable,
    };

    auto validate_r = n00b_marshal_visit_relocated_allocs(
        region->marshal_stream,
        region->marshal_len,
        n00b_gc_baked_validate_alloc,
        &ctx);
    if (n00b_result_is_err(validate_r)) {
        n00b_err_t err = n00b_result_get_err(validate_r);
        if (err >= N00B_MARSHAL_OK && err <= N00B_MARSHAL_ERR_LIMIT) {
            return n00b_result_err(bool, N00B_GC_BAKED_ERR_MARSHAL);
        }
        return n00b_result_err(bool, err);
    }
    if (!n00b_result_get(validate_r)) {
        return n00b_result_err(bool, ctx.err);
    }

    auto map_opt = n00b_mmap_register(region->base,
                                      (void *)end,
                                      n00b_mmap_static,
                                      .file              = "comptime image",
                                      .perms             = writable ? n00b_mmap_perms_rw
                                                                   : n00b_mmap_perms_ro,
                                      .definitely_unique = false);
    if (!n00b_option_is_set(map_opt)) {
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_RANGE);
    }

    auto visit_r = n00b_marshal_visit_relocated_allocs(
        region->marshal_stream,
        region->marshal_len,
        n00b_gc_baked_register_alloc,
        &ctx);
    if (n00b_result_is_err(visit_r)) {
        n00b_err_t err = n00b_result_get_err(visit_r);
        if (err >= N00B_MARSHAL_OK && err <= N00B_MARSHAL_ERR_LIMIT) {
            return n00b_result_err(bool, N00B_GC_BAKED_ERR_MARSHAL);
        }
        return n00b_result_err(bool, err);
    }
    if (!n00b_result_get(visit_r)) {
        return n00b_result_err(bool, ctx.err);
    }

    auto deferred_r = n00b_marshal_apply_deferred_static_patches();
    if (n00b_result_is_err(deferred_r)) {
        return n00b_result_err(bool, N00B_GC_BAKED_ERR_MARSHAL);
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_gc_register_baked_region(const n00b_gc_baked_region_t *region)
{
    return n00b_gc_register_baked_region_impl(region, false);
}

n00b_result_t(bool)
n00b_gc_register_baked_region_writable(const n00b_gc_baked_region_t *region)
{
    return n00b_gc_register_baked_region_impl(region, true);
}

bool
n00b_gc_addr_in_baked_region(void *addr)
{
    if (addr == nullptr) {
        return false;
    }

    auto range_opt = n00b_mmap_range_by_address(addr);
    if (!n00b_option_is_set(range_opt)) {
        return false;
    }

    n00b_alloc_range_t *range = n00b_option_get(range_opt);
    return (range->flags & N00B_STATIC_OBJECT_F_BAKED) != 0;
}
