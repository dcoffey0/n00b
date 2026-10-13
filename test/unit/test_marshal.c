#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/codegen_abi_inject.h" // GC/marshal descriptor structs / scan-cb externs used by value
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/arena.h"
#include "core/buffer.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"
#include "util/assert.h"
#include "util/marshal.h"
#include "test_check.h"

#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}

typedef struct marshal_node_t {
    uint64_t               tag;
    uint64_t               scalar;
    struct marshal_node_t *next;
    struct marshal_node_t *alias;
} marshal_node_t;

typedef struct {
    uint64_t  tag;
    uint64_t *static_ref;
} marshal_static_ref_t;

typedef struct {
    struct marshal_node_t *real_ptr;
    uint64_t               scalar_tail;
} marshal_limited_scan_t;

typedef struct {
    void *ptr;
} marshal_precise_ptr_t;

#define TEST_MARSHAL_OP_ALLOC  UINT32_C(0xe11cbab0)
#define TEST_MARSHAL_OP_CPATCH UINT32_C(0xe31cbab0)
#define TEST_MARSHAL_OP_SPATCH UINT32_C(0xe41cbab0)
#define TEST_MARSHAL_OP_STOP   UINT32_C(0xe51cbab0)
#define TEST_MARSHAL_OP_PSPATCH UINT32_C(0xe61cbab0)
#define TEST_MARSHAL_OP_CBSCAN UINT32_C(0xe71cbab0)
#define TEST_MARSHAL_OP_FNPATCH UINT32_C(0xe81cbab0)

#define TEST_MARSHAL_PAYLOAD_FRONT_VERSION 1u

typedef struct {
    uint64_t marshal_magic;
    uint32_t version;
    uint32_t base_address;
    uint32_t root_offset;
    uint32_t flags;
} test_marshal_stream_header_t;

static size_t
test_payload_front_padding(void)
{
    return ((sizeof(test_marshal_stream_header_t) + 15u) & ~((size_t)15u))
         - sizeof(test_marshal_stream_header_t);
}

typedef struct {
    uint32_t op;
    uint32_t flags;
    uint64_t vaddr;
    uint64_t user_len;
    uint64_t payload_len;
    uint64_t tinfo;
    uint32_t ptr_words;
    uint32_t scan_kind;
    uint32_t no_scan;
    uint32_t is_array;
    n00b_uint128_t cached_hash;
} test_marshal_alloc_record_t;

typedef struct {
    uint32_t op;
    uint32_t reserved;
    uint64_t vaddr;
    uint64_t value;
} test_marshal_cpatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t check_len;
    uint64_t vaddr;
    uint64_t static_addr;
    uint64_t static_start;
    uint64_t static_len;
    uint64_t object_id;
    uint8_t  check[16];
} test_marshal_spatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t record_len;
    uint64_t vaddr;
    uint64_t object_offset;
    uint64_t object_len;
    uint64_t tinfo;
    uint32_t flags_mask;
    uint32_t flags_value;
    uint32_t scan_kind;
    uint32_t identity_version;
    uint32_t identity_kind;
    uint32_t namespace_len;
    uint32_t key_len;
    uint32_t check_len;
    uint32_t reserved;
} test_marshal_pspatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t record_len;
} test_marshal_sized_record_t;

typedef struct {
    uint32_t op;
    uint32_t record_len;
    uint64_t vaddr;
    uint32_t name_len;
    uint32_t reserved;
} test_marshal_fnpatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t end_of_stream;
} test_marshal_stop_record_t;

#define TEST_PORTABLE_STATIC_TINFO UINT64_C(0x7073746174696301)

static uint64_t portable_success_src[3] = {
    UINT64_C(0x1000000000000001),
    UINT64_C(0x1000000000000002),
    UINT64_C(0x1000000000000003),
};
static uint64_t portable_success_dst[3] = {
    UINT64_C(0x1000000000000001),
    UINT64_C(0x1000000000000002),
    UINT64_C(0x1000000000000003),
};
static const n00b_static_identity_t portable_success_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "success",
};

static uint64_t portable_missing_src[3] = {
    UINT64_C(0x2000000000000001),
    UINT64_C(0x2000000000000002),
    UINT64_C(0x2000000000000003),
};
static const n00b_static_identity_t portable_missing_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "missing",
};

static uint64_t portable_mutability_src[3] = {
    UINT64_C(0x3000000000000001),
    UINT64_C(0x3000000000000002),
    UINT64_C(0x3000000000000003),
};
static uint64_t portable_mutability_dst[3] = {
    UINT64_C(0x3000000000000001),
    UINT64_C(0x3000000000000002),
    UINT64_C(0x3000000000000003),
};
static const n00b_static_identity_t portable_mutability_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "mutability",
};

static uint64_t portable_duplicate_src[3] = {
    UINT64_C(0x4000000000000001),
    UINT64_C(0x4000000000000002),
    UINT64_C(0x4000000000000003),
};
static uint64_t portable_duplicate_dst_a[3] = {
    UINT64_C(0x4000000000000001),
    UINT64_C(0x4000000000000002),
    UINT64_C(0x4000000000000003),
};
static uint64_t portable_duplicate_dst_b[3] = {
    UINT64_C(0x4000000000000001),
    UINT64_C(0x4000000000000002),
    UINT64_C(0x4000000000000003),
};
static const n00b_static_identity_t portable_duplicate_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "duplicate",
};

