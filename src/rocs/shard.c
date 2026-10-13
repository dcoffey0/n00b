#include "core/codegen_abi.h" // n00b_gc_struct_array_t, scan_cb externs
#include "core/epoch.h"
#include "core/hash.h"
#include "core/pool.h"
#include "core/static_objects.h"
#include "conduit/conduit.h"
#include "conduit/print.h"
#include "parsers/json.h"
#include "rocs/shard.h"
#include "internal/rocs/index.h"
#include "util/marshal.h"

#include <string.h>

N00B_CONDUIT_SUBSCRIPTION_IMPL(n00b_store_lifecycle_t);
N00B_CONDUIT_TOPIC_IMPL(n00b_store_lifecycle_t);

static const n00b_gc_struct_array_t rocs_shard_pointer_prefix_shape = {
    .stride = 1,
    .offset = 0,
    .count  = 4,
};

static const n00b_static_identity_t rocs_shard_pointer_prefix_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "rocs",
    .object_key   = "n00b_store_shard_t.pointer-prefix.v1",
};

N00B_STATIC_OBJECT_DESCRIPTOR_WITH_IDENTITY(
    rocs_shard_pointer_prefix_desc,
    &rocs_shard_pointer_prefix_shape,
    sizeof(rocs_shard_pointer_prefix_shape),
    typehash(n00b_gc_struct_array_t),
    N00B_STATIC_OBJECT_F_READONLY,
    N00B_GC_SCAN_KIND_NONE,
    nullptr,
    nullptr,
    UINT64_C(0x524f435300040001),
    &rocs_shard_pointer_prefix_identity);

static const n00b_gc_struct_array_t rocs_list_data_pointer_shape = {
    .stride = 1,
    .offset = 0,
    .count  = 1,
};

static const n00b_static_identity_t rocs_list_data_pointer_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "rocs",
    .object_key   = "n00b_list_t.data-pointer.v1",
};

N00B_STATIC_OBJECT_DESCRIPTOR_WITH_IDENTITY(
    rocs_list_data_pointer_desc,
    &rocs_list_data_pointer_shape,
    sizeof(rocs_list_data_pointer_shape),
    typehash(n00b_gc_struct_array_t),
    N00B_STATIC_OBJECT_F_READONLY,
    N00B_GC_SCAN_KIND_NONE,
    nullptr,
    nullptr,
    UINT64_C(0x524f435300010001),
    &rocs_list_data_pointer_identity);

typedef struct {
    void              *data;
    size_t             len;
    size_t             cap;
    n00b_rwlock_t     *lock;
    n00b_allocator_t  *allocator;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t scan_cb;
    void              *scan_user;
} rocs_list_process_view_t;

static_assert(sizeof(rocs_list_process_view_t)
              == sizeof(n00b_store_posting_ordinal_list_t));
static_assert(offsetof(rocs_list_process_view_t, data)
              == offsetof(n00b_store_posting_ordinal_list_t, data));
static_assert(offsetof(rocs_list_process_view_t, lock)
              == offsetof(n00b_store_posting_ordinal_list_t, lock));
static_assert(offsetof(rocs_list_process_view_t, allocator)
              == offsetof(n00b_store_posting_ordinal_list_t, allocator));

typedef struct {
    rocs_list_process_view_t *list;
    n00b_rwlock_t            *lock;
    n00b_allocator_t         *allocator;
} rocs_list_process_restore_t;

typedef n00b_list_t(rocs_list_process_restore_t) rocs_list_process_restore_list_t;

typedef struct {
    n00b_flagset_t   *flagset;
    n00b_rwlock_t    *lock;
    n00b_allocator_t *allocator;
} rocs_flagset_process_restore_t;

typedef n00b_list_t(rocs_flagset_process_restore_t)
    rocs_flagset_process_restore_list_t;

// The dict header's `allocator` is the only process pointer in it: `lock` is a
// bitfield, and `fn`/`scan_cb` are function pointers the marshaller patches.
typedef struct {
    _n00b_dict_internal_t *dict;
    n00b_allocator_t      *allocator;
} rocs_dict_process_restore_t;

typedef n00b_list_t(rocs_dict_process_restore_t) rocs_dict_process_restore_list_t;

// A dict store and its key/value arrays are epoch allocations: a hidden
// n00b_epoch_hdr_t rides immediately before the payload, inside the same
// allocation, so its `allocator` is a process pointer in the marshalled bytes.
// The store holds real pointers, so it is scanned conservatively (the header
// cannot be skipped) and the word has to be cleared instead. Bucket arrays are
// POD and allocated N00B_GC_SCAN_KIND_NONE, so their headers are never scanned.
typedef struct {
    n00b_epoch_hdr_t *hdr;
    n00b_allocator_t *allocator;
} rocs_epoch_process_restore_t;

typedef n00b_list_t(rocs_epoch_process_restore_t) rocs_epoch_process_restore_list_t;

typedef struct {
    rocs_list_process_restore_list_t    lists;
    rocs_flagset_process_restore_list_t flagsets;
    rocs_dict_process_restore_list_t    dicts;
    rocs_epoch_process_restore_list_t   epochs;
} rocs_shard_process_restore_t;

static void
rocs_apply_struct_field_scan(void *ptr, const n00b_gc_struct_array_t *shape)
{
    n00b_alloc_info_t ai = n00b_find_alloc_info(ptr, .scan_for_header = true);
    if (ai.kind == n00b_alloc_inline && ai.hdr.in_line != nullptr) {
        ai.hdr.in_line->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
        ai.hdr.in_line->scan_cb   = n00b_gc_scan_cb_struct_field;
        ai.hdr.in_line->scan_user = (void *)shape;
    }
}

static void
rocs_apply_scan_kind(void *ptr, n00b_gc_scan_kind_t scan_kind)
{
    n00b_alloc_info_t ai = n00b_find_alloc_info(ptr, .scan_for_header = true);
    bool no_scan = scan_kind == N00B_GC_SCAN_KIND_NONE;
    if (ai.kind == n00b_alloc_inline && ai.hdr.in_line != nullptr) {
        ai.hdr.in_line->scan_kind = scan_kind;
        ai.hdr.in_line->no_scan   = no_scan;
        ai.hdr.in_line->scan_cb   = nullptr;
        ai.hdr.in_line->scan_user = nullptr;
    }
    else if (ai.kind == n00b_alloc_oob && ai.hdr.oob != nullptr) {
        ai.hdr.oob->scan_kind = scan_kind;
        ai.hdr.oob->no_scan   = no_scan;
        ai.hdr.oob->scan_cb   = nullptr;
        ai.hdr.oob->scan_user = nullptr;
    }
}

