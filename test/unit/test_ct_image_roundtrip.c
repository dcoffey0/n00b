#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "adt/list.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/static_objects.h"
#include "text/strings/string_ops.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

#define TEST_MARSHAL_OP_ALLOC   UINT32_C(0xe11cbab0)
#define TEST_MARSHAL_OP_CPATCH  UINT32_C(0xe31cbab0)
#define TEST_MARSHAL_OP_SPATCH  UINT32_C(0xe41cbab0)
#define TEST_MARSHAL_OP_STOP    UINT32_C(0xe51cbab0)
#define TEST_MARSHAL_OP_PSPATCH UINT32_C(0xe61cbab0)
#define TEST_MARSHAL_OP_CBSCAN  UINT32_C(0xe71cbab0)
#define TEST_MARSHAL_OP_FNPATCH UINT32_C(0xe81cbab0)

#define TEST_MARSHAL_PAYLOAD_FRONT_VERSION 1u
#define TEST_PORTABLE_STATIC_TINFO UINT64_C(0x6374696d67737401)

#ifdef _WIN32
__declspec(dllexport) int
ct_image_test_abs(int value)
{
    return abs(value);
}
#define TEST_FN ct_image_test_abs
#else
#define TEST_FN abs
#endif

typedef struct {
    uint64_t marshal_magic;
    uint32_t version;
    uint32_t base_address;
    uint32_t root_offset;
    uint32_t flags;
} test_marshal_stream_header_t;

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
} test_marshal_sized_record_t;

typedef struct {
    uint32_t op;
    uint32_t end_of_stream;
} test_marshal_stop_record_t;

typedef struct ct_node_t {
    struct ct_node_t *next;
    struct ct_node_t *shared;
    int (*fn)(int);
    uint64_t tag;
} ct_node_t;

typedef struct ct_string_graph_t {
    n00b_string_t                *label;
    n00b_list_t(n00b_string_t *)  labels;
} ct_string_graph_t;

typedef struct ct_static_ref_t {
    uint64_t *static_ref;
    uint64_t  tag;
} ct_static_ref_t;

static uint64_t portable_src[3] = {
    UINT64_C(0x1000000000000001),
    UINT64_C(0x1000000000000002),
    UINT64_C(0x1000000000000003),
};
static uint64_t portable_dst[3] = {
    UINT64_C(0x1000000000000001),
    UINT64_C(0x1000000000000002),
    UINT64_C(0x1000000000000003),
};
static const n00b_static_identity_t portable_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "test.ct-image.portable",
    .object_key   = "static-ref",
};

static uint64_t nonportable_static_words[2] = {
    UINT64_C(0x4142434445464748),
    UINT64_C(0x5152535455565758),
};

static n00b_buffer_t *
export_one(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) r = n00b_ct_image_export(root);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static n00b_err_t
export_one_err(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) r = n00b_ct_image_export(root);
    CHECK(n00b_result_is_err(r));
    return n00b_result_get_err(r);
}