static uint64_t portable_check_src[3] = {
    UINT64_C(0x5000000000000001),
    UINT64_C(0x5000000000000002),
    UINT64_C(0x5000000000000003),
};
static uint64_t portable_check_dst[3] = {
    UINT64_C(0x5000000000000001),
    UINT64_C(0x50000000000000ff),
    UINT64_C(0x5000000000000003),
};
static const n00b_static_identity_t portable_check_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "check-bytes",
};

static uint64_t portable_type_src[3] = {
    UINT64_C(0x5100000000000001),
    UINT64_C(0x5100000000000002),
    UINT64_C(0x5100000000000003),
};
static uint64_t portable_type_dst[3] = {
    UINT64_C(0x5100000000000001),
    UINT64_C(0x5100000000000002),
    UINT64_C(0x5100000000000003),
};
static const n00b_static_identity_t portable_type_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "type",
};

static uint64_t portable_scan_src[3] = {
    UINT64_C(0x5200000000000001),
    UINT64_C(0x5200000000000002),
    UINT64_C(0x5200000000000003),
};
static uint64_t portable_scan_dst[3] = {
    UINT64_C(0x5200000000000001),
    UINT64_C(0x5200000000000002),
    UINT64_C(0x5200000000000003),
};
static const n00b_static_identity_t portable_scan_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "scan",
};

static uint64_t portable_length_src[3] = {
    UINT64_C(0x5300000000000001),
    UINT64_C(0x5300000000000002),
    UINT64_C(0x5300000000000003),
};
static uint64_t portable_length_dst[2] = {
    UINT64_C(0x5300000000000001),
    UINT64_C(0x5300000000000002),
};
static const n00b_static_identity_t portable_length_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "length",
};

static uint64_t portable_malformed_words[3] = {
    UINT64_C(0x6000000000000001),
    UINT64_C(0x6000000000000002),
    UINT64_C(0x6000000000000003),
};
static const n00b_static_identity_t portable_malformed_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.marshal.portable",
    .object_key   = "malformed",
};

static n00b_buffer_t *
buffer_copy_with_extra(n00b_buffer_t *buf, int64_t extra)
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)(len + extra));
    memcpy(bytes, buf->data, (size_t)len);
    memset(bytes + len, 0x5a, (size_t)extra);
    _n00b_buffer_unlock(buf);

    return n00b_buffer_from_bytes(bytes, len + extra);
}

static size_t
marshal_first_record_ix(char *bytes, size_t len)
{
    // Single wire format: the metadata records always follow the payload front,
    // whose total size (content + alignment padding) is stored in flags.
    test_marshal_stream_header_t *hdr = (void *)bytes;
    size_t ix = sizeof(*hdr);
    CHECK(hdr->version == TEST_MARSHAL_PAYLOAD_FRONT_VERSION);
    CHECK(hdr->flags <= len - ix);
    ix += hdr->flags;
    return ix;
}

// Byte offset of the payload-front CONTENT base. The single wire format aligns
// the content base to 16 bytes via padding inserted past the header; vaddr
// offsets and root_offset are relative to this base (not to sizeof(header)).
static size_t
marshal_payload_front_base(test_marshal_stream_header_t *hdr)
{
    (void)hdr;
    return (sizeof(test_marshal_stream_header_t) + 15u) & ~(size_t)15u;
}

static n00b_buffer_t *
buffer_copy_with_root_word(n00b_buffer_t *buf, uint64_t value, bool add_cpatch)
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    size_t  extra = add_cpatch ? sizeof(test_marshal_cpatch_record_t) : 0;
    char   *bytes = n00b_alloc_array(char, (size_t)len + extra);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);

    test_marshal_stream_header_t *hdr = (void *)bytes;
    size_t payload_base = marshal_payload_front_base(hdr);
    CHECK(payload_base + hdr->root_offset + sizeof(uint64_t)
          <= sizeof(*hdr) + hdr->flags);
    *(uint64_t *)(bytes + payload_base + hdr->root_offset) = value;

    if (add_cpatch) {
        size_t stop_ix = (size_t)len - sizeof(test_marshal_stop_record_t);
        test_marshal_stop_record_t *stop = (void *)(bytes + stop_ix);
        CHECK(stop->op == TEST_MARSHAL_OP_STOP);
        CHECK(stop->end_of_stream == 1);

        test_marshal_cpatch_record_t patch = {
            .op       = TEST_MARSHAL_OP_CPATCH,
            .reserved = 0,
            .vaddr    = ((uint64_t)hdr->base_address << 32) | hdr->root_offset,
            .value    = value,
        };
        memmove(bytes + stop_ix + sizeof(patch), stop, sizeof(*stop));
        memcpy(bytes + stop_ix, &patch, sizeof(patch));
    }

    return n00b_buffer_from_bytes(bytes, len + (int64_t)extra);
}

static size_t
marshal_alloc_wire_len(test_marshal_stream_header_t *hdr,
                       test_marshal_alloc_record_t  *rec)
{
    // Single wire format: payload lives in the payload front, so the alloc
    // record on the wire is just the fixed record (no trailing inline payload).
    CHECK(hdr->version == TEST_MARSHAL_PAYLOAD_FRONT_VERSION);
    (void)rec;
    return sizeof(*rec);
}

static n00b_buffer_t *
buffer_copy_mutating_alloc(n00b_buffer_t *buf,
                           void (*mutate)(test_marshal_alloc_record_t *rec))
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);

    size_t ix = marshal_first_record_ix(bytes, (size_t)len);
    test_marshal_alloc_record_t *rec = (void *)(bytes + ix);
    CHECK(rec->op == TEST_MARSHAL_OP_ALLOC);
    mutate(rec);

    return n00b_buffer_from_bytes(bytes, len);
}