static void
rocs_shard_scrub_list_process_fields(rocs_shard_process_restore_t *restores,
                                     void                             *list_ptr)
{
    if (restores == nullptr || list_ptr == nullptr) {
        return;
    }

    rocs_list_process_view_t *list = (rocs_list_process_view_t *)list_ptr;
    if (list->lock == nullptr && list->allocator == nullptr) {
        return;
    }

    n00b_list_push(restores->lists,
                   ((rocs_list_process_restore_t){
                       .list      = list,
                       .lock      = list->lock,
                       .allocator = list->allocator,
                   }));
    list->lock      = nullptr;
    list->allocator = nullptr;
}

static void
rocs_shard_scrub_flagset_process_fields(rocs_shard_process_restore_t *restores,
                                        n00b_flagset_t               *flagset)
{
    if (restores == nullptr || flagset == nullptr) {
        return;
    }

    if (flagset->lock == nullptr && flagset->allocator == nullptr) {
        return;
    }

    n00b_list_push(restores->flagsets,
                   ((rocs_flagset_process_restore_t){
                       .flagset   = flagset,
                       .lock      = flagset->lock,
                       .allocator = flagset->allocator,
                   }));
    flagset->lock      = nullptr;
    flagset->allocator = nullptr;
}

// Iteration reads only `store` and the buckets, so a dict scrubbed here is
// still walkable by the foreach that reaches its contents.
static void
rocs_shard_scrub_dict_process_fields(rocs_shard_process_restore_t *restores,
                                     void                         *dict_ptr)
{
    if (restores == nullptr || dict_ptr == nullptr) {
        return;
    }

    _n00b_dict_internal_t *dict = (_n00b_dict_internal_t *)dict_ptr;
    if (dict->allocator == nullptr) {
        return;
    }

    n00b_list_push(restores->dicts,
                   ((rocs_dict_process_restore_t){
                       .dict      = dict,
                       .allocator = dict->allocator,
                   }));
    dict->allocator = nullptr;
}

static void
rocs_shard_scrub_epoch_hdr(rocs_shard_process_restore_t *restores,
                           void                         *payload)
{
    if (restores == nullptr || payload == nullptr) {
        return;
    }

    n00b_epoch_hdr_t *hdr = (n00b_epoch_hdr_t *)((char *)payload
                                                 - sizeof(n00b_epoch_hdr_t));
    if (hdr->allocator == nullptr) {
        return;
    }

    n00b_list_push(restores->epochs,
                   ((rocs_epoch_process_restore_t){
                       .hdr       = hdr,
                       .allocator = hdr->allocator,
                   }));
    hdr->allocator = nullptr;
}

// The store pointer is read once here; seal has already quiesced writers, so
// no migration can swap it between this and the restore.
static void
rocs_shard_scrub_dict_epoch_hdrs(rocs_shard_process_restore_t *restores,
                                 void                         *dict_ptr)
{
    if (restores == nullptr || dict_ptr == nullptr) {
        return;
    }

    _n00b_dict_internal_t *dict = (_n00b_dict_internal_t *)dict_ptr;
    __n00b_internal_type_erased_store_t *store =
        (__n00b_internal_type_erased_store_t *)atomic_load(&dict->store);
    if (store == nullptr) {
        return;
    }

    rocs_shard_scrub_epoch_hdr(restores, store);
    rocs_shard_scrub_epoch_hdr(restores, store->keys);
    rocs_shard_scrub_epoch_hdr(restores, store->values);
}

static rocs_shard_process_restore_t *
rocs_shard_scrub_process_metadata(n00b_store_shard_t *shard,
                                  n00b_allocator_t   *allocator)
{
    rocs_shard_process_restore_t *restores =
        n00b_alloc_with_opts(rocs_shard_process_restore_t,
                             &(n00b_alloc_opts_t){
                                 .allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_NONE,
                             });
    // Scanned, not opaque. Between the scrub and the restore these entries are
    // the only thing referencing the locks and allocators pulled out of the
    // containers, so a collect during marshal would reclaim anything they hold
    // and the restore would put back a dangling pointer.
    restores->lists = n00b_list_new_private(rocs_list_process_restore_t,
                                            .allocator = allocator,
                                            .scan_kind = N00B_GC_SCAN_KIND_ALL);
    restores->flagsets = n00b_list_new_private(
        rocs_flagset_process_restore_t,
        .allocator = allocator,
        .scan_kind = N00B_GC_SCAN_KIND_ALL);
    restores->dicts = n00b_list_new_private(rocs_dict_process_restore_t,
                                            .allocator = allocator,
                                            .scan_kind = N00B_GC_SCAN_KIND_ALL);
    restores->epochs = n00b_list_new_private(rocs_epoch_process_restore_t,
                                             .allocator = allocator,
                                             .scan_kind = N00B_GC_SCAN_KIND_ALL);

    rocs_shard_scrub_list_process_fields(restores, shard->records);
    rocs_shard_scrub_list_process_fields(restores, shard->retain_raw);

    if (shard->columns != nullptr) {
        rocs_shard_scrub_dict_process_fields(restores, shard->columns);
        rocs_shard_scrub_dict_epoch_hdrs(restores, shard->columns);
        n00b_dict_foreach(shard->columns, field, column, {
            (void)field;
            if (column == nullptr) {
                continue;
            }
            rocs_shard_scrub_dict_process_fields(restores, column);
            rocs_shard_scrub_dict_epoch_hdrs(restores, column);
            n00b_dict_foreach(column, key, postings, {
                (void)key;
                if (postings == nullptr
                    || (postings->kind != N00B_STORE_POSTINGS_SPARSE
                        && postings->kind != N00B_STORE_POSTINGS_DENSE)) {
                    continue;
                }
                if (postings->kind == N00B_STORE_POSTINGS_SPARSE) {
                    // Every sealed image is written ascending. Pushes
                    // append, so a list reaches here unordered whenever a
                    // caller indexed out of order, and this is the one sort
                    // that pays for every read the image serves afterwards.
                    //
                    // The bit is therefore always set on what this writes.
                    // The mapped reader still handles it clear, because an
                    // image written before the bit existed has a zero
                    // reserved word, and that one scans instead.
                    rocs_posting_list_ensure_ordered(postings);
                    n00b_assert((n00b_atomic_load(&postings->reserved)
                                 & N00B_STORE_POSTINGS_ORDERED)
                                != 0);

                    rocs_shard_scrub_list_process_fields(restores,
                                                         postings->ordinals);
                }
                else {
                    n00b_flagset_trim(postings->flags);
                    n00b_atomic_store(
                        &postings->count,
                        postings->flags == nullptr
                            ? 0
                            : n00b_flagset_count(postings->flags));
                    rocs_shard_scrub_flagset_process_fields(restores,
                                                            postings->flags);
                }
            });
        });
    }

    return restores;
}