static void *
relocate_checked(n00b_buffer_t *buf)
{
    [[n00b::nomap]] n00b_result_t(void *) r =
        n00b_ct_image_relocate_inplace(buf->data, buf->byte_len);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static n00b_err_t
relocate_err(n00b_buffer_t *buf, size_t image_len)
{
    [[n00b::nomap]] n00b_result_t(void *) r =
        n00b_ct_image_relocate_inplace(buf->data, image_len);
    CHECK(n00b_result_is_err(r));
    return n00b_result_get_err(r);
}

static size_t
marshal_first_record_ix(char *stream)
{
    test_marshal_stream_header_t *hdr = (void *)stream;
    CHECK(hdr->version == TEST_MARSHAL_PAYLOAD_FRONT_VERSION);
    return sizeof(*hdr) + hdr->flags;
}

static bool
image_contains_marshal_op(n00b_buffer_t *image, uint32_t wanted_op)
{
    n00b_ct_image_header_t *image_hdr = (void *)image->data;
    char *stream = image->data + image_hdr->marshal_off;
    size_t len = image_hdr->marshal_len;
    test_marshal_stream_header_t *hdr = (void *)stream;
    size_t ix = marshal_first_record_ix(stream);

    while (ix + sizeof(uint32_t) <= len) {
        uint32_t op = *(uint32_t *)(stream + ix);
        if (op == wanted_op) {
            return true;
        }
        if (op == TEST_MARSHAL_OP_ALLOC) {
            ix += sizeof(test_marshal_alloc_record_t);
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
        if (op == TEST_MARSHAL_OP_STOP) {
            ix += sizeof(test_marshal_stop_record_t);
            CHECK(ix == len);
            return false;
        }
        if (op == TEST_MARSHAL_OP_PSPATCH || op == TEST_MARSHAL_OP_CBSCAN
            || op == TEST_MARSHAL_OP_FNPATCH) {
            test_marshal_sized_record_t *rec = (void *)(stream + ix);
            ix += rec->record_len;
            continue;
        }
        CHECK(false);
    }

    CHECK(false);
    return false;
}

static n00b_alloc_range_t *
register_words(uint64_t *words,
               size_t count,
               const n00b_static_identity_t *identity,
               uint32_t flags,
               uint64_t object_id)
{
    (void)n00b_mmap_register(words,
                             words + count,
                             n00b_mmap_static,
                             .file              = identity != nullptr
                                                    ? identity->object_key
                                                    : "test.ct-image.static",
                             .order_id          = object_id,
                             .definitely_unique = false);
    return n00b_static_object_register(words,
                                       count * sizeof(*words),
                                       TEST_PORTABLE_STATIC_TINFO,
                                       .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                       .object_id = object_id,
                                       .identity  = identity,
                                       .flags     = flags);
}

static void
unregister_words(uint64_t *words, size_t count)
{
    n00b_mmap_delete_ranges(n00b_global_mem_map(n00b_get_runtime()),
                            (uint64_t)(uintptr_t)words,
                            (uint64_t)(uintptr_t)(words + count));
}

static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words = ptr_words;
        info.hdr.oob->no_scan   = false;
        info.hdr.oob->scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
        if (info.hdr.oob->hcur != nullptr) {
            info.hdr.oob->hcur->ptr_words = ptr_words;
            info.hdr.oob->hcur->no_scan   = false;
            info.hdr.oob->hcur->scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
        }
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        info.hdr.in_line->ptr_words = ptr_words;
        info.hdr.in_line->no_scan   = false;
        info.hdr.in_line->scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    }
    n00b_alloc_hdr_unpin(pin);
}

static void
test_header_shape(void)
{
    ct_node_t *value = n00b_alloc(ct_node_t);
    set_ptr_words(value, 3);
    value->tag = 11;
    value->next = nullptr;
    value->shared = nullptr;
    value->fn = nullptr;

    n00b_buffer_t *buf = export_one(value);
    CHECK(buf->byte_len > sizeof(n00b_ct_image_header_t));

    n00b_ct_image_header_t *hdr = (void *)buf->data;
    CHECK(hdr->magic == N00B_CT_IMAGE_MAGIC);
    CHECK(hdr->version == N00B_CT_IMAGE_VERSION);
    CHECK(hdr->abi_tag == n00b_ct_image_host_abi_tag());
    CHECK(hdr->marshal_off == sizeof(n00b_ct_image_header_t));
    CHECK(hdr->marshal_len == buf->byte_len - sizeof(n00b_ct_image_header_t));
    CHECK(hdr->root_count == 1);
    CHECK(hdr->flags == 0);
}

static void
test_scalar_round_trip(void)
{
    ct_node_t *value = n00b_alloc(ct_node_t);
    set_ptr_words(value, 3);
    value->tag = 33;
    value->next = nullptr;
    value->shared = nullptr;
    value->fn = nullptr;

    n00b_buffer_t *buf  = export_one(value);
    ct_node_t     *copy = relocate_checked(buf);

    CHECK(copy != value);
    CHECK(copy->tag == value->tag);
    CHECK(copy->next == nullptr);
    CHECK(copy->shared == nullptr);
    CHECK(copy->fn == nullptr);
}

static void
test_graph_and_fnptr_round_trip(void)
{
    ct_node_t *first  = n00b_alloc(ct_node_t);
    ct_node_t *second = n00b_alloc(ct_node_t);
    ct_node_t *shared = n00b_alloc(ct_node_t);
    set_ptr_words(first, 3);
    set_ptr_words(second, 3);
    set_ptr_words(shared, 3);

    first->tag     = 101;
    first->next    = second;
    first->shared  = shared;
    first->fn      = TEST_FN;
    second->tag    = 102;
    second->next   = first;
    second->shared = shared;
    second->fn     = TEST_FN;
    shared->tag    = 103;
    shared->next   = shared;
    shared->shared = first;
    shared->fn     = TEST_FN;

    n00b_buffer_t *buf = export_one(first);
    CHECK(image_contains_marshal_op(buf, TEST_MARSHAL_OP_FNPATCH));

    ct_node_t *copy = relocate_checked(buf);
    CHECK(copy != first);
    CHECK(copy->tag == 101);
    CHECK(copy->next->tag == 102);
    CHECK(copy->shared->tag == 103);
    CHECK(copy->next != second);
    CHECK(copy->shared != shared);
    CHECK(copy->next->next == copy);
    CHECK(copy->next->shared == copy->shared);
    CHECK(copy->shared->next == copy->shared);
    CHECK(copy->shared->shared == copy);
    CHECK(copy->fn == TEST_FN);
    CHECK(copy->next->fn == TEST_FN);
    CHECK(copy->shared->fn == TEST_FN);
}