static n00b_buffer_t *
buffer_copy_mutating_record(n00b_buffer_t *buf,
                            uint32_t       wanted_op,
                            void (*mutate)(void *rec))
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);

    test_marshal_stream_header_t *hdr = (void *)bytes;
    size_t ix = marshal_first_record_ix(bytes, (size_t)len);
    while (ix + sizeof(uint32_t) <= (size_t)len) {
        uint32_t op = *(uint32_t *)(bytes + ix);
        if (op == wanted_op) {
            mutate(bytes + ix);
            return n00b_buffer_from_bytes(bytes, len);
        }

        if (op == TEST_MARSHAL_OP_ALLOC) {
            test_marshal_alloc_record_t *rec = (void *)(bytes + ix);
            ix += marshal_alloc_wire_len(hdr, rec);
            continue;
        }
        if (op == TEST_MARSHAL_OP_CPATCH) {
            ix += sizeof(test_marshal_cpatch_record_t);
            continue;
        }
        if (op == TEST_MARSHAL_OP_SPATCH) {
            ix += sizeof(test_marshal_spatch_record_t);
            continue;
        }
        if (op == TEST_MARSHAL_OP_PSPATCH) {
            test_marshal_pspatch_record_t *rec = (void *)(bytes + ix);
            ix += rec->record_len;
            continue;
        }
        if (op == TEST_MARSHAL_OP_CBSCAN || op == TEST_MARSHAL_OP_FNPATCH) {
            test_marshal_sized_record_t *rec = (void *)(bytes + ix);
            ix += rec->record_len;
            continue;
        }
        break;
    }

    n00b_require(false, "requested marshal record not found");
    return nullptr;
}

static n00b_buffer_t *
buffer_copy_inserting_bad_cpatch_before_stop(n00b_buffer_t *buf)
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char,
                                     (size_t)len + sizeof(test_marshal_cpatch_record_t));
    memcpy(bytes, buf->data, (size_t)len);

    test_marshal_stream_header_t *hdr = (void *)buf->data;
    size_t ix = marshal_first_record_ix(buf->data, (size_t)len);
    while (ix + sizeof(uint32_t) <= (size_t)len) {
        uint32_t op = *(uint32_t *)(buf->data + ix);
        if (op == TEST_MARSHAL_OP_STOP) {
            break;
        }

        if (op == TEST_MARSHAL_OP_ALLOC) {
            test_marshal_alloc_record_t *rec = (void *)(buf->data + ix);
            ix += marshal_alloc_wire_len(hdr, rec);
            continue;
        }
        if (op == TEST_MARSHAL_OP_CPATCH) {
            ix += sizeof(test_marshal_cpatch_record_t);
            continue;
        }
        if (op == TEST_MARSHAL_OP_SPATCH) {
            ix += sizeof(test_marshal_spatch_record_t);
            continue;
        }
        if (op == TEST_MARSHAL_OP_PSPATCH) {
            test_marshal_pspatch_record_t *rec = (void *)(buf->data + ix);
            ix += rec->record_len;
            continue;
        }
        if (op == TEST_MARSHAL_OP_CBSCAN || op == TEST_MARSHAL_OP_FNPATCH) {
            test_marshal_sized_record_t *rec = (void *)(buf->data + ix);
            ix += rec->record_len;
            continue;
        }

        n00b_require(false, "unsupported marshal record while inserting cpatch");
    }

    CHECK(ix + sizeof(test_marshal_stop_record_t) == (size_t)len);

    test_marshal_cpatch_record_t bad = {
        .op       = TEST_MARSHAL_OP_CPATCH,
        .reserved = 0,
        .vaddr    = ((uint64_t)hdr->base_address << 32) | UINT64_C(1),
        .value    = UINT64_C(0xbadc0ffee0ddf00d),
    };
    memcpy(bytes + ix, &bad, sizeof(bad));
    memcpy(bytes + ix + sizeof(bad),
           buf->data + ix,
           sizeof(test_marshal_stop_record_t));
    _n00b_buffer_unlock(buf);

    return n00b_buffer_from_bytes(bytes,
                                  len + (int64_t)sizeof(test_marshal_cpatch_record_t));
}

static void
mutate_unknown_op(test_marshal_alloc_record_t *rec)
{
    rec->op = UINT32_C(0xdeadbeef);
}

static void
mutate_short_payload(test_marshal_alloc_record_t *rec)
{
    assert(rec->payload_len >= sizeof(uint64_t));
    rec->payload_len -= sizeof(uint64_t);
}

static void
mutate_bad_scan_kind(test_marshal_alloc_record_t *rec)
{
    rec->scan_kind = N00B_GC_SCAN_KIND_CALLBACK + 1;
}

static void
mutate_bad_alloc_flags(test_marshal_alloc_record_t *rec)
{
    rec->flags |= UINT32_C(0x80000000);
}

static void
assert_unmarshal_status(n00b_buffer_t *buf, n00b_marshal_status_t status)
{
    n00b_unmarshal_ctx_t *ctx   = n00b_unmarshal_ctx_new();
    n00b_list_t(void *)   roots = n00b_unmarshal_incremental(ctx, buf);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == status);
    n00b_unmarshal_ctx_destroy(ctx);
}

static void
assert_unmarshal_missing_identity_fails_twice(n00b_buffer_t *buf)
{
    assert_unmarshal_status(buf, N00B_MARSHAL_ERR_STATIC_IDENTITY_MISSING);
    assert_unmarshal_status(buf, N00B_MARSHAL_ERR_STATIC_IDENTITY_MISSING);
}