static void
rocs_shard_restore_process_metadata(rocs_shard_process_restore_t *restores)
{
    if (restores == nullptr) {
        return;
    }

    for (size_t i = 0; i < restores->lists.len; i++) {
        rocs_list_process_restore_t restore = restores->lists.data[i];
        if (restore.list == nullptr) {
            continue;
        }
        restore.list->lock      = restore.lock;
        restore.list->allocator = restore.allocator;
    }

    for (size_t i = 0; i < restores->flagsets.len; i++) {
        rocs_flagset_process_restore_t restore = restores->flagsets.data[i];
        if (restore.flagset == nullptr) {
            continue;
        }
        restore.flagset->lock      = restore.lock;
        restore.flagset->allocator = restore.allocator;
    }

    for (size_t i = 0; i < restores->dicts.len; i++) {
        rocs_dict_process_restore_t restore = restores->dicts.data[i];
        if (restore.dict == nullptr) {
            continue;
        }
        restore.dict->allocator = restore.allocator;
    }

    for (size_t i = 0; i < restores->epochs.len; i++) {
        rocs_epoch_process_restore_t restore = restores->epochs.data[i];
        if (restore.hdr == nullptr) {
            continue;
        }
        restore.hdr->allocator = restore.allocator;
    }
}

static n00b_store_record_payload_list_t *
rocs_shard_record_list_new() _kargs
{
    n00b_allocator_t *allocator  = nullptr;
    uint64_t          record_cap = 0;
}
{
    n00b_store_record_payload_list_t *records =
        n00b_alloc_with_opts(n00b_store_record_payload_list_t,
                             &(n00b_alloc_opts_t){
                                 .allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
                                 .scan_cb   = n00b_gc_scan_cb_struct_field,
                                 .scan_user = (void *)&rocs_list_data_pointer_shape,
                             });
    rocs_apply_struct_field_scan(records, &rocs_list_data_pointer_shape);

    // No lock: the list has one writer and lock-free readers (see
    // rocs_shard_records_push). Pre-size to the shard's record ceiling
    // (record_cap, from the seal policy's max_records) so the backing array is
    // allocated once and growth leaves no outgrown copies behind.
    if (record_cap > 0) {
        *records = n00b_list_new_cap_private(n00b_string_t *,
                                             record_cap,
                                             .allocator = allocator,
                                             .scan_kind = N00B_GC_SCAN_KIND_ALL);
    }
    else {
        *records = n00b_list_new_private(n00b_string_t *,
                                         .allocator = allocator,
                                         .scan_kind = N00B_GC_SCAN_KIND_ALL);
    }

    return records;
}

// A hot shard's record list has one writer, the appender, which for a store is
// whoever holds commit_lock. Debug builds abort on a second one. Readers take
// no lock: they acquire the length, then the array, then the slot, so a reader
// that sees a length also sees an array that long, and a filled slot's text.
// Growth leaves the outgrown array where it is, because a reader may still
// hold it and the shard pool zeroes a freed slot for reuse. It goes back with
// the pool when the shard retires.
static void
rocs_shard_records_push(n00b_store_shard_t *shard, n00b_string_t *text)
{
    rocs_index_writer_enter(shard);
    n00b_store_record_payload_list_t *records = shard->records;
    size_t                            len     = records->len;
    if (len + 1 > records->cap) {
        size_t          cap   = n00b_align_closest_pow2_ceil(
            n00b_max(len + 1, records->cap * 2));
        n00b_string_t **grown = n00b_alloc_array_with_opts(
            n00b_string_t *,
            cap,
            &(n00b_alloc_opts_t){.allocator = records->allocator,
                                 .scan_kind = records->scan_kind,
                                 .scan_cb   = records->scan_cb,
                                 .scan_user = records->scan_user});
        if (len != 0) {
            memcpy(grown, records->data, len * sizeof(n00b_string_t *));
        }
        __atomic_store_n(&records->data, grown, __ATOMIC_RELEASE);
        records->cap = cap;
    }
    __atomic_store_n(&records->data[len], text, __ATOMIC_RELEASE);
    __atomic_store_n(&records->len, len + 1, __ATOMIC_RELEASE);
    rocs_index_writer_exit(shard);
}

static void
rocs_shard_records_fill(n00b_store_shard_t *shard,
                        uint64_t            ordinal,
                        n00b_string_t      *text)
{
    rocs_index_writer_enter(shard);
    __atomic_store_n(&shard->records->data[ordinal], text, __ATOMIC_RELEASE);
    rocs_index_writer_exit(shard);
}

static void
rocs_shard_records_drop_tail(n00b_store_shard_t *shard, uint64_t count)
{
    rocs_index_writer_enter(shard);
    size_t len = shard->records->len - (size_t)count;
    __atomic_store_n(&shard->records->len, len, __ATOMIC_RELEASE);
    rocs_index_writer_exit(shard);
}

uint64_t
n00b_store_shard_records_len(n00b_store_shard_t *shard)
{
    if (shard == nullptr || shard->records == nullptr) {
        return 0;
    }
    return (uint64_t)__atomic_load_n(&shard->records->len, __ATOMIC_ACQUIRE);
}

n00b_string_t *
n00b_store_shard_record_at(n00b_store_shard_t *shard, uint64_t ordinal)
{
    if (shard == nullptr || shard->records == nullptr) {
        return nullptr;
    }
    n00b_store_record_payload_list_t *records = shard->records;
    size_t len = __atomic_load_n(&records->len, __ATOMIC_ACQUIRE);
    if (ordinal >= (uint64_t)len) {
        return nullptr;
    }
    n00b_string_t **data = __atomic_load_n(&records->data, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&data[ordinal], __ATOMIC_ACQUIRE);
}

static n00b_store_raw_list_t *
rocs_shard_raw_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_raw_list_t *raw =
        n00b_alloc_with_opts(n00b_store_raw_list_t,
                             &(n00b_alloc_opts_t){
                                 .allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
                                 .scan_cb   = n00b_gc_scan_cb_struct_field,
                                 .scan_user = (void *)&rocs_list_data_pointer_shape,
                             });
    rocs_apply_struct_field_scan(raw, &rocs_list_data_pointer_shape);

    *raw = n00b_list_new_private(n00b_store_raw_span_t *,
                                 .allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_ALL);
    return raw;
}

