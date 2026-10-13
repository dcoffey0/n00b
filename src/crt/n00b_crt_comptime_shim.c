 /*
  * Plain-C ABI shims for ncc-generated comptime CRT entries.
  *
  * Generated C declares these flat symbols directly so it does not need to
  * include n00b result/string/option headers. Runtime-internal code should call
  * the n00b-shaped APIs in n00b_crt.h instead.
  */
 
#define N00B_USE_INTERNAL_API

#include "n00b_crt.h"

#include "core/alloc.h"
#include "core/rwlock.h"
#include "core/string.h"
#include "util/panic.h"

void *
n00b_crt_alloc_static_payload(unsigned long long count,
                              unsigned long long elem_size,
                              unsigned long long type_hash,
                              unsigned int scan_kind,
                              n00b_gc_scan_cb_t scan_cb,
                              void *scan_user)
{
    if (count > (unsigned long long)SIZE_MAX
        || elem_size > (unsigned long long)SIZE_MAX
        || scan_kind > (unsigned int)N00B_GC_SCAN_KIND_CALLBACK) {
        return nullptr;
    }

    n00b_alloc_opts_t opts = {
        .scan_kind = (n00b_gc_scan_kind_t)scan_kind,
        .scan_cb   = scan_cb,
        .scan_user = scan_user,
    };
    return n00b_alloc_size_typed_with_opts((size_t)count,
                                           (size_t)elem_size,
                                           (uint64_t)type_hash,
                                           &opts);
}

n00b_buffer_t *
n00b_crt_alloc_static_buffer(unsigned long long type_hash,
                             unsigned long long cached_hash_hi,
                             unsigned long long cached_hash_lo)
{
    n00b_buffer_t *buf =
        n00b_alloc_size_typed(1, sizeof(n00b_buffer_t), (uint64_t)type_hash);
    n00b_uint128_t cached_hash =
        (((n00b_uint128_t)cached_hash_hi) << 64) | (n00b_uint128_t)cached_hash_lo;
    if (cached_hash == (n00b_uint128_t)0) {
        return buf;
    }

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(buf, .pin = &pin);
    if (info.kind == n00b_alloc_inline) {
        info.hdr.in_line->cached_hash = cached_hash;
    }
    else if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->cached_hash = cached_hash;
    }
    n00b_alloc_hdr_unpin(pin);
    return buf;
}

n00b_rwlock_t *
n00b_crt_alloc_static_rwlock(void)
{
    n00b_rwlock_t *lock = n00b_alloc(n00b_rwlock_t);
    _n00b_rw_init(lock, nullptr);
    return lock;
}

int
n00b_crt_capture_comptime_roots(void *const *roots,
                                unsigned long long root_count,
                                const char *path)
 {
     if (roots == nullptr || root_count == 0 || path == nullptr || path[0] == '\0') {
         return 72;
     }
 
     n00b_string_t *path_string = n00b_string_from_cstr(path);
     [[n00b::nomap]] n00b_result_t(bool) capture_r =
         n00b_crt_capture_comptime_roots_to_path(roots, root_count, path_string);
     if (n00b_result_is_ok(capture_r)) {
         return 0;
     }
 
     switch (n00b_result_get_err(capture_r)) {
     case N00B_CRT_IMAGE_ERR_ARG:
         return 72;
     case N00B_CRT_IMAGE_ERR_EXPORT:
         return 73;
     case N00B_CRT_IMAGE_ERR_OPEN:
         return 74;
     default:
        return 75;
    }
}

int
n00b_crt_capture_writable_comptime_roots(void *const *roots,
                                         unsigned long long root_count,
                                         const char *path)
{
    if (roots == nullptr || root_count == 0 || path == nullptr || path[0] == '\0') {
        return 72;
    }

    n00b_string_t *path_string = n00b_string_from_cstr(path);
    [[n00b::nomap]] n00b_result_t(bool) capture_r =
        n00b_crt_capture_writable_comptime_roots_to_path(roots,
                                                         root_count,
                                                         path_string);
    if (n00b_result_is_ok(capture_r)) {
        return 0;
    }

    switch (n00b_result_get_err(capture_r)) {
    case N00B_CRT_IMAGE_ERR_ARG:
        return 72;
    case N00B_CRT_IMAGE_ERR_EXPORT:
        return 73;
    case N00B_CRT_IMAGE_ERR_OPEN:
        return 74;
    default:
        return 75;
    }
}

static int
n00b_crt_capture_static_roots_common(
    const n00b_crt_static_root_desc_t *roots,
    unsigned long long root_count,
    const char *path,
    bool writable)
{
    if (roots == nullptr || root_count == 0 || path == nullptr || path[0] == '\0') {
        return 72;
    }

    n00b_string_t *path_string = n00b_string_from_cstr(path);
    [[n00b::nomap]] n00b_result_t(bool) capture_r =
        writable
            ? n00b_crt_capture_writable_static_roots_to_path(roots,
                                                             root_count,
                                                             path_string)
            : n00b_crt_capture_static_roots_to_path(roots,
                                                    root_count,
                                                    path_string);
    if (n00b_result_is_ok(capture_r)) {
        return 0;
    }

    switch (n00b_result_get_err(capture_r)) {
    case N00B_CRT_IMAGE_ERR_ARG:
        return 72;
    case N00B_CRT_IMAGE_ERR_EXPORT:
        return 73;
    case N00B_CRT_IMAGE_ERR_OPEN:
        return 74;
    default:
        return 75;
    }
}

int
n00b_crt_capture_static_roots(const n00b_crt_static_root_desc_t *roots,
                              unsigned long long root_count,
                              const char *path)
{
    return n00b_crt_capture_static_roots_common(roots,
                                                root_count,
                                                path,
                                                false);
}

int
n00b_crt_capture_writable_static_roots(
    const n00b_crt_static_root_desc_t *roots,
    unsigned long long root_count,
    const char *path)
{
    return n00b_crt_capture_static_roots_common(roots,
                                                root_count,
                                                path,
                                                true);
}
 
 void *
 n00b_crt_apply_comptime_image(void)
 {
     [[n00b::nomap]] n00b_result_t(n00b_option_t(void *)) apply_r =
         n00b_crt_apply_linked_comptime_image();
     if (n00b_result_is_err(apply_r)) {
         n00b_panic("comptime image apply failed");
     }
 
     n00b_option_t(void *) root = n00b_result_get(apply_r);
     if (!n00b_option_is_set(root)) {
         return nullptr;
     }
 
    return n00b_option_get(root);
}

void *
n00b_crt_apply_writable_image(void)
{
    [[n00b::nomap]] n00b_result_t(n00b_option_t(void *)) apply_r =
        n00b_crt_apply_linked_writable_comptime_image();
    if (n00b_result_is_err(apply_r)) {
        n00b_panic("writable comptime image apply failed");
    }

    n00b_option_t(void *) root = n00b_result_get(apply_r);
    if (!n00b_option_is_set(root)) {
        return nullptr;
    }

    return n00b_option_get(root);
}