static n00b_alloc_range_t *
register_portable_words(uint64_t *words,
                        size_t count,
                        const n00b_static_identity_t *identity,
                        uint32_t flags,
                        uint64_t object_id);
static void
unregister_portable_words(uint64_t *words, size_t count);

static void
assert_failed_inplace_relocate_discards_deferred_patch(n00b_buffer_t *buf,
                                                       uint64_t      *words,
                                                       const n00b_static_identity_t *identity)
{
    n00b_buffer_t *bad = buffer_copy_inserting_bad_cpatch_before_stop(buf);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);

    n00b_unmarshal_ctx_t *ctx = n00b_unmarshal_ctx_new();
    auto relocate_r = n00b_unmarshal_relocate_inplace(ctx, bad->data, bad->byte_len);
    CHECK(n00b_result_is_err(relocate_r));
    CHECK(n00b_result_get_err(relocate_r) == N00B_MARSHAL_ERR_BAD_STREAM);
    CHECK(n00b_unmarshal_ctx_status(ctx) == N00B_MARSHAL_ERR_BAD_STREAM);
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);

    test_marshal_stream_header_t *hdr = (void *)bad->data;
    marshal_static_ref_t *relocated =
        (void *)(bad->data + marshal_payload_front_base(hdr) + hdr->root_offset);
    CHECK(relocated->static_ref == nullptr);

    (void)register_portable_words(words,
                                  3,
                                  identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x700b0002));
    auto deferred_r = n00b_marshal_apply_deferred_static_patches();
    CHECK(n00b_result_is_ok(deferred_r));
    CHECK(n00b_result_get(deferred_r));
    CHECK(n00b_marshal_deferred_static_patch_count() == 0);
    CHECK(relocated->static_ref == nullptr);
    unregister_portable_words(words, 3);

    n00b_unmarshal_ctx_destroy(ctx);
}

static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words = ptr_words;
        if (info.hdr.oob->hcur) {
            info.hdr.oob->hcur->ptr_words = ptr_words;
        }
    }
    else {
        assert(info.kind == n00b_alloc_inline);
        info.hdr.in_line->ptr_words = ptr_words;
    }
    n00b_alloc_hdr_unpin(pin);
}

static void
set_cached_hash(void *obj, n00b_uint128_t cached_hash)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->cached_hash = cached_hash;
        if (info.hdr.oob->hcur) {
            info.hdr.oob->hcur->cached_hash = cached_hash;
        }
    }
    else {
        assert(info.kind == n00b_alloc_inline);
        info.hdr.in_line->cached_hash = cached_hash;
    }
    n00b_alloc_hdr_unpin(pin);
}

static n00b_uint128_t
get_cached_hash(void *obj)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);
    n00b_uint128_t    hv;

    if (info.kind == n00b_alloc_oob) {
        hv = info.hdr.oob->cached_hash;
    }
    else {
        assert(info.kind == n00b_alloc_inline);
        hv = info.hdr.in_line->cached_hash;
    }
    n00b_alloc_hdr_unpin(pin);
    return hv;
}

static n00b_alloc_range_t *
register_portable_words_ex(uint64_t *words,
                           size_t count,
                           const n00b_static_identity_t *identity,
                           uint32_t flags,
                           uint64_t object_id,
                           n00b_alloc_type_info_t tinfo,
                           n00b_gc_scan_kind_t scan_kind)
{
    (void)n00b_mmap_register(words,
                             words + count,
                             n00b_mmap_static,
                             .file              = identity->object_key,
                             .order_id          = object_id,
                             .definitely_unique = false);
    return n00b_static_object_register(words,
                                       count * sizeof(*words),
                                       tinfo,
                                       .scan_kind = scan_kind,
                                       .object_id = object_id,
                                       .identity  = identity,
                                       .flags     = flags);
}

static n00b_alloc_range_t *
register_portable_words(uint64_t *words,
                        size_t count,
                        const n00b_static_identity_t *identity,
                        uint32_t flags,
                        uint64_t object_id)
{
    return register_portable_words_ex(words,
                                      count,
                                      identity,
                                      flags,
                                      object_id,
                                      TEST_PORTABLE_STATIC_TINFO,
                                      N00B_GC_SCAN_KIND_NONE);
}

static void
unregister_portable_words(uint64_t *words, size_t count)
{
    n00b_mmap_delete_ranges(n00b_global_mem_map(n00b_get_runtime()),
                            (uint64_t)(uintptr_t)words,
                            (uint64_t)(uintptr_t)(words + count));
}

static void
test_cycle_shared_and_collision(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *a = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    marshal_node_t *b = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));

    a->tag    = 0xa0a0;
    a->scalar = (UINT64_C(0x12345678) << 32) | UINT64_C(0x00000042);
    a->next   = b;
    a->alias  = b;

    b->tag    = 0xb0b0;
    b->scalar = 99;
    b->next   = a;
    b->alias  = b;

    n00b_buffer_t *buf = n00b_marshal(a, .base_address = 0x12345678u);
    assert(buf != nullptr);
    assert(n00b_buffer_len(buf) > 0);

    marshal_node_t *root = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(root != nullptr);
    assert(root != a);
    assert(root->tag == 0xa0a0);
    assert(root->scalar == ((UINT64_C(0x12345678) << 32) | UINT64_C(0x00000042)));
    assert(root->next != nullptr);
    assert(root->next == root->alias);
    assert(root->next->tag == 0xb0b0);
    assert(root->next->next == root);
    assert(root->next->alias == root->next);

    n00b_gc_register_root(root);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    assert(root->tag == 0xa0a0);
    assert(root->next->next == root);
    assert(root->next == root->alias);
    n00b_gc_unregister_root(root);

}