static n00b_store_raw_blob_t *
rocs_shard_raw_blob_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    /*
     * n00b_store_raw_blob_t is exactly two words: data pointer, then byte_len.
     * EVERY_OTHER scans word 0 only, avoiding a rocs-specific global scan kind.
     */
    n00b_store_raw_blob_t *blob =
        n00b_alloc_with_opts(n00b_store_raw_blob_t,
                             &(n00b_alloc_opts_t){
                                 .allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_EVERY_OTHER,
                             });

    blob->data     = nullptr;
    blob->byte_len = 0;
    rocs_apply_scan_kind(blob, N00B_GC_SCAN_KIND_EVERY_OTHER);
    return blob;
}

static n00b_store_columns_t *
rocs_shard_columns_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_columns_t *columns =
        n00b_alloc_with_opts(n00b_store_columns_t,
                             &(n00b_alloc_opts_t){
                                 .allocator = allocator,
                             });

    n00b_dict_init(columns,
                   .allocator       = allocator,
                   .hash            = n00b_string_hash,
                   .skip_obj_hash   = true,
                   .locked          = true,
                   .key_scan_kind   = N00B_GC_SCAN_KIND_ALL,
                   .value_scan_kind = N00B_GC_SCAN_KIND_ALL);
    return columns;
}

static n00b_store_raw_span_t *
rocs_shard_raw_append(n00b_store_raw_blob_t *blob,
                      n00b_buffer_t         *raw) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    uint64_t raw_len = (uint64_t)n00b_buffer_len(raw);
    uint64_t offset  = blob->byte_len;
    uint64_t new_len = offset + raw_len;

    n00b_store_raw_span_t *span = n00b_alloc_with_opts(
        n00b_store_raw_span_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });
    span->offset   = offset;
    span->byte_len = raw_len;
    rocs_apply_scan_kind(span, N00B_GC_SCAN_KIND_NONE);

    if (raw_len == 0) {
        return span;
    }

    uint8_t *old_data = blob->data;
    uint8_t *new_data = n00b_alloc_array_with_opts(uint8_t,
                                                   new_len,
                                                   &(n00b_alloc_opts_t){
                                                       .allocator = allocator,
                                                       .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                                   });
    rocs_apply_scan_kind(new_data, N00B_GC_SCAN_KIND_NONE);
    if (blob->byte_len != 0) {
        memcpy(new_data, old_data, blob->byte_len);
    }

    _n00b_buffer_rlock(raw);
    memcpy(new_data + offset, raw->data, raw_len);
    _n00b_buffer_unlock(raw);

    blob->data     = new_data;
    blob->byte_len = new_len;
    if (old_data != nullptr) {
        n00b_free(old_data);
    }

    return span;
}

static n00b_result_t(n00b_string_t *)
rocs_shard_encode_record(n00b_json_node_t *record,
                         n00b_allocator_t *allocator)
{
    if (record == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_SHARD_ERR_ARG);
    }

    char *encoded = n00b_json_encode(record,
                                     .pretty = false,
                                     .allocator = allocator);
    if (encoded == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_SHARD_ERR_ARG);
    }

    size_t encoded_len = strlen(encoded);
    if (encoded_len > (size_t)INT64_MAX) {
        n00b_free(encoded);
        return n00b_result_err(n00b_string_t *, N00B_STORE_SHARD_ERR_ARG);
    }

    n00b_string_t *record_text =
        n00b_string_from_raw(encoded,
                             (int64_t)encoded_len,
                             .allocator = allocator);
    n00b_free(encoded);
    if (record_text == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_SHARD_ERR_ARG);
    }

    return n00b_result_ok(n00b_string_t *, record_text);
}

static n00b_result_t(uint64_t)
rocs_shard_record_byte_delta(n00b_store_shard_t *shard,
                             n00b_string_t      *record_text,
                             n00b_buffer_t      *raw)
{
    if (shard == nullptr || record_text == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
    }

    uint64_t add_bytes   = N00B_STORE_SHARD_RECORD_OVERHEAD;
    uint64_t encoded_len = (uint64_t)record_text->u8_bytes;
    uint64_t raw_bytes   = raw == nullptr ? 0 : (uint64_t)n00b_buffer_len(raw);

    if (UINT64_MAX - add_bytes < encoded_len) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
    }
    add_bytes += encoded_len;

    if (shard->retain_raw != nullptr) {
        if (raw == nullptr || shard->raw_bytes == nullptr) {
            return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
        }
        if (raw_bytes > (uint64_t)INT64_MAX) {
            return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
        }
        if (UINT64_MAX - shard->raw_bytes->byte_len < raw_bytes) {
            return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
        }
        if (shard->raw_bytes->byte_len > (uint64_t)INT64_MAX
            || (uint64_t)INT64_MAX - shard->raw_bytes->byte_len < raw_bytes) {
            return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
        }
        if (UINT64_MAX - add_bytes < raw_bytes) {
            return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
        }
        add_bytes += raw_bytes;
    }

    if (UINT64_MAX - shard->byte_estimate < add_bytes) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
    }

    return n00b_result_ok(uint64_t, add_bytes);
}

