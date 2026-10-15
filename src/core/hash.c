// IWYU pragma: no_include <sys/_endian.h>

#define N00B_HASH_INTERNAL
#ifdef _WIN32
#define XXH_VECTOR 0
#endif

#include "n00b.h"

#if defined __linux__
#include <byteswap.h>
#define n00b_bswap64(x) bswap_64(x)
#elif defined(_WIN32)
#define n00b_bswap64(x) __builtin_bswap64(x)
#else
#define n00b_bswap64(x) htonll(x)
#endif

#include <string.h> // IWYU pragma: keep ; for strlen
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/hash.h"
#include "core/mmaps.h"
#include "core/type_info.h"
#include "core/string.h"
#include "core/buffer.h"

n00b_uint128_t
n00b_hash(void *obj, n00b_hash_fn fn)
{
    bool explicit_hash     = fn != nullptr;
    bool has_type_metadata = false;

    auto range_opt = n00b_mmap_range_by_address(obj);
    if (n00b_option_is_set(range_opt)) {
        n00b_alloc_range_t *range = n00b_option_get(range_opt);
        if (range->kind == n00b_mmap_static) {
            has_type_metadata = true;
            if (!explicit_hash && range->cached_hash != 0) {
                return range->cached_hash;
            }
        }
    }

    // The header is valid only while pinned, so the hash function, which can
    // run arbitrary code, is called outside the pin and the store below looks
    // the header up again.
    bool              pin     = false;
    n00b_alloc_info_t ainfo   = n00b_find_alloc_info(obj, .pin = &pin);
    bool              managed = n00b_alloc_info_is_heap(ainfo);
    n00b_uint128_t    cached  = 0;

    has_type_metadata = has_type_metadata || managed
                     || n00b_alloc_info_is_static_range(ainfo);

    if (managed && !explicit_hash) {
        if (ainfo.kind == n00b_alloc_inline) {
            cached = ainfo.hdr.in_line->cached_hash;
        }
        else {
            cached = ainfo.hdr.oob->cached_hash;
        }
    }
    else if (!explicit_hash && ainfo.kind == n00b_alloc_static_range) {
        // Descriptor-backed static objects carry a build-time-written
        // cached hash in the range record (see D-066). Zero means
        // "uncached"; fall through to recompute. Nonzero means the
        // build-time helper (or an explicit descriptor template) has
        // supplied a pointer-key hash; return it directly.
        //
        // Unlike the heap-side path below, we do NOT write back to the
        // range's cached_hash on a recompute: the value is
        // build-time-authoritative for static objects, and runtime
        // recomputes for static-range hits MUST NOT mutate the slot.
        cached = ainfo.hdr.range->cached_hash;
    }

    n00b_alloc_hdr_unpin(pin);
    if (cached) {
        return cached;
    }

    // If the caller didn't supply a hash function, try the vtable for any
    // object with type metadata. Only heap objects get their hash cached.
    if (!fn && has_type_metadata) {
        n00b_option_t(n00b_vtable_entry) vt_opt =
            n00b_obj_core_method(obj, N00B_BI_HASH);

        if (n00b_option_is_set(vt_opt)) {
            fn = (n00b_hash_fn)n00b_option_get(vt_opt);
        }
    }

    if (!fn) {
        fn = n00b_hash_word;
    }

    n00b_uint128_t hv = (*fn)(obj);

    // Cache the result for managed objects.
    if (managed && !explicit_hash) {
        pin   = false;
        ainfo = n00b_find_alloc_info(obj, .pin = &pin);
        if (ainfo.kind == n00b_alloc_inline) {
            ainfo.hdr.in_line->cached_hash = hv;
        }
        else if (ainfo.kind == n00b_alloc_oob) {
            ainfo.hdr.oob->cached_hash = hv;
        }
        n00b_alloc_hdr_unpin(pin);
    }

    return hv;
}

n00b_uint128_t
n00b_hash_word(void *value)
{
    n00b_word_t w = (n00b_word_t)value;

    return n00b_xxh3_128bits_raw(&w, sizeof(n00b_word_t));
}

n00b_uint128_t
n00b_hash_cstring(void *value)
{
    char *s = (char *)value;

    return n00b_xxh3_128bits_raw(s, strlen(s));
}

n00b_uint128_t
n00b_hash_raw(const void *data, size_t len)
{
    return n00b_xxh3_128bits_raw(data, len);
}

n00b_uint128_t
n00b_string_hash(void *key)
{
    n00b_string_t *s = (n00b_string_t *)key;

    if (!s || !s->u8_bytes || !s->data) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh3_128bits_raw(s->data, s->u8_bytes);
}

n00b_uint128_t
n00b_buffer_hash(n00b_buffer_t *b)
{
    if (!b || !b->byte_len) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh3_128bits_raw(b->data, b->byte_len);
}