static void
test_precise_legacy_pointer_rejected(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_precise_ptr_t *src = n00b_alloc_with_opts(
        marshal_precise_ptr_t,
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)arena,
            .scan_kind = N00B_GC_SCAN_KIND_EVERY_OTHER,
        });

    uint32_t base_address = UINT32_C(0x13572468);
    src->ptr = nullptr;
    n00b_buffer_t *legacy = n00b_marshal(src, .base_address = base_address);
    CHECK(legacy != nullptr);

    uint32_t raw_high = base_address + 1u;
    n00b_buffer_t *raw = buffer_copy_with_root_word(
        legacy,
        ((uint64_t)raw_high << 32) | UINT64_C(0x1000),
        false);
    assert_unmarshal_status(raw, N00B_MARSHAL_ERR_BAD_STREAM);

    uint64_t collision = ((uint64_t)base_address << 32) | UINT64_C(0x1000);
    n00b_buffer_t *cpatch = buffer_copy_with_root_word(legacy, collision, true);
    assert_unmarshal_status(cpatch, N00B_MARSHAL_ERR_BAD_STREAM);
}

static void
test_heap_unmarshal_preserves_cached_hash(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *src = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    src->tag            = 0xca55ed;
    src->scalar         = 77;
    src->next           = nullptr;
    src->alias          = nullptr;

    n00b_uint128_t cached_hash = (n00b_uint128_t)UINT64_C(0xc0ffee1234567890);
    set_cached_hash(src, cached_hash);

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x23456789u);
    assert(buf != nullptr);

    marshal_node_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy != src);
    assert(copy->tag == src->tag);
    assert(copy->scalar == src->scalar);
    assert(copy->next == nullptr);
    assert(copy->alias == nullptr);
    assert(get_cached_hash(copy) == cached_hash);
    assert(n00b_hash(copy, nullptr) == cached_hash);

}

static void
test_static_pointer_patch(void)
{
    static uint64_t static_words[4] = {
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728),
        UINT64_C(0x3132333435363738),
    };

    (void)n00b_mmap_register(static_words,
                             static_words + 4,
                             n00b_mmap_static,
                             .file              = "test_marshal_static_words",
                             .order_id          = UINT64_C(0x6006),
                             .definitely_unique = false);
    (void)n00b_static_object_register(static_words,
                                      sizeof(static_words),
                                      0,
                                      .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                      .object_id = UINT64_C(0x60060001));

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_static_ref_t *src = n00b_alloc_with_opts(marshal_static_ref_t,
                                                     ARENA_OPTS(arena));
    src->tag        = 0xcafe;
    src->static_ref = &static_words[1];

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x3456789au);
    assert(buf != nullptr);

    marshal_static_ref_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy != src);
    assert(copy->tag == 0xcafe);
    assert(copy->static_ref == &static_words[1]);
    assert(*copy->static_ref == static_words[1]);

    n00b_gc_register_root(copy);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    assert(copy->static_ref == &static_words[1]);
    n00b_gc_unregister_root(copy);

}

static n00b_buffer_t *
marshal_portable_ref(n00b_arena_t *arena,
                     uint64_t *words,
                     const n00b_static_identity_t *identity,
                     uint32_t flags,
                     uint64_t object_id,
                     uint32_t base_address)
{
    n00b_alloc_range_t *range = register_portable_words(words, 3, identity, flags, object_id);
    auto range_opt = n00b_mmap_range_by_address(&words[1]);
    assert(n00b_option_is_set(range_opt));
    assert(n00b_option_get(range_opt) == range);

    marshal_static_ref_t *src = n00b_alloc_with_opts(marshal_static_ref_t,
                                                     ARENA_OPTS(arena));
    src->tag        = UINT64_C(0xcafe);
    src->static_ref = &words[1];

    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(.base_address = base_address);
    n00b_buffer_t *buf = n00b_marshal_incremental(ctx, src);
    assert(buf != nullptr);
    n00b_marshal_ctx_destroy(ctx);
    return buf;
}