static n00b_buffer_t *
rocs_shard_marshal_to_allocator(n00b_store_shard_t *shard,
                                uint32_t            base_address) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    // Use the context-based marshal so that on failure we can report the
    // exact marshal status + reason (rotation otherwise flattens every seal
    // failure to N00B_STORE_ERR_INTERNAL and we are blind to the cause).
    //
    // Build the image directly in the caller's allocator.  This used to
    // materialize it in the moving GC heap and then copy it into `allocator`,
    // which for a 150 MB shard meant two buffers, each rounded up to the next
    // power of two -- ~512 MB of allocation, all of it committed on Windows,
    // for one 150 MB image, and the GC-heap half landed in the moving heap in
    // the middle of a seal (n00b-lang/n00b#432).
    //
    // N00B_MARSHAL_F_NO_STW: seal must NOT stop the world (n00b#227).
    //
    // The marshal default became stop-the-world because the marshaller copies
    // raw object bytes into scratch before resolving pointers, so a moving
    // collection mid-marshal mints stale pointers into the image. That hazard
    // does not exist here, and this caller meets the flag's stated condition
    // exactly -- every object in the graph lives in a non-moving pool:
    //
    //   "The hot shard is self-contained: shard_append copies every value
    //    (record bytes, column field-name strings, postings, ordinals,
    //    flagsets, raw spans) into this pool, and the shard holds no pointers
    //    into GC-managed arenas."                          (rocs/store.c:962)
    //
    // The hot allocator is hidden from GC root scanning (no metadata pool, so
    // n00b_mmap_is_gc_scannable is false) and is destroyed wholesale at
    // seal/retire rather than object-by-object, so a collection cannot move
    // anything the marshaller is walking. #432 above removed the GC-heap
    // staging buffer, so the image no longer lands in the moving heap either.
    //
    // Stopping the world here is also actively harmful: seal runs on a rocs
    // worker while the service's HTTP threads are live, so an STW across a
    // whole shard marshal freezes the listener. That is what broke
    // rocs_service_runtime / rocs_service_smoke / rocs_service_health on the
    // Linux leg -- each one passes its startup case and then fails its first
    // request, reproducibly, while rocs_async_seal (which seals with no
    // server to starve) passes.
    n00b_marshal_ctx_t *ctx   = n00b_marshal_ctx_new(
        .flags        = N00B_MARSHAL_F_NO_STW,
        .base_address = base_address);
    n00b_buffer_t      *image = n00b_marshal_incremental(ctx,
                                                         shard,
                                                         .close     = true,
                                                         .allocator = allocator);

    if (image == nullptr) {
        n00b_marshal_status_t st  = n00b_marshal_ctx_status(ctx);
        n00b_string_t        *msg = n00b_marshal_ctx_error(ctx);
        n00b_eprintf(
            "rocs: shard [|#|] marshal FAILED: status=[|#|] ([|#|]) reason=[|#|] "
            "record_count=[|#|] raw_bytes=[|#|]\n",
            shard->shard_id,
            (int64_t)st,
            n00b_marshal_status_name(st),
            msg == nullptr ? r"(none)" : msg,
            shard->record_count,
            shard->raw_bytes == nullptr ? (uint64_t)0 : shard->raw_bytes->byte_len);
        n00b_marshal_ctx_destroy(ctx);
        return nullptr;
    }
    n00b_marshal_ctx_destroy(ctx);

    return image;
}

static bool
rocs_lifecycle_topic_ready(n00b_store_lifecycle_topic_t *topic)
{
    if (topic == nullptr) {
        return true;
    }

    n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;

    return n00b_conduit_topic_is_active(base) && base->conduit != nullptr;
}