static void
test_string_graph_round_trip(void)
{
    ct_string_graph_t *src = n00b_alloc(ct_string_graph_t);
    set_ptr_words(src, 2);
    src->label             = n00b_string_from_cstr("ct image string graph");
    src->labels            = n00b_list_new(n00b_string_t *);
    n00b_list_push(src->labels, src->label);
    n00b_list_push(src->labels, n00b_string_from_cstr("tail"));

    n00b_buffer_t      *buf  = export_one(src);
    ct_string_graph_t  *copy = relocate_checked(buf);
    n00b_string_t      *tail = n00b_string_from_cstr("tail");

    CHECK(copy != src);
    CHECK(copy->label != src->label);
    CHECK(n00b_unicode_str_eq(copy->label, src->label));
    CHECK(n00b_list_len(copy->labels) == 2);
    CHECK(n00b_list_get(copy->labels, 0) == copy->label);
    CHECK(n00b_unicode_str_eq(n00b_list_get(copy->labels, 1), tail));
}

static void
test_portable_static_ref_round_trip(void)
{
    (void)register_words(portable_src,
                         3,
                         &portable_identity,
                         N00B_STATIC_OBJECT_F_READONLY,
                         UINT64_C(0x71010001));

    ct_static_ref_t *src = n00b_alloc(ct_static_ref_t);
    set_ptr_words(src, 1);
    src->tag             = UINT64_C(0xcafe);
    src->static_ref      = &portable_src[1];

    n00b_buffer_t *buf = export_one(src);
    CHECK(image_contains_marshal_op(buf, TEST_MARSHAL_OP_PSPATCH));
    CHECK(!image_contains_marshal_op(buf, TEST_MARSHAL_OP_SPATCH));

    unregister_words(portable_src, 3);
    (void)register_words(portable_dst,
                         3,
                         &portable_identity,
                         N00B_STATIC_OBJECT_F_READONLY,
                         UINT64_C(0x71010002));

    ct_static_ref_t *copy = relocate_checked(buf);
    CHECK(copy != src);
    CHECK(copy->tag == src->tag);
    CHECK(copy->static_ref == &portable_dst[1]);
    CHECK(*copy->static_ref == portable_dst[1]);

    unregister_words(portable_dst, 3);
}

static void
test_nonportable_static_ref_rejected(void)
{
    (void)register_words(nonportable_static_words,
                         2,
                         nullptr,
                         N00B_STATIC_OBJECT_F_READONLY,
                         UINT64_C(0x71020001));

    ct_static_ref_t *src = n00b_alloc(ct_static_ref_t);
    set_ptr_words(src, 1);
    src->tag             = UINT64_C(0xbad5);
    src->static_ref      = &nonportable_static_words[0];

    CHECK(export_one_err(src) == N00B_CT_IMAGE_ERR_NONPORTABLE);
    unregister_words(nonportable_static_words, 2);
}

static void
test_bad_images_rejected(void)
{
    n00b_ct_image_header_t bad = {};
    [[n00b::nomap]] n00b_result_t(void *) bad_magic =
        n00b_ct_image_relocate_inplace(&bad, sizeof(bad));
    CHECK(n00b_result_is_err(bad_magic));
    CHECK(n00b_result_get_err(bad_magic) == N00B_CT_IMAGE_ERR_BAD_HEADER);

    ct_node_t *value = n00b_alloc(ct_node_t);
    set_ptr_words(value, 3);
    value->tag = 1;
    value->next = nullptr;
    value->shared = nullptr;
    value->fn = nullptr;

    n00b_buffer_t *buf = export_one(value);
    CHECK(relocate_err(buf, sizeof(n00b_ct_image_header_t) - 1)
          == N00B_CT_IMAGE_ERR_BAD_HEADER);
    CHECK(relocate_err(buf, buf->byte_len - 1) == N00B_CT_IMAGE_ERR_BAD_HEADER);

    n00b_buffer_t *abi = n00b_buffer_copy(buf);
    ((n00b_ct_image_header_t *)abi->data)->abi_tag ^= UINT16_C(1);
    CHECK(relocate_err(abi, abi->byte_len) == N00B_CT_IMAGE_ERR_BAD_ABI);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_header_shape();
    test_scalar_round_trip();
    test_graph_and_fnptr_round_trip();
    test_string_graph_round_trip();
    test_portable_static_ref_round_trip();
    test_nonportable_static_ref_rejected();
    test_bad_images_rejected();

    n00b_shutdown();
    return 0;
}