static void
test_portable_static_pointer_relocation(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    n00b_buffer_t *success = marshal_portable_ref(arena,
                                                  portable_success_src,
                                                  &portable_success_identity,
                                                  N00B_STATIC_OBJECT_F_READONLY,
                                                  UINT64_C(0x70010001),
                                                  0x13572468u);
    unregister_portable_words(portable_success_src, 3);
    (void)register_portable_words(portable_success_dst,
                                  3,
                                  &portable_success_identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x70010002));
    marshal_static_ref_t *copy = n00b_unmarshal_one(success, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy->static_ref == &portable_success_dst[1]);
    assert(*copy->static_ref == portable_success_dst[1]);

    n00b_buffer_t *missing = marshal_portable_ref(arena,
                                                  portable_missing_src,
                                                  &portable_missing_identity,
                                                  N00B_STATIC_OBJECT_F_READONLY,
                                                  UINT64_C(0x70020001),
                                                  0x13572469u);
    unregister_portable_words(portable_missing_src, 3);
    assert_unmarshal_missing_identity_fails_twice(missing);

    n00b_buffer_t *mutability = marshal_portable_ref(arena,
                                                     portable_mutability_src,
                                                     &portable_mutability_identity,
                                                     N00B_STATIC_OBJECT_F_READONLY,
                                                     UINT64_C(0x70030001),
                                                     0x1357246au);
    unregister_portable_words(portable_mutability_src, 3);
    (void)register_portable_words(portable_mutability_dst,
                                  3,
                                  &portable_mutability_identity,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  UINT64_C(0x70030002));
    assert_unmarshal_status(mutability, N00B_MARSHAL_ERR_STATIC_IDENTITY_MUTABILITY);

    n00b_buffer_t *duplicate = marshal_portable_ref(arena,
                                                    portable_duplicate_src,
                                                    &portable_duplicate_identity,
                                                    N00B_STATIC_OBJECT_F_READONLY,
                                                    UINT64_C(0x70040001),
                                                    0x1357246bu);
    unregister_portable_words(portable_duplicate_src, 3);
    (void)register_portable_words(portable_duplicate_dst_a,
                                  3,
                                  &portable_duplicate_identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x70040002));
    (void)register_portable_words(portable_duplicate_dst_b,
                                  3,
                                  &portable_duplicate_identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x70040003));
    assert_unmarshal_status(duplicate, N00B_MARSHAL_ERR_STATIC_IDENTITY_DUPLICATE);

    n00b_buffer_t *check_bytes = marshal_portable_ref(arena,
                                                      portable_check_src,
                                                      &portable_check_identity,
                                                      N00B_STATIC_OBJECT_F_READONLY,
                                                      UINT64_C(0x70050001),
                                                      0x1357246cu);
    unregister_portable_words(portable_check_src, 3);
    (void)register_portable_words(portable_check_dst,
                                  3,
                                  &portable_check_identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x70050002));
    assert_unmarshal_status(check_bytes, N00B_MARSHAL_ERR_STATIC_IDENTITY_CHECK_BYTES);

    n00b_buffer_t *type = marshal_portable_ref(arena,
                                               portable_type_src,
                                               &portable_type_identity,
                                               N00B_STATIC_OBJECT_F_READONLY,
                                               UINT64_C(0x70080001),
                                               0x1357246eu);
    unregister_portable_words(portable_type_src, 3);
    (void)register_portable_words_ex(portable_type_dst,
                                     3,
                                     &portable_type_identity,
                                     N00B_STATIC_OBJECT_F_READONLY,
                                     UINT64_C(0x70080002),
                                     TEST_PORTABLE_STATIC_TINFO ^ UINT64_C(1),
                                     N00B_GC_SCAN_KIND_NONE);
    assert_unmarshal_status(type, N00B_MARSHAL_ERR_STATIC_IDENTITY_TYPE);

    n00b_buffer_t *scan = marshal_portable_ref(arena,
                                               portable_scan_src,
                                               &portable_scan_identity,
                                               N00B_STATIC_OBJECT_F_READONLY,
                                               UINT64_C(0x70090001),
                                               0x1357246fu);
    unregister_portable_words(portable_scan_src, 3);
    (void)register_portable_words_ex(portable_scan_dst,
                                     3,
                                     &portable_scan_identity,
                                     N00B_STATIC_OBJECT_F_READONLY,
                                     UINT64_C(0x70090002),
                                     TEST_PORTABLE_STATIC_TINFO,
                                     N00B_GC_SCAN_KIND_ALL);
    assert_unmarshal_status(scan, N00B_MARSHAL_ERR_STATIC_IDENTITY_SCAN);

    n00b_buffer_t *length = marshal_portable_ref(arena,
                                                 portable_length_src,
                                                 &portable_length_identity,
                                                 N00B_STATIC_OBJECT_F_READONLY,
                                                 UINT64_C(0x700a0001),
                                                 0x13572470u);
    unregister_portable_words(portable_length_src, 3);
    (void)register_portable_words(portable_length_dst,
                                  2,
                                  &portable_length_identity,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  UINT64_C(0x700a0002));
    assert_unmarshal_status(length, N00B_MARSHAL_ERR_STATIC_IDENTITY_LENGTH);

    uint64_t *unsupported_static_ptr =
        (uint64_t *)(uintptr_t)UINT64_C(0x700000000000);
    (void)n00b_mmap_register(unsupported_static_ptr,
                             unsupported_static_ptr + 2,
                             n00b_mmap_static,
                             .file              = "unsupported-static-words",
                             .order_id          = UINT64_C(0x70060001),
                             .definitely_unique = false);
    marshal_static_ref_t *unsupported = n00b_alloc_with_opts(marshal_static_ref_t,
                                                             ARENA_OPTS(arena));
    unsupported->tag        = UINT64_C(0x70060002);
    unsupported->static_ref = unsupported_static_ptr;
    n00b_marshal_ctx_t *mctx = n00b_marshal_ctx_new(.base_address = 0x1357246du);
    assert(n00b_marshal_incremental(mctx, unsupported) == nullptr);
    assert(n00b_marshal_ctx_status(mctx)
           == N00B_MARSHAL_ERR_UNSUPPORTED_STATIC_POINTER);
    n00b_marshal_ctx_destroy(mctx);

}