static n00b_result_t(bool)
rocs_shard_emit_lifecycle(n00b_store_shard_t           *shard,
                          n00b_store_lifecycle_topic_t *topic,
                          n00b_store_lifecycle_kind_t   kind,
                          uint64_t                      byte_size,
                          n00b_string_t                *drop_reason)
{
    if (topic == nullptr) {
        return n00b_result_ok(bool, false);
    }

    n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;
    if (!rocs_lifecycle_topic_ready(topic)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_result_t(n00b_conduit_publisher_t *) pub_r =
        n00b_conduit_publish_try_claim(base);
    if (n00b_result_is_err(pub_r)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_conduit_publisher_t *pub = n00b_result_get(pub_r);
    n00b_alloc_opts_t         opts = {
        .allocator = base->conduit->allocator,
    };
    n00b_store_lifecycle_msg_t *msg =
        n00b_alloc_with_opts(n00b_store_lifecycle_msg_t, &opts);

    if (msg == nullptr) {
        n00b_conduit_publish_yield(pub);
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = base;
    msg->header.generation = n00b_conduit_topic_generation(base);
    msg->header.epoch      = n00b_conduit_topic_epoch(base);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;
    msg->payload.kind         = kind;
    msg->payload.shard_id     = shard->shard_id;
    msg->payload.record_count = shard->record_count;
    msg->payload.byte_size    = byte_size;
    msg->payload.open_ts      = shard->open_ts;
    msg->payload.seal_ts      = shard->seal_ts;
    msg->payload.drop_reason  = kind == N00B_STORE_LIFECYCLE_DROPPED
                                  ? drop_reason
                                  : nullptr;

    n00b_conduit_topic_deliver_msg(n00b_store_lifecycle_t,
                                   topic,
                                   msg,
                                   N00B_CONDUIT_OP_ALL);
    n00b_conduit_publish_yield(pub);

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_store_lifecycle_publish(n00b_store_lifecycle_topic_t *topic,
                             n00b_store_lifecycle_t        event)
{
    if (topic == nullptr) {
        return n00b_result_ok(bool, false);
    }

    n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;
    if (!rocs_lifecycle_topic_ready(topic)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_result_t(n00b_conduit_publisher_t *) pub_r =
        n00b_conduit_publish_try_claim(base);
    if (n00b_result_is_err(pub_r)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_conduit_publisher_t *pub = n00b_result_get(pub_r);
    n00b_alloc_opts_t         opts = {
        .allocator = base->conduit->allocator,
    };
    n00b_store_lifecycle_msg_t *msg =
        n00b_alloc_with_opts(n00b_store_lifecycle_msg_t, &opts);

    if (msg == nullptr) {
        n00b_conduit_publish_yield(pub);
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = base;
    msg->header.generation = n00b_conduit_topic_generation(base);
    msg->header.epoch      = n00b_conduit_topic_epoch(base);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;
    msg->payload           = event;

    n00b_conduit_topic_deliver_msg(n00b_store_lifecycle_t,
                                   topic,
                                   msg,
                                   N00B_CONDUIT_OP_ALL);
    n00b_conduit_publish_yield(pub);

    return n00b_result_ok(bool, true);
}

n00b_string_t *
n00b_store_shard_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_SHARD_OK:          return r"OK";
    case N00B_STORE_SHARD_ERR_ARG:     return r"ARG";
    case N00B_STORE_SHARD_ERR_STATE:   return r"STATE";
    case N00B_STORE_SHARD_ERR_MARSHAL: return r"MARSHAL";
    case N00B_STORE_SHARD_ERR_EVENT:   return r"EVENT";
    }
    return r"UNKNOWN";
}

n00b_result_t(n00b_store_lifecycle_topic_t *)
n00b_store_lifecycle_topic_get(n00b_conduit_t *conduit,
                               n00b_conduit_uri_t uri)
{
    n00b_store_lifecycle_topic_t *topic =
        n00b_conduit_topic_init(n00b_store_lifecycle_t, conduit, uri);
    if (topic == nullptr) {
        return n00b_result_err(n00b_store_lifecycle_topic_t *,
                               N00B_STORE_SHARD_ERR_EVENT);
    }

    return n00b_result_ok(n00b_store_lifecycle_topic_t *, topic);
}

n00b_result_t(n00b_store_lifecycle_inbox_t *)
n00b_store_lifecycle_inbox_new(n00b_conduit_t *conduit) _kargs
{
    n00b_conduit_backpressure_t backpressure = N00B_CONDUIT_BP_UNBOUNDED;
    uint32_t                    limit        = 0;
    n00b_allocator_t           *allocator    = nullptr;
}
{
    if (conduit == nullptr) {
        return n00b_result_err(n00b_store_lifecycle_inbox_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }

    if (allocator == nullptr) {
        allocator = conduit->allocator;
    }

    n00b_alloc_opts_t opts = {
        .allocator = allocator,
    };
    n00b_store_lifecycle_inbox_t *inbox =
        n00b_alloc_with_opts(n00b_store_lifecycle_inbox_t, &opts);

    if (inbox == nullptr) {
        return n00b_result_err(n00b_store_lifecycle_inbox_t *,
                               N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_conduit_inbox_init(n00b_store_lifecycle_t,
                            inbox,
                            conduit,
                            backpressure,
                            limit);

    return n00b_result_ok(n00b_store_lifecycle_inbox_t *, inbox);
}

n00b_result_t(n00b_conduit_sub_handle_t)
n00b_store_lifecycle_subscribe(n00b_store_lifecycle_topic_t *topic,
                               n00b_store_lifecycle_inbox_t *inbox) _kargs
{
    uint32_t                    operations   = N00B_CONDUIT_OP_ALL;
    uint32_t                    flags        = 0;
    uint32_t                    timeout_ms   = 0;
    n00b_conduit_backpressure_t backpressure = N00B_CONDUIT_BP_UNBOUNDED;
    uint32_t                    inbox_limit  = 0;
}
{
    if (topic == nullptr || inbox == nullptr) {
        return n00b_result_err(n00b_conduit_sub_handle_t,
                               N00B_STORE_SHARD_ERR_ARG);
    }

    if (!rocs_lifecycle_topic_ready(topic)) {
        return n00b_result_err(n00b_conduit_sub_handle_t,
                               N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_conduit_sub_handle_t handle =
        n00b_conduit_subscribe(n00b_store_lifecycle_t,
                               topic,
                               inbox,
                               .operations   = operations,
                               .flags        = flags,
                               .timeout_ms   = timeout_ms,
                               .backpressure = backpressure,
                               .inbox_limit  = inbox_limit);
    if (handle == N00B_CONDUIT_INVALID_SUB_HANDLE) {
        return n00b_result_err(n00b_conduit_sub_handle_t,
                               N00B_STORE_SHARD_ERR_EVENT);
    }

    return n00b_result_ok(n00b_conduit_sub_handle_t, handle);
}

n00b_result_t(n00b_store_shard_t *)
n00b_store_shard_new() _kargs
{
    uint64_t          shard_id   = 0;
    bool              retain_raw = false;
    uint64_t          open_ts    = 0;
    n00b_allocator_t *allocator  = nullptr;
    // Pre-size the (lock-free) record list to the shard's record ceiling so it
    // never reallocs while a lock-free reader holds the backing array. Callers
    // that create the growing hot shard pass the seal policy's max_records; 0
    // (the default) keeps the old default-capacity behavior for callers with no
    // record ceiling.
    uint64_t          record_cap = 0;
}
{
    n00b_alloc_opts_t opts = {
        .allocator = allocator,
        .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
        .scan_cb   = n00b_gc_scan_cb_struct_field,
        .scan_user = (void *)&rocs_shard_pointer_prefix_shape,
    };
    n00b_store_shard_t *shard =
        n00b_alloc_with_opts(n00b_store_shard_t, &opts);

    // Hot pool is hidden/inline/non-metadata so the CALLBACK above is demoted
    // and ncc emits no auto layout for this type; re-apply the manual prefix map
    // on this shard's inline header so the marshal scans only the 4 pointer
    // words and not the scalar byte_estimate.  Marshal/GC read scan_cb from the
    // inline header; only this object is affected.
    n00b_alloc_info_t shard_ai = n00b_find_alloc_info(shard,
                                                      .scan_for_header = true);
    if (shard_ai.kind == n00b_alloc_inline && shard_ai.hdr.in_line != nullptr) {
        shard_ai.hdr.in_line->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
        shard_ai.hdr.in_line->scan_cb   = n00b_gc_scan_cb_struct_field;
        shard_ai.hdr.in_line->scan_user = (void *)&rocs_shard_pointer_prefix_shape;
    }

    shard->records       = rocs_shard_record_list_new(.allocator  = allocator,
                                                       .record_cap = record_cap);
    shard->columns       = rocs_shard_columns_new(.allocator = allocator);
    shard->retain_raw    = retain_raw
                              ? rocs_shard_raw_list_new(.allocator = allocator)
                              : nullptr;
    shard->raw_bytes     = retain_raw
                              ? rocs_shard_raw_blob_new(.allocator = allocator)
                              : nullptr;
    shard->state         = N00B_SHARD_STATE_OPEN;
    shard->reserved      = 0;
    shard->record_count  = 0;
    shard->byte_estimate = 0;
    shard->open_ts       = open_ts;
    shard->seal_ts       = 0;
    shard->shard_id      = shard_id;

    return n00b_result_ok(n00b_store_shard_t *, shard);
}

n00b_result_t(uint64_t)
n00b_store_shard_append(n00b_store_shard_t *shard,
                        n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t *raw = nullptr;
}
{
    auto reserve_r = n00b_store_shard_reserve(shard, 1);
    if (n00b_result_is_err(reserve_r)) {
        return reserve_r;
    }
    uint64_t ordinal = n00b_result_get(reserve_r);

    auto fill_r = n00b_store_shard_fill_reserved(shard,
                                                 ordinal,
                                                 record,
                                                 .raw = raw);
    if (n00b_result_is_err(fill_r)) {
        (void)n00b_store_shard_cancel_tail_reservation(shard, ordinal, 1);
        return n00b_result_err(uint64_t, n00b_result_get_err(fill_r));
    }

    return n00b_result_ok(uint64_t, ordinal);
}

n00b_result_t(uint64_t)
n00b_store_shard_reserve(n00b_store_shard_t *shard,
                         uint64_t            count)
{
    if (shard == nullptr || shard->records == nullptr
        || shard->columns == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_STATE);
    }
    if (UINT64_MAX - shard->record_count < count) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->record_count != (uint64_t)shard->records->len) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_STATE);
    }
    if (shard->retain_raw != nullptr
        && shard->record_count != (uint64_t)n00b_list_len(*shard->retain_raw)) {
        return n00b_result_err(uint64_t, N00B_STORE_SHARD_ERR_STATE);
    }
    uint64_t start = shard->record_count;
    for (uint64_t i = 0; i < count; i++) {
        rocs_shard_records_push(shard, nullptr);
        if (shard->retain_raw != nullptr) {
            n00b_list_push(*shard->retain_raw, nullptr);
        }
    }
    shard->record_count += count;

    return n00b_result_ok(uint64_t, start);
}

n00b_result_t(bool)
n00b_store_shard_fill_reserved(n00b_store_shard_t *shard,
                               uint64_t            ordinal,
                               n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t *raw = nullptr;
}
{
    if (shard == nullptr || record == nullptr || shard->records == nullptr
        || shard->columns == nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (ordinal >= shard->record_count
        || shard->record_count != (uint64_t)n00b_list_len(*shard->records)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (n00b_list_get(*shard->records, (size_t)ordinal) != nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (shard->retain_raw != nullptr
        && (shard->raw_bytes == nullptr
            || shard->record_count != (uint64_t)n00b_list_len(*shard->retain_raw)
            || n00b_list_get(*shard->retain_raw, (size_t)ordinal)
                   != nullptr)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }

    if (shard->retain_raw == nullptr) {
        auto prepared_r = n00b_store_shard_prepare_reserved_slot(
            shard,
            record,
            .raw = raw);
        if (n00b_result_is_err(prepared_r)) {
            return n00b_result_err(bool, n00b_result_get_err(prepared_r));
        }
        return n00b_store_shard_fill_prepared_reserved(
            shard,
            ordinal,
            n00b_result_get(prepared_r));
    }

    auto encoded_r = rocs_shard_encode_record(record, shard->records->allocator);
    if (n00b_result_is_err(encoded_r)) {
        return n00b_result_err(bool, n00b_result_get_err(encoded_r));
    }
    n00b_string_t *record_text = n00b_result_get(encoded_r);

    auto add_r = rocs_shard_record_byte_delta(shard, record_text, raw);
    if (n00b_result_is_err(add_r)) {
        return n00b_result_err(bool, n00b_result_get_err(add_r));
    }

    n00b_store_raw_span_t *raw_span = nullptr;
    if (shard->retain_raw != nullptr) {
        raw_span = rocs_shard_raw_append(
            shard->raw_bytes,
            raw,
            .allocator = shard->retain_raw->allocator);
    }

    rocs_shard_records_fill(shard, ordinal, record_text);
    if (shard->retain_raw != nullptr) {
        n00b_list_set(*shard->retain_raw, (size_t)ordinal, raw_span);
    }
    shard->byte_estimate += n00b_result_get(add_r);

    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_store_shard_prepared_slot_t *)
n00b_store_shard_prepare_reserved_slot(n00b_store_shard_t *shard,
                                       n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t         *raw       = nullptr;
    n00b_store_raw_span_t *raw_span  = nullptr;
    n00b_allocator_t      *allocator = nullptr;
}
{
    if (shard == nullptr || record == nullptr || shard->records == nullptr
        || shard->columns == nullptr) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               N00B_STORE_SHARD_ERR_STATE);
    }
    if (shard->retain_raw != nullptr && raw_span == nullptr) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->retain_raw == nullptr && raw_span != nullptr) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }

    auto encoded_r = rocs_shard_encode_record(record, shard->records->allocator);
    if (n00b_result_is_err(encoded_r)) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               n00b_result_get_err(encoded_r));
    }
    n00b_string_t *record_text = n00b_result_get(encoded_r);

    uint64_t add_bytes = N00B_STORE_SHARD_RECORD_OVERHEAD
                       + (uint64_t)record_text->u8_bytes;
    if (shard->retain_raw == nullptr) {
        auto add_r = rocs_shard_record_byte_delta(shard, record_text, raw);
        if (n00b_result_is_err(add_r)) {
            return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                                   n00b_result_get_err(add_r));
        }
        add_bytes = n00b_result_get(add_r);
    }
    else if (UINT64_MAX - add_bytes < raw_span->byte_len) {
        return n00b_result_err(n00b_store_shard_prepared_slot_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    else {
        add_bytes += raw_span->byte_len;
    }

    n00b_allocator_t *wrapper_allocator =
        allocator == nullptr ? shard->records->allocator : allocator;
    n00b_store_shard_prepared_slot_t *prepared = n00b_alloc_with_opts(
        n00b_store_shard_prepared_slot_t,
        &(n00b_alloc_opts_t){
            .allocator = wrapper_allocator,
        });
    prepared->record_text = record_text;
    prepared->raw_span    = raw_span;
    prepared->byte_delta  = add_bytes;

    return n00b_result_ok(n00b_store_shard_prepared_slot_t *, prepared);
}

n00b_result_t(n00b_store_raw_span_t *)
n00b_store_shard_reserve_raw_span(n00b_store_shard_t *shard,
                                  n00b_buffer_t      *raw) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (shard == nullptr || raw == nullptr || shard->retain_raw == nullptr
        || shard->raw_bytes == nullptr) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_STATE);
    }
    uint64_t raw_len = (uint64_t)n00b_buffer_len(raw);
    if (raw_len > (uint64_t)INT64_MAX) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->raw_bytes->byte_len > (uint64_t)INT64_MAX
        || (uint64_t)INT64_MAX - shard->raw_bytes->byte_len < raw_len) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }
    if (UINT64_MAX - shard->raw_bytes->byte_len < raw_len) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_ARG);
    }

    n00b_allocator_t *span_allocator =
        allocator == nullptr ? shard->retain_raw->allocator : allocator;
    n00b_store_raw_span_t *span =
        rocs_shard_raw_append(shard->raw_bytes,
                              raw,
                              .allocator = span_allocator);
    if (span == nullptr) {
        return n00b_result_err(n00b_store_raw_span_t *,
                               N00B_STORE_SHARD_ERR_EVENT);
    }

    return n00b_result_ok(n00b_store_raw_span_t *, span);
}