static void
test_ptr_words_limits_scan_extent(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *decoy = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    decoy->tag            = 0xd0d0;
    decoy->scalar         = 88;
    decoy->next           = nullptr;
    decoy->alias          = nullptr;

    marshal_limited_scan_t *src = n00b_alloc_with_opts(marshal_limited_scan_t,
                                                       ARENA_OPTS(arena));
    src->real_ptr    = decoy;
    src->scalar_tail = (uint64_t)(uintptr_t)decoy;
    set_ptr_words(src, 1);

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x65432110u);
    assert(buf != nullptr);

    marshal_limited_scan_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy != src);
    assert(copy->real_ptr != nullptr);
    assert(copy->real_ptr != decoy);
    assert(copy->real_ptr->tag == decoy->tag);
    assert(copy->scalar_tail == (uint64_t)(uintptr_t)decoy);
    assert(copy->scalar_tail != (uint64_t)(uintptr_t)copy->real_ptr);

    bool              copy_pin  = false;
    n00b_alloc_info_t copy_info = n00b_find_alloc_info(copy, .pin = &copy_pin);
    if (copy_info.kind == n00b_alloc_oob) {
        assert(copy_info.hdr.oob->ptr_words == 1);
    }
    else {
        assert(copy_info.kind == n00b_alloc_inline);
        assert(copy_info.hdr.in_line->ptr_words == 1);
    }
    n00b_alloc_hdr_unpin(copy_pin);

}

static void
mutate_ptr_words_too_large(test_marshal_alloc_record_t *rec)
{
    rec->ptr_words = (uint32_t)(rec->user_len / sizeof(uint64_t)) + 1;
}

static void
mutate_cpatch_unaligned(void *rec)
{
    ((test_marshal_cpatch_record_t *)rec)->vaddr += 1;
}

static void
mutate_spatch_unaligned(void *rec)
{
    ((test_marshal_spatch_record_t *)rec)->vaddr += 1;
}

static void
mutate_pspatch_unaligned(void *rec)
{
    ((test_marshal_pspatch_record_t *)rec)->vaddr += 1;
}

static void
test_bad_ptr_words_rejected(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *node = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    node->tag            = 0xf00d;
    node->scalar         = 5;
    node->next           = nullptr;
    node->alias          = nullptr;

    n00b_buffer_t *buf = n00b_marshal(node, .base_address = 0x76543210u);
    assert(buf != nullptr);
    n00b_buffer_t *bad = buffer_copy_mutating_alloc(buf, mutate_ptr_words_too_large);
    assert_unmarshal_status(bad, N00B_MARSHAL_ERR_BAD_STREAM);

}

static void
test_malformed_stream_hardening(void)
{
    static uint64_t static_words[2] = {
        UINT64_C(0x4142434445464748),
        UINT64_C(0x5152535455565758),
    };

    (void)n00b_mmap_register(static_words,
                             static_words + 2,
                             n00b_mmap_static,
                             .file              = "test_marshal_bad_spatch_words",
                             .order_id          = UINT64_C(0x6007),
                             .definitely_unique = false);
    (void)n00b_static_object_register(static_words,
                                      sizeof(static_words),
                                      0,
                                      .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                      .object_id = UINT64_C(0x60070001));

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *node = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    node->tag            = 0xabab;
    node->scalar         = (UINT64_C(0x56789abc) << 32) | UINT64_C(5);
    node->next           = nullptr;
    node->alias          = nullptr;

    marshal_static_ref_t *static_ref = n00b_alloc_with_opts(marshal_static_ref_t,
                                                            ARENA_OPTS(arena));
    static_ref->tag        = 0xfefe;
    static_ref->static_ref = &static_words[0];

    n00b_buffer_t *buf = n00b_marshal(node, .base_address = 0x56789abcu);
    assert(buf != nullptr);

    n00b_buffer_t *trailing = buffer_copy_with_extra(buf, 1);
    assert_unmarshal_status(trailing, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *unknown_op = buffer_copy_mutating_alloc(buf, mutate_unknown_op);
    assert_unmarshal_status(unknown_op, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *short_payload = buffer_copy_mutating_alloc(buf, mutate_short_payload);
    assert_unmarshal_status(short_payload, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_kind = buffer_copy_mutating_alloc(buf, mutate_bad_scan_kind);
    assert_unmarshal_status(bad_kind, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_flags = buffer_copy_mutating_alloc(buf, mutate_bad_alloc_flags);
    assert_unmarshal_status(bad_flags, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_cpatch = buffer_copy_mutating_record(buf,
                                                            TEST_MARSHAL_OP_CPATCH,
                                                            mutate_cpatch_unaligned);
    assert_unmarshal_status(bad_cpatch, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *spatch_stream = n00b_marshal(static_ref,
                                                .base_address = 0x56789abcu);
    assert(spatch_stream != nullptr);
    n00b_buffer_t *bad_spatch = buffer_copy_mutating_record(spatch_stream,
                                                            TEST_MARSHAL_OP_SPATCH,
                                                            mutate_spatch_unaligned);
    assert_unmarshal_status(bad_spatch, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *pspatch_stream = marshal_portable_ref(arena,
                                                         portable_malformed_words,
                                                         &portable_malformed_identity,
                                                         N00B_STATIC_OBJECT_F_READONLY,
                                                         UINT64_C(0x70070001),
                                                         0x56789abcu);
    n00b_buffer_t *bad_pspatch = buffer_copy_mutating_record(pspatch_stream,
                                                             TEST_MARSHAL_OP_PSPATCH,
                                                             mutate_pspatch_unaligned);
    assert_unmarshal_status(bad_pspatch, N00B_MARSHAL_ERR_BAD_STREAM);

    unregister_portable_words(portable_malformed_words, 3);
    assert_failed_inplace_relocate_discards_deferred_patch(
        pspatch_stream,
        portable_malformed_words,
        &portable_malformed_identity);

    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);
    ((test_marshal_stream_header_t *)bytes)->marshal_magic ^= UINT64_C(0x55);

    n00b_unmarshal_ctx_t *ctx = n00b_unmarshal_ctx_new();
    n00b_buffer_t        *c1  = n00b_buffer_from_bytes(bytes, 4);
    n00b_list_t(void *) roots = n00b_unmarshal_incremental(ctx, c1);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == N00B_MARSHAL_ERR_INCOMPLETE_STREAM);

    n00b_buffer_t *c2 = n00b_buffer_from_bytes(bytes + 4, len - 4);
    roots = n00b_unmarshal_incremental(ctx, c2);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == N00B_MARSHAL_ERR_BAD_STREAM);
    n00b_unmarshal_ctx_destroy(ctx);

}

static void
test_single_root_context_boundary(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *a = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    marshal_node_t *b = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));

    a->tag = 1;
    b->tag = 2;

    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new();
    assert(n00b_marshal_incremental(ctx, a, .close = false) != nullptr);
    assert(n00b_marshal_incremental(ctx, b, .close = true) == nullptr);
    assert(n00b_marshal_ctx_status(ctx) == N00B_MARSHAL_ERR_CONTEXT_CLOSED);
    n00b_marshal_ctx_destroy(ctx);

}

// Regression: every pool registered during a marshal/unmarshal cycle must be
// unregistered by the time the contexts are destroyed. The old retain_scratch
// path skipped destroying the ctx-embedded scratch pool but freed the ctx
// anyway, leaving a dangling global pool-registry entry over freed memory
// (the 2026-07-15 gateway registry-spin deadlock).
static void
test_unmarshal_scratch_pool_fully_torn_down(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *src = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    src->tag            = 0x7ea2d0;
    src->scalar         = 11;
    src->next           = nullptr;
    src->alias          = nullptr;

    n00b_pool_global_stats_t before = n00b_pool_global_stats();

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x23456789u);
    assert(buf != nullptr);
    marshal_node_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy->tag == src->tag);

    n00b_pool_global_stats_t after = n00b_pool_global_stats();

    uint64_t inits    = after.total_init_count - before.total_init_count;
    uint64_t destroys = after.total_destroy_count - before.total_destroy_count;
    // Both the marshal ctx and the unmarshal ctx register a scratch pool;
    // each must have been unregistered again.
    assert(inits >= 2);
    assert(destroys == inits);

}