n00b_result_t(bool)
n00b_store_shard_fill_prepared_reserved(
    n00b_store_shard_t               *shard,
    uint64_t                          ordinal,
    n00b_store_shard_prepared_slot_t *prepared) _kargs
{
    bool account_byte_estimate = true;
}
{
    if (shard == nullptr || prepared == nullptr
        || prepared->record_text == nullptr || shard->records == nullptr
        || shard->columns == nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (ordinal >= shard->record_count
        || shard->record_count != (uint64_t)n00b_list_len(*shard->records)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (n00b_list_get(*shard->records, (size_t)ordinal) != nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (shard->retain_raw != nullptr
        && (prepared->raw_span == nullptr || shard->retain_raw == nullptr
            || shard->record_count != (uint64_t)n00b_list_len(*shard->retain_raw)
            || n00b_list_get(*shard->retain_raw, (size_t)ordinal)
                   != nullptr)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->retain_raw == nullptr && prepared->raw_span != nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }
    if (account_byte_estimate
        && UINT64_MAX - shard->byte_estimate < prepared->byte_delta) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }

    rocs_shard_records_fill(shard, ordinal, prepared->record_text);
    if (shard->retain_raw != nullptr) {
        n00b_list_set(*shard->retain_raw, (size_t)ordinal, prepared->raw_span);
    }
    if (account_byte_estimate) {
        shard->byte_estimate += prepared->byte_delta;
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_store_shard_cancel_tail_reservation(n00b_store_shard_t *shard,
                                         uint64_t            start,
                                         uint64_t            count)
{
    if (shard == nullptr || shard->records == nullptr) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (count == 0) {
        return n00b_result_ok(bool, true);
    }
    if (start > shard->record_count || count > shard->record_count - start
        || start + count != shard->record_count
        || shard->record_count != (uint64_t)n00b_list_len(*shard->records)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }
    if (shard->retain_raw != nullptr
        && shard->record_count != (uint64_t)n00b_list_len(*shard->retain_raw)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }

    for (uint64_t i = start; i < start + count; i++) {
        if (n00b_list_get(*shard->records, (size_t)i) != nullptr) {
            return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
        }
        if (shard->retain_raw != nullptr
            && n00b_list_get(*shard->retain_raw, (size_t)i) != nullptr) {
            return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
        }
    }

    rocs_shard_records_drop_tail(shard, count);
    for (uint64_t i = 0; shard->retain_raw != nullptr && i < count; i++) {
        (void)n00b_list_pop(n00b_store_raw_span_t *, *shard->retain_raw);
    }
    shard->record_count -= count;

    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_buffer_t *)
n00b_store_shard_seal(n00b_store_shard_t *shard) _kargs
{
    uint64_t                      seal_ts      = 0;
    uint32_t                      base_address = 0;
    n00b_store_lifecycle_topic_t *topic        = nullptr;
    n00b_allocator_t             *allocator    = nullptr;
}
{
    if (shard == nullptr || shard->records == nullptr || shard->columns == nullptr
        || (shard->retain_raw != nullptr && shard->raw_bytes == nullptr)) {
        return n00b_result_err(n00b_buffer_t *, N00B_STORE_SHARD_ERR_ARG);
    }

    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_buffer_t *, N00B_STORE_SHARD_ERR_STATE);
    }

    if (!rocs_lifecycle_topic_ready(topic)) {
        n00b_eprintf("rocs: shard [|#|] seal FAILED: lifecycle topic not ready "
                     "(record_count=[|#|])\n",
                     shard->shard_id,
                     shard->record_count);
        return n00b_result_err(n00b_buffer_t *, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_shard_state_t old_state   = shard->state;
    uint64_t           old_seal_ts = shard->seal_ts;

    shard->state   = N00B_SHARD_STATE_SEALED;
    shard->seal_ts = seal_ts;

    n00b_pool_t       scrub_pool  = {};
    n00b_allocator_t *scrub_alloc = n00b_pool_init(
        &scrub_pool,
        .hidden            = true,
        .external_metadata = true,
        .use_epochs        = false,
        .name              = "rocs_shard_seal_list_scrub");
    rocs_shard_process_restore_t *restores =
        rocs_shard_scrub_process_metadata(shard, scrub_alloc);

    n00b_buffer_t *image = rocs_shard_marshal_to_allocator(shard,
                                                           base_address,
                                                           .allocator = allocator);
    if (image == nullptr) {
        rocs_shard_restore_process_metadata(restores);
        n00b_allocator_destroy(scrub_alloc);
        shard->state   = old_state;
        shard->seal_ts = old_seal_ts;
        return n00b_result_err(n00b_buffer_t *, N00B_STORE_SHARD_ERR_MARSHAL);
    }

    uint64_t image_bytes = (uint64_t)n00b_buffer_len(image);
    auto event_r = rocs_shard_emit_lifecycle(shard,
                                             topic,
                                             N00B_STORE_LIFECYCLE_SEALED,
                                             image_bytes,
                                             nullptr);
    if (n00b_result_is_err(event_r)) {
        n00b_eprintf("rocs: shard [|#|] seal FAILED: sealed-lifecycle emit err=[|#|] "
                     "(record_count=[|#|] image_bytes=[|#|])\n",
                     shard->shard_id,
                     (int64_t)n00b_result_get_err(event_r),
                     shard->record_count,
                     image_bytes);
        rocs_shard_restore_process_metadata(restores);
        n00b_allocator_destroy(scrub_alloc);
        shard->state   = old_state;
        shard->seal_ts = old_seal_ts;
        return n00b_result_err(n00b_buffer_t *, n00b_result_get_err(event_r));
    }

    // The image is a copy, so the hot containers get their locks and
    // allocators back. A store whose later seal step fails reopens this shard
    // and appends to it.
    rocs_shard_restore_process_metadata(restores);
    n00b_allocator_destroy(scrub_alloc);
    return n00b_result_ok(n00b_buffer_t *, image);
}

n00b_result_t(bool)
n00b_store_shard_drop(n00b_store_shard_t *shard) _kargs
{
    n00b_store_lifecycle_topic_t *topic       = nullptr;
    n00b_string_t                *drop_reason = nullptr;
    uint64_t                      byte_size   = 0;
}
{
    if (shard == nullptr || shard->records == nullptr || shard->columns == nullptr
        || (shard->retain_raw != nullptr && shard->raw_bytes == nullptr)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_ARG);
    }

    if (shard->state != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_STATE);
    }

    if (!rocs_lifecycle_topic_ready(topic)) {
        return n00b_result_err(bool, N00B_STORE_SHARD_ERR_EVENT);
    }

    n00b_shard_state_t old_state = shard->state;
    shard->state                 = N00B_SHARD_STATE_DROPPED;

    if (byte_size == 0) {
        byte_size = shard->byte_estimate;
    }

    auto event_r = rocs_shard_emit_lifecycle(shard,
                                             topic,
                                             N00B_STORE_LIFECYCLE_DROPPED,
                                             byte_size,
                                             drop_reason);
    if (n00b_result_is_err(event_r)) {
        shard->state = old_state;
        return n00b_result_err(bool, n00b_result_get_err(event_r));
    }

    return n00b_result_ok(bool, true);
}