// n00b#227: n00b_marshal copies raw object bytes into scratch before resolving
// pointers, so a moving collection during the marshal used to mint stale
// addresses into the image unless the caller passed N00B_MARSHAL_F_STW. STW
// is now the default. This marshals a 2,000-node cyclic graph while a helper
// thread allocates garbage and forces collections as fast as it can; with
// the default flags the image must round-trip intact every time.
typedef struct {
    n00b_arena_t *arena;
    _Atomic bool  stop;
    _Atomic int   collects;
} churn_ctx_t;

static void *
churn_main(void *arg)
{
    churn_ctx_t *ctx = arg;
    while (!atomic_load(&ctx->stop)) {
        for (int i = 0; i < 64; i++) {
            (void)n00b_alloc_array(uint8_t, 4096);
        }
        n00b_collect(n00b_get_runtime()->default_arena);
        atomic_fetch_add(&ctx->collects, 1);
    }
    return nullptr;
}

static void
test_marshal_stops_the_world_by_default(void)
{
    enum { N = 2000 };
    marshal_node_t *nodes[N];
    for (int i = 0; i < N; i++) {
        nodes[i]         = n00b_alloc(marshal_node_t);
        nodes[i]->tag    = 0x1000u + (uint64_t)i;
        nodes[i]->scalar = ((uint64_t)i << 32) | 0x5a5au;
    }
    for (int i = 0; i < N; i++) {
        nodes[i]->next  = nodes[(i + 1) % N];
        nodes[i]->alias = nodes[(i * 7 + 3) % N];
    }

    churn_ctx_t ctx = {.arena = n00b_get_runtime()->default_arena};
    auto        sp  = n00b_thread_spawn(churn_main, &ctx);
    assert(n00b_result_is_ok(sp));
    n00b_thread_t *churn = n00b_result_get(sp);

    for (int round = 0; round < 8; round++) {
        n00b_buffer_t *buf = n00b_marshal(nodes[0]);
        assert(buf != nullptr);
        marshal_node_t *root = n00b_unmarshal_one(buf);
        assert(root != nullptr);
        marshal_node_t *cur = root;
        for (int i = 0; i < N; i++) {
            assert(cur->tag == 0x1000u + (uint64_t)i);
            assert(cur->scalar == (((uint64_t)i << 32) | 0x5a5au));
            assert(cur->alias != nullptr);
            assert(cur->alias->tag == 0x1000u + (uint64_t)((i * 7 + 3) % N));
            cur = cur->next;
            assert(cur != nullptr);
        }
        assert(cur == root); // the cycle closes on the unmarshalled root
    }

    atomic_store(&ctx.stop, true);
    n00b_thread_join(churn);
    // The churn thread must actually have collected during the window, or
    // the test proved nothing.
    assert(atomic_load(&ctx.collects) > 0);
    printf("  [PASS] marshal_stops_the_world_by_default (%d collects)\n",
           atomic_load(&ctx.collects));
}
int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_marshal_stops_the_world_by_default();
    test_cycle_shared_and_collision();
    test_precise_legacy_pointer_rejected();
    test_heap_unmarshal_preserves_cached_hash();
    test_static_pointer_patch();
    test_portable_static_pointer_relocation();
    test_ptr_words_limits_scan_extent();
    test_bad_ptr_words_rejected();
    test_malformed_stream_hardening();
    test_single_root_context_boundary();
    test_unmarshal_scratch_pool_fully_torn_down();
    n00b_shutdown();
    return 0;
}
