#include "rocs/index.h"
#include "core/alloc.h"
#include "core/codegen_abi.h" // n00b_gc_struct_array_t, scan_cb externs
#include "core/static_objects.h"

#include "adt/list.h"
#include "core/hash.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "internal/rocs/index.h"
#include "internal/rocs/store.h"
#include "internal/rocs/json_field.h"
#include "internal/rocs/map.h"
#include "rocs/map.h"
#include "rocs/normalizer.h"
#include "text/strings/string_ops.h"

struct n00b_store_index_t {
    n00b_string_t                 *field;
    n00b_store_index_field_list_t *catch_all_fields;
    n00b_store_index_kind_t        kind;
    n00b_store_postings_kind_t     postings;
    uint8_t                        ngram_n;
    bool                           catch_all;
};

struct n00b_store_record_t {
    n00b_store_pos_t          pos;
    n00b_store_shard_t       *hot_shard;
    n00b_store_map_shard_t   *mapped_shard;
    n00b_json_node_t         *owned_json;
    // Stored compact JSON copied verbatim out of a hot shard. Mutually
    // exclusive with owned_json on construction; n00b_store_record_view_json
    // parses it into owned_json on the first caller that wants a node graph.
    n00b_string_t            *owned_text;
};

typedef n00b_list_t(n00b_uint128_t) rocs_index_key_list_t;
typedef n00b_list_t(n00b_store_posting_list_t *) rocs_hot_posting_list_t;
typedef n00b_list_t(n00b_store_map_posting_list_t *) rocs_mapped_posting_list_t;

// A lookup's answer is the matching ordinals, ascending and unique, in one
// private array. Positions and record views are built from an ordinal when a
// caller asks for one, so a lookup that feeds a planner builds neither.
struct n00b_store_postings_t {
    uint64_t               *ordinals;
    uint64_t                len;
    n00b_allocator_t       *allocator;
    n00b_store_shard_t     *hot_shard;
    n00b_store_map_shard_t *mapped_shard;
    uint64_t                shard_id;
    uint64_t                generation;
};

#ifdef N00B_DEBUG
static _Atomic(uint64_t) rocs_record_views_built = 0;
static _Atomic(uint64_t) rocs_record_parses      = 0;
static _Atomic(uint64_t) rocs_record_field_scans = 0;

uint64_t
n00b_store_record_views_built(void)
{
    return atomic_load_explicit(&rocs_record_views_built, memory_order_relaxed);
}

uint64_t
n00b_store_record_parses(void)
{
    return atomic_load_explicit(&rocs_record_parses, memory_order_relaxed);
}

uint64_t
n00b_store_record_field_scans(void)
{
    return atomic_load_explicit(&rocs_record_field_scans, memory_order_relaxed);
}

void
n00b_store_record_counters_reset(void)
{
    atomic_store_explicit(&rocs_record_views_built, 0, memory_order_relaxed);
    atomic_store_explicit(&rocs_record_parses, 0, memory_order_relaxed);
    atomic_store_explicit(&rocs_record_field_scans, 0, memory_order_relaxed);
}

#define ROCS_COUNT(counter)                                                  \
    atomic_fetch_add_explicit(&(counter), 1, memory_order_relaxed)
#define ROCS_POSTING_STEPS(n) rocs_posting_steps_add(n)
#else
#define ROCS_COUNT(counter)   ((void)0)
#define ROCS_POSTING_STEPS(n) ((void)0)
#endif

static bool
rocs_index_kind_known(n00b_store_index_kind_t kind)
{
    switch (kind) {
    case N00B_STORE_INDEX_TERM:
    case N00B_STORE_INDEX_FULLTEXT:
    case N00B_STORE_INDEX_NGRAM:
    case N00B_STORE_INDEX_NUMERIC:
    case N00B_STORE_INDEX_BOOL:
    case N00B_STORE_INDEX_VECTOR:
        return true;
    case N00B_STORE_INDEX_NONE:
        return false;
    }
    return false;
}

static bool
rocs_index_ngram_n_valid(uint8_t ngram_n)
{
    return ngram_n >= N00B_STORE_NGRAM_MIN_N
        && ngram_n <= N00B_STORE_NGRAM_MAX_N;
}

static bool
rocs_postings_kind_valid(n00b_store_postings_kind_t kind)
{
    return kind == N00B_STORE_POSTINGS_SPARSE
        || kind == N00B_STORE_POSTINGS_DENSE;
}

static int
rocs_u64_compare(const void *left, const void *right)
{
    uint64_t l = *(uint64_t const *)left;
    uint64_t r = *(uint64_t const *)right;

    if (l < r) {
        return -1;
    }
    if (l > r) {
        return 1;
    }
    return 0;
}

// Sort and drop repeats in place, returning the new length. A list already
// ascending skips the sort; the repeat pass is linear either way.
static uint64_t
rocs_ordinals_normalize(uint64_t *ordinals, uint64_t len, bool ascending)
{
    if (len < 2) {
        return len;
    }
    if (!ascending) {
        qsort(ordinals, (size_t)len, sizeof(uint64_t), rocs_u64_compare);
    }

    uint64_t out = 1;
    for (uint64_t i = 1; i < len; i++) {
        if (ordinals[i] != ordinals[out - 1]) {
            ordinals[out++] = ordinals[i];
        }
    }
    return out;
}

static uint64_t *
rocs_ordinals_alloc(uint64_t len, n00b_allocator_t *allocator)
{
    if (len == 0) {
        return nullptr;
    }
    return n00b_alloc_array_with_opts(
        uint64_t,
        (size_t)len,
        &(n00b_alloc_opts_t){.allocator = allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});
}

static n00b_store_postings_t *
rocs_postings_new(uint64_t          shard_id,
                  uint64_t          generation) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_postings_t *postings = n00b_alloc_with_opts(
        n00b_store_postings_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    postings->ordinals     = nullptr;
    postings->len          = 0;
    postings->allocator    = allocator;
    postings->hot_shard    = nullptr;
    postings->mapped_shard = nullptr;
    postings->shard_id     = shard_id;
    postings->generation   = generation;
    return postings;
}

static n00b_store_record_t *
_rocs_record_view_new(n00b_store_pos_t        pos,
                      n00b_store_shard_t     *hot_shard,
                      n00b_store_map_shard_t *mapped_shard) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_record_t *view = n00b_alloc_with_opts(
        n00b_store_record_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    view->pos          = pos;
    view->hot_shard    = hot_shard;
    view->mapped_shard = mapped_shard;
    view->owned_json   = nullptr;
    view->owned_text   = nullptr;
    ROCS_COUNT(rocs_record_views_built);
    return view;
}

extern n00b_result_t(n00b_json_node_t *)
rocs_json_node_copy(n00b_json_node_t *node) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

static n00b_result_t(n00b_json_node_t *)
rocs_json_array_copy(n00b_json_node_t *node,
                     n00b_allocator_t *allocator)
{
    n00b_json_node_t *copy = n00b_json_array_new(.allocator = allocator);
    size_t            len  = n00b_json_array_len(node);

    for (size_t i = 0; i < len; i++) {
        n00b_json_node_t *child = n00b_json_array_get(node, i);
        if (child == nullptr) {
            return n00b_result_err(n00b_json_node_t *,
                                   N00B_STORE_INDEX_ERR_STATE);
        }

        auto child_r = rocs_json_node_copy(child, .allocator = allocator);
        if (n00b_result_is_err(child_r)) {
            return child_r;
        }
        n00b_json_array_push(copy, n00b_result_get(child_r));
    }

    return n00b_result_ok(n00b_json_node_t *, copy);
}

static n00b_result_t(n00b_json_node_t *)
rocs_json_object_copy(n00b_json_node_t *node,
                      n00b_allocator_t *allocator)
{
    auto entries_r = n00b_json_object_entries(node,
                                              .allocator = allocator);
    if (n00b_result_is_err(entries_r)) {
        return n00b_result_err(n00b_json_node_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_json_node_t              *copy    =
        n00b_json_object_new(.allocator = allocator);
    n00b_json_object_entry_list_t *entries = n00b_result_get(entries_r);
    size_t                        len     = n00b_list_len(*entries);

    for (size_t i = 0; i < len; i++) {
        n00b_json_object_entry_t *entry = n00b_list_get(*entries, i);
        if (entry == nullptr || entry->key == nullptr
            || entry->value == nullptr) {
            return n00b_result_err(n00b_json_node_t *,
                                   N00B_STORE_INDEX_ERR_STATE);
        }

        n00b_string_t *key =
            n00b_unicode_str_copy(entry->key, .allocator = allocator);
        if (key == nullptr) {
            return n00b_result_err(n00b_json_node_t *,
                                   N00B_STORE_INDEX_ERR_INTERNAL);
        }

        auto value_r = rocs_json_node_copy(entry->value,
                                           .allocator = allocator);
        if (n00b_result_is_err(value_r)) {
            return value_r;
        }

        n00b_json_object_put_n00b(copy, key, n00b_result_get(value_r));
    }

    return n00b_result_ok(n00b_json_node_t *, copy);
}

n00b_result_t(n00b_json_node_t *)
rocs_json_node_copy(n00b_json_node_t *node) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (node == nullptr) {
        return n00b_result_err(n00b_json_node_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    switch (n00b_json_type(node)) {
    case N00B_JSON_NULL:
        return n00b_result_ok(
            n00b_json_node_t *,
            n00b_json_null_new(.allocator = allocator));
    case N00B_JSON_BOOL:
        return n00b_result_ok(
            n00b_json_node_t *,
            n00b_json_bool_new(n00b_json_as_bool(node),
                               .allocator = allocator));
    case N00B_JSON_INT:
        return n00b_result_ok(
            n00b_json_node_t *,
            n00b_json_int_new(n00b_json_as_i64(node),
                              .allocator = allocator));
    case N00B_JSON_DOUBLE:
        return n00b_result_ok(
            n00b_json_node_t *,
            n00b_json_double_new(n00b_json_as_f64(node),
                                 .allocator = allocator));
    case N00B_JSON_STRING:
        return n00b_result_ok(
            n00b_json_node_t *,
            n00b_json_string_new_from_n00b(n00b_json_as_string(node),
                                           .allocator = allocator));
    case N00B_JSON_ARRAY:
        return rocs_json_array_copy(node, allocator);
    case N00B_JSON_OBJECT:
        return rocs_json_object_copy(node, allocator);
    }

    return n00b_result_err(n00b_json_node_t *,
                           N00B_STORE_INDEX_ERR_STATE);
}

// The stored bytes for one record, borrowed rather than copied and not parsed.
// Both the parsing accessor and the field scan start here.
static n00b_result_t(n00b_string_t *)
rocs_hot_shard_record_stored(n00b_store_shard_t *shard, uint64_t ordinal)
{
    if (shard == nullptr || shard->records == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    if (ordinal >= n00b_store_shard_records_len(shard)) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_string_t *text = n00b_store_shard_record_at(shard, ordinal);
    if (text == nullptr || (text->u8_bytes != 0 && text->data == nullptr)) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    return n00b_result_ok(n00b_string_t *, text);
}

static n00b_result_t(n00b_json_node_t *)
rocs_hot_shard_record_json(n00b_store_shard_t *shard,
                           uint64_t            ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto text_r = rocs_hot_shard_record_stored(shard, ordinal);
    if (n00b_result_is_err(text_r)) {
        return n00b_result_err(n00b_json_node_t *, n00b_result_get_err(text_r));
    }

    n00b_string_t *text = n00b_result_get(text_r);

    const char       *err  = nullptr;
    n00b_json_node_t *node = n00b_json_parse(text->data,
                                             text->u8_bytes,
                                             &err,
                                             .allocator = allocator);
    if (node == nullptr || err != nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    return n00b_result_ok(n00b_json_node_t *, node);
}

// Sibling of rocs_hot_shard_record_json that stops at the stored bytes. A hot
// shard already holds each record as compact JSON, so a caller that wants the
// JSON *string* needs a copy of those bytes and nothing else -- no parse, no
// node graph, no re-encode. Mirrors what the sealed path gets for free from the
// mapped image (n00b_store_map_shard_record_json_string).
n00b_result_t(n00b_string_t *)
rocs_hot_shard_record_text(n00b_store_shard_t *shard,
                           uint64_t            ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    // Live callers bound ordinals by the post-fill publication watermark.
    auto text_r = rocs_hot_shard_record_stored(shard, ordinal);
    if (n00b_result_is_err(text_r)) {
        return n00b_result_err(n00b_string_t *, n00b_result_get_err(text_r));
    }

    n00b_string_t *text = n00b_result_get(text_r);
    n00b_string_t *copy = n00b_string_from_raw(text->data,
                                               (int64_t)text->u8_bytes,
                                               .allocator = allocator);
    if (copy == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    return n00b_result_ok(n00b_string_t *, copy);
}

static n00b_result_t(n00b_store_postings_t *)
rocs_empty_postings(uint64_t shard_id, uint64_t generation) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return n00b_result_ok(n00b_store_postings_t *,
                          rocs_postings_new(shard_id,
                                            generation,
                                            .allocator = allocator));
}

static n00b_err_t
rocs_index_norm_err(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_NORM_ERR_ARG:     return N00B_STORE_INDEX_ERR_ARG;
    case N00B_STORE_NORM_ERR_TYPE:    return N00B_STORE_INDEX_ERR_ARG;
    case N00B_STORE_NORM_ERR_NUMERIC: return N00B_STORE_INDEX_ERR_ARG;
    case N00B_STORE_NORM_ERR_STATE:   return N00B_STORE_INDEX_ERR_STATE;
    default:                          return N00B_STORE_INDEX_ERR_INTERNAL;
    }
}

// n00b#403: the index error enum is public and narrower than the map's, so a
// map error has to be collapsed into it. What it must NOT do is lose which
// one: nine distinct N00B_STORE_MAP_ERR_* codes arrive here and all but ARG
// left as a bare ERR_STATE (-2), which is also what a legitimately non-SEALED
// shard returns. That ambiguity cost #403 three debugging sessions and two
// wrong hypotheses -- a -2 at a call site could mean I/O failure, bad magic,
// a version or layout mismatch, a range or schema problem, a backing-store
// error, a cache error, or simply "this shard is not sealed", and the caller
// has no way to tell those apart.
//
// Keep the public mapping exactly as it was -- this is a diagnosis change,
// not a behaviour change, and callers that switch on these values must not
// move -- but record the underlying code so it can be recovered.
//
// n00b_store_index_last_map_err() returns the most recent map error this
// thread collapsed. Thread-local because concurrent queries would otherwise
// overwrite each other's, which on a 963-shard store is every query.
static _Thread_local n00b_err_t rocs_index_tls_last_map_err = 0;
static _Thread_local int        rocs_index_tls_last_state   = -1;

n00b_err_t
n00b_store_index_last_map_err(void)
{
    return rocs_index_tls_last_map_err;
}

void
n00b_store_index_clear_last_map_err(void)
{
    rocs_index_tls_last_map_err  = 0;
    rocs_index_tls_last_state    = -1;
}

// The OTHER source of a -2, and the one #403 actually hits: a shard whose
// state is not SEALED. It does not pass through rocs_index_map_err at all, so
// without this a -2 is still ambiguous between "some map error" and "not
// sealed" -- which is half the ambiguity this change exists to remove.
// -1 means "no non-sealed state recorded"; 0/1/2 are the n00b_shard_state_t
// values (OPEN / SEALED / DROPPED).
int
n00b_store_index_last_unsealed_state(void)
{
    return rocs_index_tls_last_state;
}

static n00b_err_t
rocs_index_not_sealed(n00b_shard_state_t state)
{
    rocs_index_tls_last_state = (int)state;
    return N00B_STORE_INDEX_ERR_STATE;
}

static n00b_err_t
rocs_index_map_err(n00b_err_t err)
{
    rocs_index_tls_last_map_err = err;

    switch (err) {
    case N00B_STORE_MAP_ERR_ARG: return N00B_STORE_INDEX_ERR_ARG;
    default:                    return N00B_STORE_INDEX_ERR_STATE;
    }
}

// The name, for a log line or a breakpoint. Static storage, no allocation --
// this has to be callable from a crash path and from lldb.
const char *
n00b_store_map_err_name(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_MAP_ERR_ARG:         return "MAP_ERR_ARG";
    case N00B_STORE_MAP_ERR_IO:          return "MAP_ERR_IO";
    case N00B_STORE_MAP_ERR_BAD_MAGIC:   return "MAP_ERR_BAD_MAGIC";
    case N00B_STORE_MAP_ERR_BAD_VERSION: return "MAP_ERR_BAD_VERSION";
    case N00B_STORE_MAP_ERR_BAD_LAYOUT:  return "MAP_ERR_BAD_LAYOUT";
    case N00B_STORE_MAP_ERR_RANGE:       return "MAP_ERR_RANGE";
    case N00B_STORE_MAP_ERR_SCHEMA:      return "MAP_ERR_SCHEMA";
    case N00B_STORE_MAP_ERR_BACKING:     return "MAP_ERR_BACKING";
    case N00B_STORE_MAP_ERR_CACHE:       return "MAP_ERR_CACHE";
    case 0:                              return "none";
    default:                             return "MAP_ERR_<unknown>";
    }
}

static n00b_err_t
rocs_index_hot_ready(n00b_store_index_t *index)
{
    if (index == nullptr) {
        return N00B_STORE_INDEX_ERR_ARG;
    }
    if (index->catch_all) {
        if (index->catch_all_fields == nullptr
            || n00b_list_len(*index->catch_all_fields) == 0
            || index->kind != N00B_STORE_INDEX_FULLTEXT) {
            return N00B_STORE_INDEX_ERR_STATE;
        }
        return N00B_STORE_INDEX_OK;
    }
    if (index->field == nullptr) {
        return N00B_STORE_INDEX_ERR_ARG;
    }
    switch (index->kind) {
    case N00B_STORE_INDEX_TERM:
    case N00B_STORE_INDEX_FULLTEXT:
    case N00B_STORE_INDEX_NGRAM:
        return N00B_STORE_INDEX_OK;
    case N00B_STORE_INDEX_NONE:
    case N00B_STORE_INDEX_NUMERIC:
    case N00B_STORE_INDEX_BOOL:
    case N00B_STORE_INDEX_VECTOR:
        return N00B_STORE_INDEX_ERR_UNREADY;
    }
    return N00B_STORE_INDEX_ERR_UNREADY;
}

// A posting list's only pointers are `ordinals` and `flags`, words 2 and 3.
// The first word packs `kind` with the `reserved` flag word, and once
// N00B_STORE_POSTINGS_ORDERED is set (every sealed sparse list) that word is
// 0x1_0000_0000 | kind: pointer-shaped. A conservative every-word scan takes
// it for a pointer, and the seal marshaller then fails the whole shard with
// unsupported-static-pointer when the value lands in a registered static
// range (n00b-lang/n00b#375; it took out every store-touching path in wax).
//
// The marshal-time precise-scan promotion cannot be relied on to prevent it:
// it needs the link-time GC type-map dictionary, which n00b's own binaries
// carry and downstream consumers linking without the gcmap wrapper do not.
// So the list describes itself, the way the shard root and the record lists
// already do (see rocs_shard_pointer_prefix_shape in shard.c).
static const n00b_gc_struct_array_t rocs_posting_list_pointer_shape = {
    .stride = 1,
    .offset = 2,
    .count  = 2,
};

static const n00b_static_identity_t rocs_posting_list_pointer_identity = {
    .version      = N00B_STATIC_IDENTITY_VERSION,
    .kind         = N00B_STATIC_IDENTITY_MANUAL,
    .namespace_id = "rocs",
    .object_key   = "n00b_store_posting_list_t.pointer-tail.v1",
};

N00B_STATIC_OBJECT_DESCRIPTOR_WITH_IDENTITY(
    rocs_posting_list_pointer_desc,
    &rocs_posting_list_pointer_shape,
    sizeof(rocs_posting_list_pointer_shape),
    typehash(n00b_gc_struct_array_t),
    N00B_STATIC_OBJECT_F_READONLY,
    N00B_GC_SCAN_KIND_NONE,
    nullptr,
    nullptr,
    UINT64_C(0x524f435300050001),
    &rocs_posting_list_pointer_identity);

const n00b_gc_struct_array_t *
rocs_posting_list_scan_shape(void)
{
    return &rocs_posting_list_pointer_shape;
}

void
rocs_posting_list_apply_scan(n00b_store_posting_list_t *postings)
{
    if (postings == nullptr) {
        return;
    }
    // The allocation asked for this shape too; re-applying on the header
    // covers an allocator that could not record a callback at alloc time
    // (a hidden inline-header pool on an older runtime), the same belt and
    // braces the shard root uses.
    bool              pin = false;
    n00b_alloc_info_t ai  = n00b_find_alloc_info(postings,
                                                 .scan_for_header = true,
                                                 .pin             = &pin);
    if (ai.kind == n00b_alloc_inline && ai.hdr.in_line != nullptr) {
        ai.hdr.in_line->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
        ai.hdr.in_line->scan_cb   = n00b_gc_scan_cb_struct_field;
        ai.hdr.in_line->scan_user = (void *)&rocs_posting_list_pointer_shape;
    }
    else if (ai.kind == n00b_alloc_oob && ai.hdr.oob != nullptr) {
        ai.hdr.oob->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
        ai.hdr.oob->scan_cb   = n00b_gc_scan_cb_struct_field;
        ai.hdr.oob->scan_user = (void *)&rocs_posting_list_pointer_shape;
    }
    n00b_alloc_hdr_unpin(pin);
}

n00b_store_posting_list_t *
rocs_posting_list_new() _kargs
{
    n00b_store_postings_kind_t kind      = N00B_STORE_POSTINGS_SPARSE;
    n00b_allocator_t          *allocator = nullptr;
}
{
    n00b_store_posting_list_t *postings = n00b_alloc_with_opts(
        n00b_store_posting_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
            .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
            .scan_cb   = n00b_gc_scan_cb_struct_field,
            .scan_user = (void *)&rocs_posting_list_pointer_shape,
        });
    rocs_posting_list_apply_scan(postings);

    postings->kind     = rocs_postings_kind_valid(kind)
                           ? kind
                           : N00B_STORE_POSTINGS_SPARSE;
    // Empty, so trivially ascending; pushes clear this when one lands low.
    n00b_atomic_store(&postings->reserved, N00B_STORE_POSTINGS_ORDERED);
    n00b_atomic_store(&postings->count, 0);
    postings->ordinals = nullptr;
    postings->flags    = nullptr;

    // No lock on either representation. Each shard's posting lists have one
    // writer at a time (store ingest holds commit_lock; n00b_store_index_add
    // callers must not overlap on a shard), and readers pin the shard and read
    // lock-free below the store's published bound: see rocs_posting_list_push.
    if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
        postings->flags = n00b_flagset_new(.length    = 64,
                                           .locked    = false,
                                           .allocator = allocator);
    }
    else {
        postings->ordinals = n00b_alloc_with_opts(
            n00b_store_posting_ordinal_list_t,
            &(n00b_alloc_opts_t){
                .allocator = allocator,
            });
        *postings->ordinals = n00b_list_new_private(
            uint64_t,
            .allocator = allocator,
            .scan_kind = N00B_GC_SCAN_KIND_NONE);
    }
    return postings;
}

static n00b_store_column_t *
rocs_column_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_column_t *column = n00b_alloc_with_opts(
        n00b_store_column_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    /*
     * Column keys are normalized 128-bit term hashes stored by value. The
     * typed dict's raw-key mode hashes those 16 key bytes for its bucket hv.
     */
    n00b_dict_init(column,
                   .allocator       = allocator,
                   .skip_obj_hash   = true,
                   .locked          = true,
                   .key_scan_kind   = N00B_GC_SCAN_KIND_NONE,
                   .value_scan_kind = N00B_GC_SCAN_KIND_ALL);
    return column;
}

static n00b_result_t(n00b_store_column_t *)
rocs_column_get_or_create(n00b_store_shard_t *shard, n00b_string_t *field)
{
    if (shard == nullptr || shard->columns == nullptr || field == nullptr) {
        return n00b_result_err(n00b_store_column_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    bool found = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns, field, &found);
    if (found) {
        if (column == nullptr) {
            return n00b_result_err(n00b_store_column_t *,
                                   N00B_STORE_INDEX_ERR_STATE);
        }
        return n00b_result_ok(n00b_store_column_t *, column);
    }

    n00b_allocator_t *allocator = shard->columns->allocator;
    column = rocs_column_new(.allocator = allocator);

    /*
     * Store a heap string inside the shard graph, even when the descriptor
     * field is a static r-string. Mapped readers can resolve heap string
     * vaddrs in the sealed image; static pointer patch slots are not shard
     * vaddrs and must not become mapped column keys.
     */
    n00b_string_t *stored_field =
        n00b_string_from_raw(field->data,
                             (int64_t)field->u8_bytes,
                             .allocator = allocator);
    if (stored_field == nullptr) {
        return n00b_result_err(n00b_store_column_t *,
                               N00B_STORE_INDEX_ERR_INTERNAL);
    }
    if (n00b_dict_add(shard->columns, stored_field, column)) {
        return n00b_result_ok(n00b_store_column_t *, column);
    }

    column = n00b_dict_get(shard->columns, field, &found);
    if (!found || column == nullptr) {
        return n00b_result_err(n00b_store_column_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }
    return n00b_result_ok(n00b_store_column_t *, column);
}

static n00b_result_t(n00b_store_posting_list_t *)
rocs_column_postings_get_or_create(n00b_store_column_t *column,
                                   n00b_uint128_t       key,
                                   n00b_store_postings_kind_t kind)
{
    if (column == nullptr) {
        return n00b_result_err(n00b_store_posting_list_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    bool found = false;
    n00b_store_posting_list_t *postings = n00b_dict_get(column, key, &found);
    if (found) {
        if (postings == nullptr) {
            return n00b_result_err(n00b_store_posting_list_t *,
                                   N00B_STORE_INDEX_ERR_STATE);
        }
        return n00b_result_ok(n00b_store_posting_list_t *, postings);
    }

    postings = rocs_posting_list_new(.kind      = kind,
                                     .allocator = column->allocator);
    if (postings == nullptr) {
        return n00b_result_err(n00b_store_posting_list_t *,
                               N00B_STORE_INDEX_ERR_INTERNAL);
    }
    if (n00b_dict_add(column, key, postings)) {
        return n00b_result_ok(n00b_store_posting_list_t *, postings);
    }

    postings = n00b_dict_get(column, key, &found);
    if (!found || postings == nullptr) {
        return n00b_result_err(n00b_store_posting_list_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }
    return n00b_result_ok(n00b_store_posting_list_t *, postings);
}

static n00b_result_t(n00b_option_t(n00b_store_posting_list_t *))
rocs_column_postings_find(n00b_store_column_t *column, n00b_uint128_t key)
{
    if (column == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_posting_list_t *),
                               N00B_STORE_INDEX_ERR_ARG);
    }

    bool found = false;
    n00b_store_posting_list_t *postings = n00b_dict_get(column, key, &found);
    if (!found) {
        return n00b_result_ok(n00b_option_t(n00b_store_posting_list_t *),
                              n00b_option_none(n00b_store_posting_list_t *));
    }
    if (postings == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_posting_list_t *),
                               N00B_STORE_INDEX_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_store_posting_list_t *),
                          n00b_option_set(n00b_store_posting_list_t *,
                                          postings));
}

static n00b_result_t(n00b_uint128_t)
rocs_term_key(n00b_store_index_kind_t kind,
              n00b_store_normalized_t *term) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto hash_r = n00b_store_normalize_hash(kind,
                                            term,
                                            .allocator = allocator);
    if (n00b_result_is_err(hash_r)) {
        return n00b_result_err(n00b_uint128_t,
                               rocs_index_norm_err(n00b_result_get_err(hash_r)));
    }
    return n00b_result_ok(n00b_uint128_t, n00b_result_get(hash_r));
}

static n00b_uint128_t
rocs_column_bucket_hash(n00b_uint128_t key)
{
    return n00b_hash_raw(&key, sizeof(key));
}

#ifdef N00B_DEBUG
// Debug-only claim on a hot shard's posting lists and record list, kept in the
// shard root's reserved word, which is zero whenever no write is in flight and
// so reaches a sealed image as the zero the mapped reader requires.
void
rocs_index_writer_enter(n00b_store_shard_t *shard)
{
    if (shard == nullptr) {
        return;
    }
    uint32_t idle = 0;
    n00b_require(__atomic_compare_exchange_n(&shard->reserved,
                                             &idle,
                                             1u,
                                             false,
                                             __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED),
                 "two threads are writing one hot shard");
}

void
rocs_index_writer_exit(n00b_store_shard_t *shard)
{
    if (shard == nullptr) {
        return;
    }
    __atomic_store_n(&shard->reserved, 0u, __ATOMIC_RELEASE);
}

static _Atomic(rocs_posting_read_hook_t) rocs_posting_read_hook     = nullptr;
static void *_Atomic                     rocs_posting_read_hook_ctx = nullptr;

void
rocs_posting_read_hook_set(rocs_posting_read_hook_t hook, void *ctx)
{
    atomic_store(&rocs_posting_read_hook_ctx, ctx);
    atomic_store(&rocs_posting_read_hook, hook);
}

static void
rocs_posting_read_point(n00b_store_posting_list_t *postings)
{
    rocs_posting_read_hook_t hook = atomic_load(&rocs_posting_read_hook);
    if (hook != nullptr) {
        hook(postings, atomic_load(&rocs_posting_read_hook_ctx));
    }
}
#else
#define rocs_posting_read_point(postings) ((void)(postings))
#endif

// A consistent prefix of a sparse list, for a reader that holds no lock.
//
// The length is acquired before the array: the writer publishes a grown array
// before any length that needs it, so the array loaded second holds at least
// that many elements. The order bit is read last, because the writer clears it
// before publishing the length that exposes an out-of-order element.
static const uint64_t *
rocs_posting_sparse_view(n00b_store_posting_list_t *postings,
                         size_t                    *len_out,
                         bool                      *ordered_out)
{
    n00b_store_posting_ordinal_list_t *list = postings->ordinals;
    size_t          len  = __atomic_load_n(&list->len, __ATOMIC_ACQUIRE);
    const uint64_t *data = __atomic_load_n(&list->data, __ATOMIC_ACQUIRE);

    *len_out     = len;
    *ordered_out = (n00b_atomic_load(&postings->reserved)
                    & N00B_STORE_POSTINGS_ORDERED)
                != 0;
    rocs_posting_read_point(postings);
    return data;
}

// Put a sparse list in ascending order, and drop repeats.
//
// Only the shard's sealer calls this, after the store has drained every hot
// reader, so it sorts in place with nothing to exclude. A reader must not: it
// would renumber the list under any other reader walking it. Readers that want
// order either take a private copy and sort that
// (rocs_posting_snapshot_ordinals) or consult N00B_STORE_POSTINGS_ORDERED and
// scan when it is clear.
//
// Pushes append, so a list stays ascending until an ordinal arrives below the
// tail. Ordering here rather than placing on arrival is what keeps indexing n
// records O(n) instead of O(n^2) when a caller supplies them in any order but
// ascending, which n00b_store_index_add permits.
void
rocs_posting_list_ensure_ordered(n00b_store_posting_list_t *postings)
{
    if (postings == nullptr || postings->kind != N00B_STORE_POSTINGS_SPARSE
        || postings->ordinals == nullptr) {
        return;
    }
    if ((n00b_atomic_load(&postings->reserved)
         & N00B_STORE_POSTINGS_ORDERED)
        != 0) {
        return;
    }

    size_t len = postings->ordinals->len;
    if (len > 1) {
        qsort(postings->ordinals->data, len, sizeof(uint64_t),
              rocs_u64_compare);

        size_t out = 1;
        for (size_t i = 1; i < len; i++) {
            if (postings->ordinals->data[i]
                != postings->ordinals->data[out - 1]) {
                postings->ordinals->data[out++] = postings->ordinals->data[i];
            }
        }
        postings->ordinals->len = out;
        n00b_atomic_store(&postings->count, (uint64_t)out);
    }
    n00b_atomic_or(&postings->reserved, N00B_STORE_POSTINGS_ORDERED);
}

static bool
rocs_posting_list_contains_ordinal(n00b_store_posting_list_t *postings,
                                   uint64_t                   ordinal)
{
    if (postings == nullptr) {
        return false;
    }

    if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
        if (postings->flags == nullptr) {
            return false;
        }
        const uint64_t *words   = nullptr;
        uint64_t        wordlen = 0;
        n00b_flagset_view(postings->flags, &words, &wordlen);
        rocs_posting_read_point(postings);
        if ((ordinal >> 6) >= wordlen) {
            return false;
        }
        uint64_t word = __atomic_load_n(&words[ordinal >> 6], __ATOMIC_RELAXED);
        return (word & (1ull << (ordinal & 63u))) != 0;
    }

    if (postings->ordinals == nullptr) {
        return false;
    }

    // Note this is not the push path's tail check, which is a dedup predicate
    // and answers membership only for the ordinal being appended.
    //
    // The prefix below the loaded length never moves while the shard is hot:
    // pushes only append past it, and the one in-place sort runs at seal. So a
    // binary search over it sees the list it started on.
    size_t          len     = 0;
    bool            ordered = false;
    const uint64_t *data    = rocs_posting_sparse_view(postings, &len, &ordered);

    // An unordered list is scanned, which is right regardless and is what the
    // sealed reader does with the same bit clear.
    if (!ordered) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == ordinal) {
                return true;
            }
        }
        return false;
    }

    uint64_t lo = 0;
    uint64_t hi = (uint64_t)len;
    while (lo < hi) {
        uint64_t mid   = lo + (hi - lo) / 2;
        uint64_t value = data[mid];
        if (value == ordinal) {
            return true;
        }
        if (value < ordinal) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return false;
}

// A private copy of a sparse list's ordinals, ascending.
//
// A copy rather than a walk of the live list: callers do work per element, and
// a copy lets them sort without touching the shared list. The prefix copied
// is fixed once its length is loaded (see rocs_posting_sparse_view).
//
// Dense lists are returned as null; rocs_posting_list_collect walks their
// bitmap instead.
static uint64_t *
rocs_posting_snapshot_ordinals(n00b_store_posting_list_t *postings,
                               uint64_t                  *len_out,
                               n00b_allocator_t          *allocator)
{
    *len_out = 0;
    if (postings == nullptr || postings->kind != N00B_STORE_POSTINGS_SPARSE
        || postings->ordinals == nullptr) {
        return nullptr;
    }

    size_t          len     = 0;
    bool            ordered = false;
    const uint64_t *data    = rocs_posting_sparse_view(postings, &len, &ordered);
    uint64_t       *copy    = nullptr;
    if (len != 0) {
        copy = n00b_alloc_array_with_opts(
            uint64_t,
            len,
            &(n00b_alloc_opts_t){.allocator = allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_NONE});
        memcpy(copy, data, len * sizeof(uint64_t));
    }

    // Callers walk this ascending. Sorted here, on the private copy, rather
    // than by ordering the shared list, which only the sealer may renumber.
    if (!ordered && len > 1) {
        qsort(copy, len, sizeof(uint64_t), rocs_u64_compare);
    }

    *len_out = (uint64_t)len;
    return copy;
}

// Every ordinal of a hot list, ascending and unique, in one private array.
//
// A sparse list is copied (rocs_posting_snapshot_ordinals). A dense one has
// its words copied first, because the writer may set bits while this runs and
// the two passes below must agree: one popcount pass sizes the output, and
// one walk fills it, so p postings in a bitmap of w words cost 2w + p. Sizing
// from the bits keeps the answer independent of the list's separate count.
static uint64_t *
rocs_posting_list_collect(n00b_store_posting_list_t *postings,
                          uint64_t                  *len_out,
                          n00b_allocator_t          *allocator)
{
    *len_out = 0;
    if (postings == nullptr) {
        return nullptr;
    }

    if (postings->kind == N00B_STORE_POSTINGS_SPARSE) {
        uint64_t  len  = 0;
        uint64_t *copy = rocs_posting_snapshot_ordinals(postings,
                                                        &len,
                                                        allocator);
        ROCS_POSTING_STEPS(len);
        // Sorted by the snapshot; a term pushed twice for one record leaves a
        // repeat, which this drops.
        *len_out = rocs_ordinals_normalize(copy, len, true);
        return copy;
    }

    n00b_flagset_t *flags = postings->flags;
    if (flags == nullptr) {
        return nullptr;
    }

    const uint64_t *live    = nullptr;
    uint64_t        wordlen = 0;
    n00b_flagset_view(flags, &live, &wordlen);
    rocs_posting_read_point(postings);
    if (wordlen == 0) {
        ROCS_POSTING_STEPS(0);
        return nullptr;
    }

    uint64_t *words = rocs_ordinals_alloc(wordlen, allocator);
    uint64_t  count = 0;
    for (uint64_t word_ix = 0; word_ix < wordlen; word_ix++) {
        words[word_ix] = __atomic_load_n(&live[word_ix], __ATOMIC_RELAXED);
        count += (uint64_t)__builtin_popcountll(words[word_ix]);
    }
    ROCS_POSTING_STEPS(wordlen);

    uint64_t *out     = rocs_ordinals_alloc(count, allocator);
    uint64_t  written = 0;
    for (uint64_t word_ix = 0; word_ix < wordlen && written < count;
         word_ix++) {
        uint64_t word = words[word_ix];
        while (word != 0) {
            out[written++] = (word_ix << 6) + (uint64_t)__builtin_ctzll(word);
            word &= word - 1;
        }
        ROCS_POSTING_STEPS(1);
    }
    n00b_free(words);

    ROCS_POSTING_STEPS(written);
    *len_out = written;
    return out;
}

// Keep the ordinals every other list also carries, in place. Lists are tested
// in the order given, which callers make smallest first, so the candidate set
// shrinks as early as it can.
static uint64_t
rocs_hot_filter_in_place(uint64_t                   *ordinals,
                         uint64_t                    len,
                         n00b_store_posting_list_t **lists,
                         size_t                      list_count)
{
    for (size_t j = 1; j < list_count && len != 0; j++) {
        uint64_t kept = 0;
        for (uint64_t i = 0; i < len; i++) {
            if (rocs_posting_list_contains_ordinal(lists[j], ordinals[i])) {
                ordinals[kept++] = ordinals[i];
            }
        }
        ROCS_POSTING_STEPS(len);
        len = kept;
    }
    return len;
}

static n00b_result_t(uint64_t)
rocs_mapped_filter_in_place(uint64_t                       *ordinals,
                            uint64_t                        len,
                            n00b_store_map_posting_list_t **lists,
                            size_t                          list_count)
{
    for (size_t j = 1; j < list_count && len != 0; j++) {
        uint64_t kept = 0;
        for (uint64_t i = 0; i < len; i++) {
            auto has_r = n00b_store_map_posting_list_contains(lists[j],
                                                              ordinals[i]);
            if (n00b_result_is_err(has_r)) {
                return n00b_result_err(
                    uint64_t,
                    rocs_index_map_err(n00b_result_get_err(has_r)));
            }
            if (n00b_result_get(has_r)) {
                ordinals[kept++] = ordinals[i];
            }
        }
        len = kept;
    }
    return n00b_result_ok(uint64_t, len);
}

// Every ordinal of a sealed list, ascending and unique, in one private array.
static n00b_result_t(uint64_t *)
rocs_mapped_posting_list_collect(n00b_store_map_posting_list_t *list,
                                 uint64_t                      *len_out,
                                 n00b_allocator_t              *allocator)
{
    *len_out = 0;
    auto len_r = n00b_store_map_posting_list_len(list);
    if (n00b_result_is_err(len_r)) {
        return n00b_result_err(uint64_t *,
                               rocs_index_map_err(n00b_result_get_err(len_r)));
    }

    uint64_t  len = n00b_result_get(len_r);
    uint64_t *out = rocs_ordinals_alloc(len, allocator);
    auto      asc_r = n00b_store_map_posting_list_copy_ordinals(list, out, len);
    if (n00b_result_is_err(asc_r)) {
        return n00b_result_err(uint64_t *,
                               rocs_index_map_err(n00b_result_get_err(asc_r)));
    }

    *len_out = rocs_ordinals_normalize(out, len, n00b_result_get(asc_r));
    return n00b_result_ok(uint64_t *, out);
}

// Order a lookup's term lists smallest first. Intersecting starts from the
// first, so this is what makes a query pairing a rare term with a common one
// cost the rare term's postings rather than the common one's. Term counts are
// small, so an insertion sort.
static void
rocs_hot_lists_by_size(n00b_store_posting_list_t **lists, size_t len)
{
    for (size_t i = 1; i < len; i++) {
        n00b_store_posting_list_t *item  = lists[i];
        uint64_t                   count = n00b_atomic_load(&item->count);
        size_t                     j     = i;
        while (j > 0 && n00b_atomic_load(&lists[j - 1]->count) > count) {
            lists[j] = lists[j - 1];
            j--;
        }
        lists[j] = item;
    }
}

static void
rocs_mapped_lists_by_size(n00b_store_map_posting_list_t **lists,
                          uint64_t                       *counts,
                          size_t                          len)
{
    for (size_t i = 1; i < len; i++) {
        n00b_store_map_posting_list_t *item  = lists[i];
        uint64_t                       count = counts[i];
        size_t                         j     = i;
        while (j > 0 && counts[j - 1] > count) {
            lists[j]  = lists[j - 1];
            counts[j] = counts[j - 1];
            j--;
        }
        lists[j]  = item;
        counts[j] = count;
    }
}

// Hand a finished ordinal array to a postings object. The largest ordinal is
// the last, so one comparison bounds them all by the shard's records.
static n00b_result_t(n00b_store_postings_t *)
rocs_postings_from_ordinals(uint64_t               *ordinals,
                            uint64_t                len,
                            uint64_t                record_count,
                            n00b_store_shard_t     *hot_shard,
                            n00b_store_map_shard_t *mapped_shard,
                            uint64_t                shard_id,
                            uint64_t                generation,
                            n00b_allocator_t       *allocator)
{
    if (len != 0 && ordinals[len - 1] >= record_count) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_postings_t *postings =
        rocs_postings_new(shard_id, generation, .allocator = allocator);
    postings->ordinals     = len == 0 ? nullptr : ordinals;
    postings->len          = len;
    postings->hot_shard    = hot_shard;
    postings->mapped_shard = mapped_shard;
    return n00b_result_ok(n00b_store_postings_t *, postings);
}

static n00b_result_t(n00b_option_t(n00b_store_map_dict_t *))
rocs_mapped_column_find(n00b_store_map_dict_t *columns,
                        n00b_string_t         *field)
{
    if (columns == nullptr || field == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_map_dict_t *),
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto field_entry_r =
        n00b_store_map_dict_find_hv(columns, n00b_string_hash(field));
    if (n00b_result_is_err(field_entry_r)) {
        return n00b_result_err(
            n00b_option_t(n00b_store_map_dict_t *),
            rocs_index_map_err(n00b_result_get_err(field_entry_r)));
    }

    n00b_option_t(n00b_store_map_dict_entry_t *) field_entry_opt =
        n00b_result_get(field_entry_r);
    if (!n00b_option_is_set(field_entry_opt)) {
        return n00b_result_ok(n00b_option_t(n00b_store_map_dict_t *),
                              n00b_option_none(n00b_store_map_dict_t *));
    }

    n00b_store_map_dict_entry_t *field_entry =
        n00b_option_get(field_entry_opt);
    auto field_eq_r =
        n00b_store_map_slot_string_eq(field_entry->key, field);
    if (n00b_result_is_err(field_eq_r)) {
        return n00b_result_err(
            n00b_option_t(n00b_store_map_dict_t *),
            rocs_index_map_err(n00b_result_get_err(field_eq_r)));
    }
    if (!n00b_result_get(field_eq_r)) {
        return n00b_result_ok(n00b_option_t(n00b_store_map_dict_t *),
                              n00b_option_none(n00b_store_map_dict_t *));
    }

    auto column_r = n00b_store_map_slot_column(field_entry->value);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(n00b_option_t(n00b_store_map_dict_t *),
                               rocs_index_map_err(n00b_result_get_err(column_r)));
    }

    return n00b_result_ok(n00b_option_t(n00b_store_map_dict_t *),
                          n00b_option_set(n00b_store_map_dict_t *,
                                          n00b_result_get(column_r)));
}

static n00b_result_t(n00b_option_t(n00b_store_map_posting_list_t *))
rocs_mapped_column_postings_find(n00b_store_map_dict_t *column,
                                 n00b_uint128_t         key)
{
    if (column == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_map_posting_list_t *),
                               N00B_STORE_INDEX_ERR_ARG);
    }

    n00b_uint128_t bucket_hv = rocs_column_bucket_hash(key);
    auto           entry_r   = n00b_store_map_dict_find_hv(column, bucket_hv);
    if (n00b_result_is_err(entry_r)) {
        return n00b_result_err(n00b_option_t(n00b_store_map_posting_list_t *),
                               rocs_index_map_err(n00b_result_get_err(entry_r)));
    }

    n00b_option_t(n00b_store_map_dict_entry_t *) entry_opt =
    n00b_result_get(entry_r);
    if (!n00b_option_is_set(entry_opt)) {
        return n00b_result_ok(n00b_option_t(n00b_store_map_posting_list_t *),
                              n00b_option_none(n00b_store_map_posting_list_t *));
    }

    n00b_store_map_dict_entry_t *entry = n00b_option_get(entry_opt);
    auto                         key_slot_r =
        n00b_store_map_slot_u128(entry->key);
    if (n00b_result_is_err(key_slot_r)) {
        return n00b_result_err(
            n00b_option_t(n00b_store_map_posting_list_t *),
            rocs_index_map_err(n00b_result_get_err(key_slot_r)));
    }
    if (n00b_result_get(key_slot_r) != key) {
        return n00b_result_ok(n00b_option_t(n00b_store_map_posting_list_t *),
                              n00b_option_none(n00b_store_map_posting_list_t *));
    }

    auto list_r = n00b_store_map_slot_posting_list(entry->value);
    if (n00b_result_is_err(list_r)) {
        return n00b_result_err(n00b_option_t(n00b_store_map_posting_list_t *),
                               rocs_index_map_err(n00b_result_get_err(list_r)));
    }

    return n00b_result_ok(n00b_option_t(n00b_store_map_posting_list_t *),
                          n00b_option_set(n00b_store_map_posting_list_t *,
                                          n00b_result_get(list_r)));
}

n00b_string_t *
n00b_store_index_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_INDEX_OK:           return r"OK";
    case N00B_STORE_INDEX_ERR_ARG:      return r"ARG";
    case N00B_STORE_INDEX_ERR_STATE:    return r"STATE";
    case N00B_STORE_INDEX_ERR_KIND:     return r"KIND";
    case N00B_STORE_INDEX_ERR_UNREADY:  return r"UNREADY";
    case N00B_STORE_INDEX_ERR_INTERNAL: return r"INTERNAL";
    }
    return r"UNKNOWN";
}

n00b_result_t(n00b_store_index_t *)
n00b_store_index_new(n00b_string_t          *field,
                     n00b_store_index_kind_t kind) _kargs
{
    uint8_t                    ngram_n   = N00B_STORE_NGRAM_DEFAULT_N;
    n00b_store_postings_kind_t postings  = N00B_STORE_POSTINGS_SPARSE;
    n00b_allocator_t          *allocator = nullptr;
}
{
    if (!rocs_json_field_name_valid(field)) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_ARG);
    }
    if (!rocs_index_kind_known(kind)) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_KIND);
    }
    if (kind == N00B_STORE_INDEX_NGRAM) {
        if (!rocs_index_ngram_n_valid(ngram_n)) {
            return n00b_result_err(n00b_store_index_t *,
                                   N00B_STORE_INDEX_ERR_ARG);
        }
    }
    else if (ngram_n != N00B_STORE_NGRAM_DEFAULT_N) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_ARG);
    }
    if (!rocs_postings_kind_valid(postings)) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_ARG);
    }
    if ((kind == N00B_STORE_INDEX_NONE
         || kind == N00B_STORE_INDEX_NUMERIC
         || kind == N00B_STORE_INDEX_BOOL
         || kind == N00B_STORE_INDEX_VECTOR)
        && postings != N00B_STORE_POSTINGS_SPARSE) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    n00b_store_index_t *index = n00b_alloc_with_opts(
        n00b_store_index_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    index->field    = field;
    index->kind     = kind;
    index->postings = postings;
    index->ngram_n  = ngram_n;
    index->catch_all_fields = nullptr;
    index->catch_all        = false;

    return n00b_result_ok(n00b_store_index_t *, index);
}

n00b_result_t(n00b_store_index_t *)
n00b_store_index_new_catch_all(n00b_store_index_field_list_t *fields) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (fields == nullptr || n00b_list_len(*fields) == 0) {
        return n00b_result_err(n00b_store_index_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    n00b_store_index_t *index = n00b_alloc_with_opts(
        n00b_store_index_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    index->field            = nullptr;
    index->catch_all_fields = fields;
    index->kind             = N00B_STORE_INDEX_FULLTEXT;
    index->postings         = N00B_STORE_POSTINGS_SPARSE;
    index->ngram_n          = N00B_STORE_NGRAM_DEFAULT_N;
    index->catch_all        = true;

    return n00b_result_ok(n00b_store_index_t *, index);
}

n00b_result_t(n00b_store_index_field_list_t *)
n00b_store_index_catch_all_fields(n00b_store_index_t *index)
{
    if (index == nullptr || !index->catch_all) {
        return n00b_result_err(n00b_store_index_field_list_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    return n00b_result_ok(n00b_store_index_field_list_t *,
                          index->catch_all_fields);
}

n00b_result_t(bool)
n00b_store_index_is_catch_all(n00b_store_index_t *index)
{
    if (index == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    return n00b_result_ok(bool, index->catch_all);
}

n00b_result_t(n00b_store_index_kind_t)
n00b_store_index_kind(n00b_store_index_t *index)
{
    if (index == nullptr) {
        return n00b_result_err(n00b_store_index_kind_t, N00B_STORE_INDEX_ERR_ARG);
    }

    return n00b_result_ok(n00b_store_index_kind_t, index->kind);
}

n00b_result_t(uint8_t)
n00b_store_index_ngram_n(n00b_store_index_t *index)
{
    if (index == nullptr) {
        return n00b_result_err(uint8_t, N00B_STORE_INDEX_ERR_ARG);
    }
    if (index->kind != N00B_STORE_INDEX_NGRAM) {
        return n00b_result_err(uint8_t, N00B_STORE_INDEX_ERR_KIND);
    }
    if (!rocs_index_ngram_n_valid(index->ngram_n)) {
        return n00b_result_err(uint8_t, N00B_STORE_INDEX_ERR_STATE);
    }

    return n00b_result_ok(uint8_t, index->ngram_n);
}

n00b_result_t(n00b_store_postings_kind_t)
n00b_store_index_postings_kind(n00b_store_index_t *index)
{
    if (index == nullptr) {
        return n00b_result_err(n00b_store_postings_kind_t,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    return n00b_result_ok(n00b_store_postings_kind_t, index->postings);
}

n00b_result_t(n00b_string_t *)
n00b_store_index_field(n00b_store_index_t *index)
{
    if (index == nullptr || index->field == nullptr || index->catch_all) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    return n00b_result_ok(n00b_string_t *, index->field);
}

n00b_store_advert_t
n00b_store_index_advertise(n00b_store_index_t *index,
                           n00b_string_t      *field,
                           int64_t             op)
{
    if (index == nullptr || field == nullptr || index->field == nullptr
        || index->catch_all) {
        return (n00b_store_advert_t){
            .accelerates = false,
            .kind        = N00B_STORE_INDEX_NONE,
        };
    }

    bool same_field = n00b_unicode_str_eq(index->field, field);
    bool term_eq    = same_field
                   && index->kind == N00B_STORE_INDEX_TERM
                   && (op == (int64_t)N00B_STORE_INDEX_OP_UNSPECIFIED
                       || op == (int64_t)N00B_STORE_INDEX_OP_EQ);
    bool fulltext_contains = same_field
                          && index->kind == N00B_STORE_INDEX_FULLTEXT
                          && op == (int64_t)N00B_STORE_INDEX_OP_CONTAINS;
    bool ngram_text = same_field
                   && index->kind == N00B_STORE_INDEX_NGRAM
                   && op == (int64_t)N00B_STORE_INDEX_OP_PREFIX;
    bool accelerates = term_eq || fulltext_contains || ngram_text;
    return (n00b_store_advert_t){
        .accelerates      = accelerates,
        .kind             = term_eq ? N00B_STORE_INDEX_TERM
                                    : (fulltext_contains
                                           ? N00B_STORE_INDEX_FULLTEXT
                                           : (ngram_text
                                                  ? N00B_STORE_INDEX_NGRAM
                                                  : N00B_STORE_INDEX_NONE)),
        .selectivity_hint = fulltext_contains ? 0.05
                           : (term_eq ? 0.10
                                      : (ngram_text ? 0.20 : 1.0)),
    };
}

static void **
rocs_pointer_array(size_t len, n00b_allocator_t *allocator)
{
    return n00b_alloc_array_with_opts(
        void *,
        len == 0 ? 1 : len,
        &(n00b_alloc_opts_t){.allocator = allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_ALL});
}

// The catch-all's posting lists for one key: one per covered field that holds
// the term. A field without a column, or without the term, contributes none.
// @p out holds one slot per covered field.
static n00b_result_t(size_t)
rocs_catch_all_hot_lists(n00b_store_index_t          *index,
                         n00b_store_shard_t          *shard,
                         n00b_uint128_t               key,
                         n00b_store_posting_list_t  **out)
{
    size_t field_count = n00b_list_len(*index->catch_all_fields);
    size_t found       = 0;
    for (size_t i = 0; i < field_count; i++) {
        n00b_string_t *field = n00b_list_get(*index->catch_all_fields, i);
        if (field == nullptr) {
            return n00b_result_err(size_t, N00B_STORE_INDEX_ERR_STATE);
        }

        bool                 found_column = false;
        n00b_store_column_t *column       = n00b_dict_get(shard->columns,
                                                          field,
                                                          &found_column);
        if (!found_column) {
            continue;
        }
        if (column == nullptr) {
            return n00b_result_err(size_t, N00B_STORE_INDEX_ERR_STATE);
        }

        auto postings_r = rocs_column_postings_find(column, key);
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(size_t, n00b_result_get_err(postings_r));
        }
        n00b_option_t(n00b_store_posting_list_t *) opt =
            n00b_result_get(postings_r);
        if (n00b_option_is_set(opt)) {
            out[found++] = n00b_option_get(opt);
        }
    }
    return n00b_result_ok(size_t, found);
}

static n00b_result_t(size_t)
rocs_catch_all_mapped_lists(n00b_store_index_t             *index,
                            n00b_store_map_dict_t          *columns,
                            n00b_uint128_t                  key,
                            n00b_store_map_posting_list_t **out)
{
    size_t field_count = n00b_list_len(*index->catch_all_fields);
    size_t found       = 0;
    for (size_t i = 0; i < field_count; i++) {
        n00b_string_t *field = n00b_list_get(*index->catch_all_fields, i);
        if (field == nullptr) {
            return n00b_result_err(size_t, N00B_STORE_INDEX_ERR_STATE);
        }

        auto column_r = rocs_mapped_column_find(columns, field);
        if (n00b_result_is_err(column_r)) {
            return n00b_result_err(size_t, n00b_result_get_err(column_r));
        }
        n00b_option_t(n00b_store_map_dict_t *) column_opt =
            n00b_result_get(column_r);
        if (!n00b_option_is_set(column_opt)) {
            continue;
        }

        auto postings_r =
            rocs_mapped_column_postings_find(n00b_option_get(column_opt), key);
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(size_t, n00b_result_get_err(postings_r));
        }
        n00b_option_t(n00b_store_map_posting_list_t *) opt =
            n00b_result_get(postings_r);
        if (n00b_option_is_set(opt)) {
            out[found++] = n00b_option_get(opt);
        }
    }
    return n00b_result_ok(size_t, found);
}

// Concatenate per-field ordinal arrays into one ascending, unique array. One
// field's array is already that, and is returned as it is.
static uint64_t *
rocs_ordinals_union(uint64_t        **parts,
                    uint64_t         *lens,
                    size_t            count,
                    uint64_t         *len_out,
                    n00b_allocator_t *allocator)
{
    *len_out = 0;
    if (count == 0) {
        return nullptr;
    }
    if (count == 1) {
        *len_out = lens[0];
        return parts[0];
    }

    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += lens[i];
    }
    uint64_t *out = rocs_ordinals_alloc(total, allocator);
    uint64_t  at  = 0;
    for (size_t i = 0; i < count; i++) {
        if (lens[i] != 0) {
            memcpy(out + at, parts[i], (size_t)lens[i] * sizeof(uint64_t));
            at += lens[i];
        }
    }
    // A record matching in several covered fields appears once per field.
    *len_out = rocs_ordinals_normalize(out, total, false);
    return out;
}

// The catch-all key for a lookup: the value's first full-text term, whatever
// field it is found in.
static n00b_result_t(n00b_uint128_t)
rocs_catch_all_key(n00b_store_normalized_list_t *terms,
                   n00b_allocator_t             *allocator)
{
    return rocs_term_key(N00B_STORE_INDEX_FULLTEXT,
                         n00b_list_get(*terms, 0),
                         .allocator = allocator);
}

static n00b_result_t(n00b_store_postings_t *)
rocs_index_lookup_catch_all_terms(n00b_store_index_t            *index,
                                  n00b_store_shard_t            *shard,
                                  n00b_store_normalized_list_t  *terms) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (index == nullptr || shard == nullptr || terms == nullptr
        || index->catch_all_fields == nullptr) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    uint64_t shard_id   = shard->shard_id;
    uint64_t generation = shard->seal_ts;
    if (n00b_list_len(*terms) == 0) {
        return rocs_empty_postings(shard_id, generation, .allocator = allocator);
    }

    auto key_r = rocs_catch_all_key(terms, allocator);
    if (n00b_result_is_err(key_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(key_r));
    }

    size_t                      field_count =
        n00b_list_len(*index->catch_all_fields);
    n00b_store_posting_list_t **lists       = (n00b_store_posting_list_t **)
        rocs_pointer_array(field_count, allocator);
    auto found_r = rocs_catch_all_hot_lists(index,
                                            shard,
                                            n00b_result_get(key_r),
                                            lists);
    if (n00b_result_is_err(found_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(found_r));
    }
    size_t found = n00b_result_get(found_r);

    uint64_t **parts = (uint64_t **)rocs_pointer_array(found, allocator);
    uint64_t  *lens  = rocs_ordinals_alloc(found == 0 ? 1 : found, allocator);
    for (size_t i = 0; i < found; i++) {
        parts[i] = rocs_posting_list_collect(lists[i], &lens[i], allocator);
    }

    uint64_t  len      = 0;
    uint64_t *ordinals = rocs_ordinals_union(parts, lens, found, &len, allocator);
    return rocs_postings_from_ordinals(ordinals,
                                       len,
                                       n00b_store_shard_records_len(shard),
                                       shard,
                                       nullptr,
                                       shard_id,
                                       generation,
                                       allocator);
}

static n00b_result_t(n00b_store_normalized_list_t *)
rocs_index_hot_terms(n00b_store_index_t *index,
                     n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (index == nullptr || value == nullptr) {
        return n00b_result_err(n00b_store_normalized_list_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    if (index->kind == N00B_STORE_INDEX_TERM) {
        auto terms_r = n00b_store_normalize_json(value,
                                                 .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(
                n00b_store_normalized_list_t *,
                rocs_index_norm_err(n00b_result_get_err(terms_r)));
        }
        return terms_r;
    }

    if (index->kind == N00B_STORE_INDEX_FULLTEXT) {
        auto terms_r = n00b_store_normalize_text_tokens(
            value,
            .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(
                n00b_store_normalized_list_t *,
                rocs_index_norm_err(n00b_result_get_err(terms_r)));
        }
        return terms_r;
    }

    if (index->kind == N00B_STORE_INDEX_NGRAM) {
        auto terms_r = n00b_store_normalize_text_ngrams(
            value,
            .ngram_n   = index->ngram_n,
            .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(
                n00b_store_normalized_list_t *,
                rocs_index_norm_err(n00b_result_get_err(terms_r)));
        }
        return terms_r;
    }

    return n00b_result_err(n00b_store_normalized_list_t *,
                           N00B_STORE_INDEX_ERR_UNREADY);
}

static n00b_result_t(n00b_store_normalized_list_t *)
rocs_index_hot_query_terms(n00b_store_index_t *index,
                           n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto terms_r = rocs_index_hot_terms(index,
                                        value,
                                        .allocator = allocator);
    if (n00b_result_is_err(terms_r)) {
        return terms_r;
    }

    n00b_store_normalized_list_t *terms = n00b_result_get(terms_r);
    if (index->kind == N00B_STORE_INDEX_FULLTEXT) {
        if (n00b_list_len(*terms) == 0) {
            return n00b_result_err(n00b_store_normalized_list_t *,
                                   N00B_STORE_INDEX_ERR_ARG);
        }
        if (n00b_list_len(*terms) == 1) {
            return n00b_result_ok(n00b_store_normalized_list_t *, terms);
        }

        // Indexing a multi-token value stores the whole value as a term
        // beside each token. Looking the whole value up would ask for a field
        // equal to it, so a query is reduced to its tokens and the lookup
        // intersects them. That is the whole-token containment a record scan
        // performs, which is what makes an indexed query and an unindexed one
        // answer alike.
        auto tokens_r = n00b_store_normalize_text_tokens(
            value,
            .include_full_value = false,
            .allocator          = allocator);
        if (n00b_result_is_err(tokens_r)) {
            return n00b_result_err(
                n00b_store_normalized_list_t *,
                rocs_index_norm_err(n00b_result_get_err(tokens_r)));
        }
        n00b_store_normalized_list_t *tokens = n00b_result_get(tokens_r);
        if (n00b_list_len(*tokens) == 0) {
            return n00b_result_err(n00b_store_normalized_list_t *,
                                   N00B_STORE_INDEX_ERR_ARG);
        }
        return n00b_result_ok(n00b_store_normalized_list_t *, tokens);
    }
    if (index->kind == N00B_STORE_INDEX_NGRAM
        && n00b_list_len(*terms) == 0) {
        return n00b_result_err(n00b_store_normalized_list_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    return n00b_result_ok(n00b_store_normalized_list_t *, terms);
}

static bool
rocs_index_unique_postings(n00b_store_index_kind_t kind)
{
    return kind == N00B_STORE_INDEX_FULLTEXT
        || kind == N00B_STORE_INDEX_NGRAM;
}

// Grow a sparse list for its one writer. The copy is published before any
// length that reaches past the old array, and the old array is left where it
// is: a reader that loaded it may still be copying out of it, and freeing it
// into the shard's pool would hand those words to the next allocation, which
// zeroes and reuses them. It goes back with the pool when the shard retires.
static void
rocs_posting_ordinals_grow(n00b_store_posting_ordinal_list_t *list,
                           size_t                             need)
{
    size_t    cap   = n00b_align_closest_pow2_ceil(n00b_max(need, list->cap * 2));
    uint64_t *grown = n00b_alloc_array_with_opts(
        uint64_t,
        cap,
        &(n00b_alloc_opts_t){.allocator = list->allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});
    if (list->len != 0) {
        memcpy(grown, list->data, list->len * sizeof(uint64_t));
    }
    __atomic_store_n(&list->data, grown, __ATOMIC_RELEASE);
    list->cap = cap;
}

n00b_result_t(bool)
rocs_posting_list_push(n00b_store_posting_list_t *postings,
                       uint64_t                   ordinal,
                       bool                       unique)
{
    if (postings == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
        if (postings->flags == nullptr) {
            return n00b_result_err(bool, N00B_STORE_INDEX_ERR_STATE);
        }
        // The count is kept rather than popcounted because a count read is
        // O(1) that way, where popcounting the bitmap is a pass over
        // record_count/64 words on every df read. A reader can see the bit
        // before the count that describes it; the count is an estimate.
        bool fresh = n00b_flagset_set_owned(postings->flags, ordinal);
        if (fresh) {
            atomic_store_explicit(&postings->count,
                                  atomic_load_explicit(&postings->count,
                                                       memory_order_relaxed)
                                      + 1,
                                  memory_order_release);
        }
        return n00b_result_ok(bool, fresh);
    }
    if (postings->ordinals == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_posting_ordinal_list_t *list = postings->ordinals;
    size_t len = __atomic_load_n(&list->len, __ATOMIC_RELAXED);

    // Ascending order is a property readers want, not a promise asked of
    // callers, so it is established when a reader needs it rather than on
    // every push. n00b_store_index_add is public and does not constrain
    // arrival order; placing each ordinal on arrival would cost a descending
    // caller a shift per element, and quadratic time overall.
    //
    // Appending instead is O(1) whatever the order, and an append that lands
    // below the tail clears N00B_STORE_POSTINGS_ORDERED before the length
    // that exposes it is published. That bit is what readers consult before
    // binary-searching; a list that loses it stays unordered and is scanned
    // until sealing sorts it, which costs speed on a hot shard and nothing on
    // a sealed image.
    //
    // The tail check below still catches the duplicate that actually occurs: a
    // full-text or n-gram field emitting the same term twice pushes the same
    // ordinal twice in a row. A duplicate arriving non-consecutively survives
    // until the sort, which drops it.
    if (len != 0) {
        uint64_t last = list->data[len - 1];

        if (unique && ordinal == last) {
            return n00b_result_ok(bool, false);
        }
        if (ordinal < last) {
            n00b_atomic_and(&postings->reserved, ~N00B_STORE_POSTINGS_ORDERED);
        }
    }

    if (len == list->cap) {
        rocs_posting_ordinals_grow(list, len + 1);
    }
    list->data[len] = ordinal;
    __atomic_store_n(&list->len, len + 1, __ATOMIC_RELEASE);
    atomic_store_explicit(&postings->count,
                          atomic_load_explicit(&postings->count,
                                               memory_order_relaxed)
                              + 1,
                          memory_order_release);
    return n00b_result_ok(bool, true);
}

static n00b_result_t(uint64_t)
rocs_index_add_terms(n00b_store_index_t            *index,
                     n00b_store_shard_t            *shard,
                     uint64_t                       record_ordinal,
                     n00b_store_normalized_list_t  *terms) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (index == nullptr || shard == nullptr || terms == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }

    size_t len = n00b_list_len(*terms);
    if (len == 0) {
        return n00b_result_ok(uint64_t, 0);
    }

    auto column_r = rocs_column_get_or_create(shard, index->field);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(column_r));
    }
    n00b_store_column_t *column = n00b_result_get(column_r);

    uint64_t added = 0;
    for (size_t i = 0; i < len; i++) {
        n00b_store_normalized_t *term = n00b_list_get(*terms, i);
        auto                    key_r = rocs_term_key(index->kind,
                                                      term,
                                                      .allocator = allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(key_r));
        }

        auto postings_r = rocs_column_postings_get_or_create(
            column,
            n00b_result_get(key_r),
            index->postings);
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(postings_r));
        }

        auto push_r = rocs_posting_list_push(
            n00b_result_get(postings_r),
            record_ordinal,
            rocs_index_unique_postings(index->kind));
        if (n00b_result_is_err(push_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(push_r));
        }
        if (n00b_result_get(push_r)) {
            added++;
        }
    }

    return n00b_result_ok(uint64_t, added);
}

static n00b_result_t(n00b_uint128_t)
rocs_lookup_key_at(n00b_store_index_t           *index,
                   n00b_store_normalized_list_t *terms,
                   n00b_store_index_keys_t      *keys,
                   size_t                        i,
                   n00b_allocator_t             *allocator)
{
    if (keys != nullptr) {
        return n00b_result_ok(n00b_uint128_t,
                              n00b_store_index_keys_at(keys, (uint64_t)i));
    }
    return rocs_term_key(index->kind,
                         n00b_list_get(*terms, i),
                         .allocator = allocator);
}

static n00b_result_t(n00b_store_postings_t *)
rocs_index_lookup_terms(n00b_store_index_t            *index,
                        n00b_store_shard_t            *shard,
                        n00b_store_column_t           *column,
                        n00b_store_normalized_list_t  *terms) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    if (index == nullptr || shard == nullptr
        || (terms == nullptr && keys == nullptr)) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    uint64_t shard_id   = shard->shard_id;
    uint64_t generation = shard->seal_ts;
    // The walk is the common path, so it wants the resolved keys as much as
    // the posting-count read does: without them a fan-out normalizes and
    // hashes the same value once per shard it visits.
    size_t len = keys != nullptr ? (size_t)n00b_store_index_keys_count(keys)
                                 : n00b_list_len(*terms);
    if (len == 0) {
        return rocs_empty_postings(shard_id, generation, .allocator = allocator);
    }
    if (column == nullptr) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_posting_list_t **lists = (n00b_store_posting_list_t **)
        rocs_pointer_array(len, allocator);
    for (size_t i = 0; i < len; i++) {
        auto key_r = rocs_lookup_key_at(index, terms, keys, i, allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(key_r));
        }

        auto postings_r = rocs_column_postings_find(column,
                                                    n00b_result_get(key_r));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_posting_list_t *) current_opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(current_opt)) {
            return rocs_empty_postings(shard_id, generation, .allocator = allocator);
        }
        lists[i] = n00b_option_get(current_opt);
    }

    // Enumerate the smallest list and test the rest for each of its ordinals.
    // The counts are read without a lock and may be a push behind; they only
    // choose an order, which the answer does not depend on.
    rocs_hot_lists_by_size(lists, len);

    uint64_t  count    = 0;
    uint64_t *ordinals = rocs_posting_list_collect(lists[0], &count, allocator);
    count              = rocs_hot_filter_in_place(ordinals, count, lists, len);

    return rocs_postings_from_ordinals(ordinals,
                                       count,
                                       n00b_store_shard_records_len(shard),
                                       shard,
                                       nullptr,
                                       shard_id,
                                       generation,
                                       allocator);
}

static n00b_result_t(n00b_store_postings_t *)
rocs_index_lookup_mapped_terms(n00b_store_index_t           *index,
                               n00b_store_map_shard_t      *shard,
                               uint64_t                     record_count,
                               n00b_store_map_dict_t       *column,
                               n00b_store_normalized_list_t *terms,
                               uint64_t                     shard_id,
                               uint64_t                     generation) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    if (index == nullptr || shard == nullptr || column == nullptr
        || (terms == nullptr && keys == nullptr)) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    size_t len = keys != nullptr ? (size_t)n00b_store_index_keys_count(keys)
                                 : n00b_list_len(*terms);
    if (len == 0) {
        return rocs_empty_postings(shard_id, generation, .allocator = allocator);
    }

    n00b_store_map_posting_list_t **lists  = (n00b_store_map_posting_list_t **)
        rocs_pointer_array(len, allocator);
    uint64_t                       *counts = rocs_ordinals_alloc(len, allocator);
    for (size_t i = 0; i < len; i++) {
        auto key_r = rocs_lookup_key_at(index, terms, keys, i, allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(key_r));
        }

        auto postings_r = rocs_mapped_column_postings_find(
            column,
            n00b_result_get(key_r));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_map_posting_list_t *) current_opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(current_opt)) {
            return rocs_empty_postings(shard_id, generation, .allocator = allocator);
        }
        lists[i] = n00b_option_get(current_opt);

        auto count_r = n00b_store_map_posting_list_len(lists[i]);
        if (n00b_result_is_err(count_r)) {
            return n00b_result_err(
                n00b_store_postings_t *,
                rocs_index_map_err(n00b_result_get_err(count_r)));
        }
        counts[i] = n00b_result_get(count_r);
    }

    // Smallest first, for the reason rocs_index_lookup_terms gives. Sealed
    // counts are exact, so the first list bounds the answer.
    rocs_mapped_lists_by_size(lists, counts, len);

    uint64_t count      = 0;
    auto     ordinals_r = rocs_mapped_posting_list_collect(lists[0],
                                                           &count,
                                                           allocator);
    if (n00b_result_is_err(ordinals_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(ordinals_r));
    }
    uint64_t *ordinals = n00b_result_get(ordinals_r);

    auto kept_r = rocs_mapped_filter_in_place(ordinals, count, lists, len);
    if (n00b_result_is_err(kept_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(kept_r));
    }

    return rocs_postings_from_ordinals(ordinals,
                                       n00b_result_get(kept_r),
                                       record_count,
                                       nullptr,
                                       shard,
                                       shard_id,
                                       generation,
                                       allocator);
}

static n00b_result_t(n00b_store_postings_t *)
rocs_index_lookup_mapped_catch_all_terms(
    n00b_store_index_t           *index,
    n00b_store_map_shard_t       *shard,
    uint64_t                      record_count,
    n00b_store_map_dict_t        *columns,
    n00b_store_normalized_list_t *terms,
    uint64_t                      shard_id,
    uint64_t                      generation) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (index == nullptr || shard == nullptr || columns == nullptr
        || terms == nullptr || index->catch_all_fields == nullptr) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    if (n00b_list_len(*terms) == 0) {
        return rocs_empty_postings(shard_id, generation, .allocator = allocator);
    }

    auto key_r = rocs_catch_all_key(terms, allocator);
    if (n00b_result_is_err(key_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(key_r));
    }

    size_t                          field_count =
        n00b_list_len(*index->catch_all_fields);
    n00b_store_map_posting_list_t **lists       =
        (n00b_store_map_posting_list_t **)rocs_pointer_array(field_count,
                                                             allocator);
    auto found_r = rocs_catch_all_mapped_lists(index,
                                               columns,
                                               n00b_result_get(key_r),
                                               lists);
    if (n00b_result_is_err(found_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               n00b_result_get_err(found_r));
    }
    size_t found = n00b_result_get(found_r);

    uint64_t **parts = (uint64_t **)rocs_pointer_array(found, allocator);
    uint64_t  *lens  = rocs_ordinals_alloc(found == 0 ? 1 : found, allocator);
    for (size_t i = 0; i < found; i++) {
        auto part_r = rocs_mapped_posting_list_collect(lists[i],
                                                       &lens[i],
                                                       allocator);
        if (n00b_result_is_err(part_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(part_r));
        }
        parts[i] = n00b_result_get(part_r);
    }

    uint64_t  len      = 0;
    uint64_t *ordinals = rocs_ordinals_union(parts, lens, found, &len, allocator);
    return rocs_postings_from_ordinals(ordinals,
                                       len,
                                       record_count,
                                       nullptr,
                                       shard,
                                       shard_id,
                                       generation,
                                       allocator);
}

n00b_result_t(uint64_t)
n00b_store_index_add(n00b_store_index_t *index,
                     n00b_store_shard_t *shard,
                     uint64_t            record_ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (index != nullptr && index->catch_all) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_UNREADY);
    }

    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(uint64_t, ready);
    }
    if (shard == nullptr || shard->records == nullptr || shard->columns == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_STATE);
    }

    if (record_ordinal >= n00b_store_shard_records_len(shard)) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }

    auto text_r = rocs_hot_shard_record_stored(shard, record_ordinal);
    if (n00b_result_is_err(text_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(text_r));
    }

    n00b_string_t    *text        = n00b_result_get(text_r);
    n00b_json_node_t *field_value = nullptr;
    size_t            start       = 0;
    size_t            span        = 0;

    // One field is all this reads, so the record's other values are built only
    // when the scan declines and the whole thing has to be parsed anyway.
    switch (rocs_json_scan_field_span(text->data,
                                      (size_t)text->u8_bytes,
                                      index->field,
                                      &start,
                                      &span)) {
    case ROCS_JSON_SCAN_ABSENT:
        return n00b_result_ok(uint64_t, 0);

    case ROCS_JSON_SCAN_FOUND: {
        const char *err = nullptr;

        field_value = n00b_json_parse(text->data + start,
                                      span,
                                      &err,
                                      .allocator = allocator);
        if (field_value == nullptr || err != nullptr) {
            return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_STATE);
        }
        break;
    }

    case ROCS_JSON_SCAN_UNSURE: {
        auto record_r = rocs_hot_shard_record_json(shard,
                                                   record_ordinal,
                                                   .allocator = allocator);
        if (n00b_result_is_err(record_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(record_r));
        }
        field_value = rocs_json_object_get_field(n00b_result_get(record_r),
                                                 index->field);
        break;
    }
    }

    if (field_value == nullptr) {
        return n00b_result_ok(uint64_t, 0);
    }
    if ((index->kind == N00B_STORE_INDEX_FULLTEXT
         || index->kind == N00B_STORE_INDEX_NGRAM)
        && !n00b_json_is_string(field_value)) {
        return n00b_result_ok(uint64_t, 0);
    }

    auto terms_r = rocs_index_hot_terms(index,
                                        field_value,
                                        .allocator = allocator);
    if (n00b_result_is_err(terms_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(terms_r));
    }

    rocs_index_writer_enter(shard);
    auto added_r = rocs_index_add_terms(index,
                                        shard,
                                        record_ordinal,
                                        n00b_result_get(terms_r),
                                        .allocator = allocator);
    rocs_index_writer_exit(shard);
    return added_r;
}

n00b_result_t(n00b_store_postings_t *)
n00b_store_index_lookup(n00b_store_index_t *index,
                        n00b_store_shard_t *shard,
                        n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_postings_t *, ready);
    }
    if (shard == nullptr || value == nullptr || shard->records == nullptr
        || shard->columns == nullptr) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    if (index->catch_all) {
        auto terms_r = rocs_index_hot_terms(index, value, .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(terms_r));
        }
        return rocs_index_lookup_catch_all_terms(index,
                                                 shard,
                                                 n00b_result_get(terms_r),
                                                 .allocator = allocator);
    }

    // A shard that never held the column can hold no match, so answer that
    // before paying for key normalization. Term indexes only: for full-text and
    // n-gram, normalizing the value is also what rejects a malformed query, and
    // the documented contract is that those errors surface.
    bool                 found_column = false;
    n00b_store_column_t *column       = nullptr;
    if (index->kind == N00B_STORE_INDEX_TERM) {
        column = n00b_dict_get(shard->columns, index->field, &found_column);
        if (!found_column) {
            return rocs_empty_postings(shard->shard_id,
                                       shard->seal_ts,
                                       .allocator = allocator);
        }
    }

    // Resolved keys already carry the normalized terms and their hashes, and
    // resolving them ran this same normalization, so a value that would fail
    // here failed there. Skipping it is the point of handing them over: a
    // fan-out otherwise normalizes once per shard, which for n-gram and
    // full-text is the expensive half.
    n00b_store_normalized_list_t *terms = nullptr;
    if (keys == nullptr) {
        auto terms_r = rocs_index_hot_query_terms(index,
                                                  value,
                                                  .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(terms_r));
        }
        terms = n00b_result_get(terms_r);
    }

    if (!found_column) {
        column = n00b_dict_get(shard->columns, index->field, &found_column);
        if (!found_column) {
            return rocs_empty_postings(shard->shard_id,
                                       shard->seal_ts,
                                       .allocator = allocator);
        }
    }

    return rocs_index_lookup_terms(index,
                                   shard,
                                   column,
                                   terms,
                                   .allocator = allocator,
                                   .keys      = keys);
}

n00b_result_t(n00b_store_postings_t *)
n00b_store_index_lookup_mapped(n00b_store_index_t     *index,
                               n00b_store_map_shard_t *shard,
                               n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_postings_t *, ready);
    }
    if (shard == nullptr || value == nullptr) {
        return n00b_result_err(n00b_store_postings_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_map_err(n00b_result_get_err(state_r)));
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_not_sealed(n00b_result_get(state_r)));
    }

    auto shard_id_r = n00b_store_map_shard_id(shard);
    if (n00b_result_is_err(shard_id_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_map_err(n00b_result_get_err(shard_id_r)));
    }
    auto generation_r = n00b_store_map_shard_seal_ts(shard);
    if (n00b_result_is_err(generation_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_map_err(n00b_result_get_err(generation_r)));
    }

    uint64_t shard_id   = n00b_result_get(shard_id_r);
    uint64_t generation = n00b_result_get(generation_r);

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_map_err(n00b_result_get_err(columns_r)));
    }

    // A shard that never held the column can hold no match, so answer that
    // before paying for key normalization and the record fetch. Term indexes
    // only: for full-text and n-gram, normalizing the value is also what
    // rejects a malformed query, and the documented contract is that those
    // errors surface.
    n00b_store_map_dict_t *column = nullptr;
    if (!index->catch_all && index->kind == N00B_STORE_INDEX_TERM) {
        auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                                index->field);
        if (n00b_result_is_err(column_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(column_r));
        }
        n00b_option_t(n00b_store_map_dict_t *) column_opt =
            n00b_result_get(column_r);
        if (!n00b_option_is_set(column_opt)) {
            return rocs_empty_postings(shard_id, generation, .allocator = allocator);
        }
        column = n00b_option_get(column_opt);
    }

    // Resolved keys carry the normalized terms and their hashes, and resolving
    // them ran this same normalization, so a value that would fail here failed
    // there. Skipping it is what makes handing them over worth it across a
    // fan-out. The catch-all has no resolved keys and always normalizes.
    n00b_store_normalized_list_t *terms = nullptr;
    if (keys == nullptr || index->catch_all) {
        auto terms_r = index->catch_all
                         ? rocs_index_hot_terms(index,
                                                value,
                                                .allocator = allocator)
                         : rocs_index_hot_query_terms(index,
                                                      value,
                                                      .allocator = allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(terms_r));
        }
        terms = n00b_result_get(terms_r);
    }

    auto records_r = n00b_store_map_shard_records_len(shard);
    if (n00b_result_is_err(records_r)) {
        return n00b_result_err(n00b_store_postings_t *,
                               rocs_index_map_err(n00b_result_get_err(records_r)));
    }

    if (index->catch_all) {
        return rocs_index_lookup_mapped_catch_all_terms(
            index,
            shard,
            n00b_result_get(records_r),
            n00b_result_get(columns_r),
            terms,
            shard_id,
            generation,
            .allocator = allocator);
    }

    if (column == nullptr) {
        auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                                index->field);
        if (n00b_result_is_err(column_r)) {
            return n00b_result_err(n00b_store_postings_t *,
                                   n00b_result_get_err(column_r));
        }
        n00b_option_t(n00b_store_map_dict_t *) column_opt =
            n00b_result_get(column_r);
        if (!n00b_option_is_set(column_opt)) {
            return rocs_empty_postings(shard_id, generation, .allocator = allocator);
        }
        column = n00b_option_get(column_opt);
    }

    return rocs_index_lookup_mapped_terms(index,
                                          shard,
                                          n00b_result_get(records_r),
                                          column,
                                          terms,
                                          shard_id,
                                          generation,
                                          .allocator = allocator,
                                          .keys      = keys);
}

n00b_result_t(bool)
n00b_store_index_declare(n00b_store_index_t *index, n00b_store_shard_t *shard)
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(bool, ready);
    }
    if (shard == nullptr || shard->columns == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_STATE);
    }
    // The catch-all is virtual: it has no physical column of its own, and
    // n00b_store_index_present_mapped already reports it present everywhere.
    if (index->catch_all) {
        return n00b_result_ok(bool, false);
    }

    bool found = false;
    (void)n00b_dict_get(shard->columns, index->field, &found);
    if (found) {
        return n00b_result_ok(bool, false);
    }

    auto column_r = rocs_column_get_or_create(shard, index->field);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(bool, n00b_result_get_err(column_r));
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_store_index_present_mapped(n00b_store_index_t     *index,
                                n00b_store_map_shard_t *shard)
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(bool, ready);
    }
    if (shard == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    if (index->catch_all) {
        return n00b_result_ok(bool, true);
    }

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return n00b_result_err(
            bool,
            rocs_index_map_err(n00b_result_get_err(columns_r)));
    }
    auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                            index->field);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(bool, n00b_result_get_err(column_r));
    }
    return n00b_result_ok(bool,
                          n00b_option_is_set(n00b_result_get(column_r)));
}

// The hot half of the same question. A count of zero from a shard with no
// column for the field means nobody indexed it, not that nothing matches it,
// and only the caller that can tell those apart may act on the zero.
n00b_result_t(bool)
n00b_store_index_present_hot(n00b_store_index_t *index,
                             n00b_store_shard_t *shard)
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(bool, ready);
    }
    if (shard == nullptr || shard->columns == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    if (index->catch_all) {
        return n00b_result_ok(bool, true);
    }

    bool                 found  = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns,
                                                index->field,
                                                &found);
    return n00b_result_ok(bool, found && column != nullptr);
}

// The shard-independent half of a lookup, resolved once.
//
// Finding a posting list means normalizing the query value into terms and
// hashing each into a column key. Neither step touches a shard: both depend
// only on the descriptor's kind and the value being looked up. Left where they
// were, they ran again for every shard a query visited and again for every
// caller that asked the same node a question, which on a plan with many
// indexed leaves cost more than the posting counts it was fetching.
//
// Resolved here, a posting-count read is a dict probe and a field load.
struct n00b_store_index_keys_t {
    rocs_index_key_list_t *keys;
    // A value that normalizes to no terms matches nothing, which is different
    // from a value whose terms are simply absent from this shard.
    bool                   matches_nothing;
};

static n00b_result_t(n00b_store_index_keys_t *)
rocs_index_keys_for(n00b_store_index_t      *index,
                    n00b_json_node_t        *value,
                    n00b_store_index_keys_t *keys,
                    n00b_allocator_t        *allocator);

n00b_result_t(n00b_store_index_keys_t *)
n00b_store_index_keys_new(n00b_store_index_t *index,
                          n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_index_keys_t *, ready);
    }
    if (value == nullptr || index->catch_all) {
        return n00b_result_err(n00b_store_index_keys_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto terms_r = rocs_index_hot_query_terms(index,
                                              value,
                                              .allocator = allocator);
    if (n00b_result_is_err(terms_r)) {
        return n00b_result_err(n00b_store_index_keys_t *,
                               n00b_result_get_err(terms_r));
    }

    n00b_store_index_keys_t *out = n00b_alloc_with_opts(
        n00b_store_index_keys_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    out->keys = n00b_alloc_with_opts(
        rocs_index_key_list_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    // Private, so no rwlock is installed. A locked list would put the lock in
    // the collected heap, and this list is declared N00B_GC_SCAN_KIND_NONE, so
    // a collect traces none of its words and the lock field would dangle. No
    // lock is needed regardless: the key set is filled once here and only ever
    // read afterwards.
    *out->keys = n00b_list_new_private(n00b_uint128_t,
                                       .allocator = allocator,
                                       .scan_kind = N00B_GC_SCAN_KIND_NONE);

    n00b_store_normalized_list_t *terms = n00b_result_get(terms_r);
    size_t                        len   = n00b_list_len(*terms);
    out->matches_nothing                = len == 0;

    for (size_t i = 0; i < len; i++) {
        auto key_r = rocs_term_key(index->kind,
                                   n00b_list_get(*terms, i),
                                   .allocator = allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(n00b_store_index_keys_t *,
                                   n00b_result_get_err(key_r));
        }
        n00b_list_push(*out->keys, n00b_result_get(key_r));
    }

    return n00b_result_ok(n00b_store_index_keys_t *, out);
}

bool
n00b_store_index_keys_equal(n00b_store_index_keys_t *a,
                            n00b_store_index_keys_t *b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (a->matches_nothing != b->matches_nothing) {
        return false;
    }

    size_t len = n00b_list_len(*a->keys);
    if (len != (size_t)n00b_list_len(*b->keys)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        n00b_uint128_t x = n00b_list_get(*a->keys, i);
        n00b_uint128_t y = n00b_list_get(*b->keys, i);
        if (x != y) {
            return false;
        }
    }
    return true;
}

n00b_uint128_t
n00b_store_index_keys_at(n00b_store_index_keys_t *keys, uint64_t index)
{
    return n00b_list_get(*keys->keys, (size_t)index);
}

uint64_t
n00b_store_index_keys_digest(n00b_store_index_keys_t *keys)
{
    if (keys == nullptr) {
        return 0;
    }

    // Order-sensitive, matching n00b_store_index_keys_equal: the same terms in
    // a different order are a different lookup, so they must digest
    // differently. Mixing constant is the 64-bit FNV prime; the keys are
    // already hashes, so this only needs to spread them, not to hash them.
    uint64_t h   = keys->matches_nothing ? UINT64_C(0x9e3779b97f4a7c15) : 0;
    size_t   len = n00b_list_len(*keys->keys);

    for (size_t i = 0; i < len; i++) {
        n00b_uint128_t k = n00b_list_get(*keys->keys, i);
        h ^= (uint64_t)k;
        h *= UINT64_C(0x100000001b3);
        h ^= (uint64_t)(k >> 64);
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

uint64_t
n00b_store_index_keys_count(n00b_store_index_keys_t *keys)
{
    if (keys == nullptr || keys->matches_nothing) {
        return 0;
    }
    return (uint64_t)n00b_list_len(*keys->keys);
}

static n00b_result_t(n00b_store_index_keys_t *)
rocs_index_keys_for(n00b_store_index_t      *index,
                    n00b_json_node_t        *value,
                    n00b_store_index_keys_t *keys,
                    n00b_allocator_t        *allocator)
{
    if (keys != nullptr) {
        return n00b_result_ok(n00b_store_index_keys_t *, keys);
    }
    return n00b_store_index_keys_new(index, value, .allocator = allocator);
}

// Upper bound on a lookup's matches, read from the posting headers rather than
// from the postings. Both representations maintain their count on insert and
// the sealed image stores it in the header, so each term costs a dict probe
// and a field read.
//
// A multi-term lookup intersects its terms, so the smallest term's count
// bounds the result. Bounding rather than resolving is the point: a caller
// deciding whether the lookup is worth doing must not pay for the lookup to
// find out.
// A hot posting list's size.
//
// Read with no lock while the list's one writer updates it, which is why the
// field is atomic.
//
// So it is an estimate at the moment it is read, which is what every caller
// wants it for. Callers size work from it; none decides membership by it.
static uint64_t
rocs_hot_posting_count(n00b_store_posting_list_t *postings)
{
    if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
        // The maintained count, not a popcount: O(1) where popcounting the
        // bitmap is a pass over record_count/64 words on every df read.
        return n00b_atomic_load(&postings->count);
    }
    if (postings->ordinals == nullptr) {
        return 0;
    }

    return n00b_atomic_load(&postings->count);
}

static n00b_result_t(uint64_t)
rocs_index_df_hot_keys(n00b_store_index_keys_t *keys,
                       n00b_store_column_t     *column)
{
    size_t len = n00b_list_len(*keys->keys);
    if (keys->matches_nothing || len == 0) {
        return n00b_result_ok(uint64_t, 0);
    }

    uint64_t bound = UINT64_MAX;
    for (size_t i = 0; i < len; i++) {
        auto postings_r = rocs_column_postings_find(
            column,
            n00b_list_get(*keys->keys, i));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_posting_list_t *) current_opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(current_opt)) {
            return n00b_result_ok(uint64_t, 0);
        }

        n00b_store_posting_list_t *current = n00b_option_get(current_opt);
        if (current == nullptr) {
            return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_STATE);
        }
        uint64_t count = rocs_hot_posting_count(current);
        if (count < bound) {
            bound = count;
        }
    }

    return n00b_result_ok(uint64_t, bound);
}

// A resolved lookup that answers membership instead of enumerating. The term
// posting lists are found once; each ordinal after that costs a bitmap bit or
// a binary search, not a walk.
//
// This is the other way to read an index. Enumerating costs one step per
// posting; probing costs one step per candidate. Which is cheaper depends on
// how many of each there are, and the caller decides with the df bound.
//
// A catch-all probe holds one list per covered field carrying the term, and
// answers whether any of them has the ordinal, since the catch-all is their
// union. Every other probe holds one list per term and asks all of them.
struct n00b_store_index_probe_t {
    rocs_hot_posting_list_t    *hot;
    rocs_mapped_posting_list_t *mapped;
    bool                        any;
};

// The terms a catch-all lookup resolves, or null when the value normalizes to
// none, which matches nothing.
static n00b_result_t(n00b_store_normalized_list_t *)
rocs_catch_all_terms(n00b_store_index_t *index,
                     n00b_json_node_t   *value,
                     n00b_allocator_t   *allocator)
{
    auto terms_r = rocs_index_hot_terms(index, value, .allocator = allocator);
    if (n00b_result_is_err(terms_r)) {
        return terms_r;
    }
    n00b_store_normalized_list_t *terms = n00b_result_get(terms_r);
    return n00b_result_ok(n00b_store_normalized_list_t *,
                          n00b_list_len(*terms) == 0 ? nullptr : terms);
}

static n00b_result_t(n00b_store_index_probe_t *)
rocs_catch_all_probe_hot(n00b_store_index_t *index,
                         n00b_store_shard_t *shard,
                         n00b_json_node_t   *value,
                         n00b_allocator_t   *allocator)
{
    n00b_store_index_probe_t *probe = n00b_alloc_with_opts(
        n00b_store_index_probe_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    probe->hot = n00b_alloc_with_opts(
        rocs_hot_posting_list_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    *probe->hot   = n00b_list_new_private(n00b_store_posting_list_t *,
                                          .allocator = allocator,
                                          .scan_kind = N00B_GC_SCAN_KIND_ALL);
    probe->mapped = nullptr;
    probe->any    = true;

    auto terms_r = rocs_catch_all_terms(index, value, allocator);
    if (n00b_result_is_err(terms_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(terms_r));
    }
    if (n00b_result_get(terms_r) == nullptr) {
        return n00b_result_ok(n00b_store_index_probe_t *, probe);
    }

    auto key_r = rocs_catch_all_key(n00b_result_get(terms_r), allocator);
    if (n00b_result_is_err(key_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(key_r));
    }

    size_t                      field_count =
        n00b_list_len(*index->catch_all_fields);
    n00b_store_posting_list_t **lists       = (n00b_store_posting_list_t **)
        rocs_pointer_array(field_count, allocator);
    auto found_r = rocs_catch_all_hot_lists(index,
                                            shard,
                                            n00b_result_get(key_r),
                                            lists);
    if (n00b_result_is_err(found_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(found_r));
    }
    for (size_t i = 0; i < n00b_result_get(found_r); i++) {
        n00b_list_push(*probe->hot, lists[i]);
    }
    return n00b_result_ok(n00b_store_index_probe_t *, probe);
}

static n00b_result_t(n00b_store_index_probe_t *)
rocs_catch_all_probe_mapped(n00b_store_index_t     *index,
                            n00b_store_map_shard_t *shard,
                            n00b_json_node_t       *value,
                            n00b_allocator_t       *allocator)
{
    n00b_store_index_probe_t *probe = n00b_alloc_with_opts(
        n00b_store_index_probe_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    probe->hot    = nullptr;
    probe->mapped = n00b_alloc_with_opts(
        rocs_mapped_posting_list_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    *probe->mapped = n00b_list_new_private(n00b_store_map_posting_list_t *,
                                           .allocator = allocator,
                                           .scan_kind = N00B_GC_SCAN_KIND_ALL);
    probe->any     = true;

    auto terms_r = rocs_catch_all_terms(index, value, allocator);
    if (n00b_result_is_err(terms_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(terms_r));
    }
    if (n00b_result_get(terms_r) == nullptr) {
        return n00b_result_ok(n00b_store_index_probe_t *, probe);
    }

    auto key_r = rocs_catch_all_key(n00b_result_get(terms_r), allocator);
    if (n00b_result_is_err(key_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(key_r));
    }

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return n00b_result_err(
            n00b_store_index_probe_t *,
            rocs_index_map_err(n00b_result_get_err(columns_r)));
    }

    size_t                          field_count =
        n00b_list_len(*index->catch_all_fields);
    n00b_store_map_posting_list_t **lists       =
        (n00b_store_map_posting_list_t **)rocs_pointer_array(field_count,
                                                             allocator);
    auto found_r = rocs_catch_all_mapped_lists(index,
                                               n00b_result_get(columns_r),
                                               n00b_result_get(key_r),
                                               lists);
    if (n00b_result_is_err(found_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(found_r));
    }
    for (size_t i = 0; i < n00b_result_get(found_r); i++) {
        n00b_list_push(*probe->mapped, lists[i]);
    }
    return n00b_result_ok(n00b_store_index_probe_t *, probe);
}

n00b_result_t(n00b_store_index_probe_t *)
n00b_store_index_probe_hot(n00b_store_index_t *index,
                           n00b_store_shard_t *shard,
                           n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_index_probe_t *, ready);
    }
    if (shard == nullptr || value == nullptr || shard->columns == nullptr) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }
    if (index->catch_all) {
        return rocs_catch_all_probe_hot(index, shard, value, allocator);
    }

    auto keys_r = rocs_index_keys_for(index, value, keys, allocator);
    if (n00b_result_is_err(keys_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(keys_r));
    }
    n00b_store_index_keys_t *resolved = n00b_result_get(keys_r);

    n00b_store_index_probe_t *probe = n00b_alloc_with_opts(
        n00b_store_index_probe_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    probe->hot = n00b_alloc_with_opts(
        rocs_hot_posting_list_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    *probe->hot = n00b_list_new_private(n00b_store_posting_list_t *,
                                        .allocator = allocator,
                                        .scan_kind = N00B_GC_SCAN_KIND_ALL);
    probe->mapped = nullptr;
    probe->any    = false;

    size_t len = resolved->matches_nothing
                   ? 0
                   : n00b_list_len(*resolved->keys);

    bool                 found  = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns,
                                                index->field,
                                                &found);
    if (!found || column == nullptr || len == 0) {
        // No column and no terms both mean no record can match. An empty term
        // list answers false for every ordinal, which is that.
        return n00b_result_ok(n00b_store_index_probe_t *, probe);
    }

    for (size_t i = 0; i < len; i++) {
        auto postings_r = rocs_column_postings_find(
            column,
            n00b_list_get(*resolved->keys, i));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(n00b_store_index_probe_t *,
                                   n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_posting_list_t *) opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(opt)) {
            *probe->hot = n00b_list_new_private(
                n00b_store_posting_list_t *,
                .allocator = allocator,
                .scan_kind = N00B_GC_SCAN_KIND_ALL);
            n00b_list_push(*probe->hot, nullptr);
            return n00b_result_ok(n00b_store_index_probe_t *, probe);
        }
        n00b_list_push(*probe->hot, n00b_option_get(opt));
    }

    return n00b_result_ok(n00b_store_index_probe_t *, probe);
}

n00b_result_t(n00b_store_index_probe_t *)
n00b_store_index_probe_mapped(n00b_store_index_t     *index,
                              n00b_store_map_shard_t *shard,
                              n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_index_probe_t *, ready);
    }
    if (shard == nullptr || value == nullptr) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        return n00b_result_err(
            n00b_store_index_probe_t *,
            rocs_index_map_err(n00b_result_get_err(state_r)));
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               rocs_index_not_sealed(n00b_result_get(state_r)));
    }
    if (index->catch_all) {
        return rocs_catch_all_probe_mapped(index, shard, value, allocator);
    }

    auto keys_r = rocs_index_keys_for(index, value, keys, allocator);
    if (n00b_result_is_err(keys_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(keys_r));
    }
    n00b_store_index_keys_t *resolved = n00b_result_get(keys_r);

    n00b_store_index_probe_t *probe = n00b_alloc_with_opts(
        n00b_store_index_probe_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    probe->hot    = nullptr;
    probe->mapped = n00b_alloc_with_opts(
        rocs_mapped_posting_list_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    *probe->mapped = n00b_list_new_private(n00b_store_map_posting_list_t *,
                                           .allocator = allocator,
                                           .scan_kind = N00B_GC_SCAN_KIND_ALL);
    probe->any     = false;

    size_t len = resolved->matches_nothing
                   ? 0
                   : n00b_list_len(*resolved->keys);
    if (len == 0) {
        n00b_list_push(*probe->mapped, nullptr);
        return n00b_result_ok(n00b_store_index_probe_t *, probe);
    }

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return n00b_result_err(
            n00b_store_index_probe_t *,
            rocs_index_map_err(n00b_result_get_err(columns_r)));
    }

    auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                            index->field);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(n00b_store_index_probe_t *,
                               n00b_result_get_err(column_r));
    }

    n00b_option_t(n00b_store_map_dict_t *) column_opt =
        n00b_result_get(column_r);
    if (!n00b_option_is_set(column_opt)) {
        n00b_list_push(*probe->mapped, nullptr);
        return n00b_result_ok(n00b_store_index_probe_t *, probe);
    }
    n00b_store_map_dict_t *column = n00b_option_get(column_opt);

    for (size_t i = 0; i < len; i++) {
        auto postings_r = rocs_mapped_column_postings_find(
            column,
            n00b_list_get(*resolved->keys, i));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(n00b_store_index_probe_t *,
                                   n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_map_posting_list_t *) opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(opt)) {
            n00b_list_push(*probe->mapped, nullptr);
            return n00b_result_ok(n00b_store_index_probe_t *, probe);
        }
        n00b_list_push(*probe->mapped, n00b_option_get(opt));
    }

    return n00b_result_ok(n00b_store_index_probe_t *, probe);
}

n00b_result_t(bool)
n00b_store_index_probe_contains(n00b_store_index_probe_t *probe,
                                uint64_t                  ordinal)
{
    if (probe == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }

    // Every list must carry the ordinal for an intersection, and any one of
    // them for a catch-all's union. Either way the first list that settles it
    // ends the test. An empty probe matches nothing.
    bool settle = probe->any;

    if (probe->hot != nullptr) {
        size_t len = n00b_list_len(*probe->hot);
        if (len == 0) {
            return n00b_result_ok(bool, false);
        }
        for (size_t i = 0; i < len; i++) {
            n00b_store_posting_list_t *list = n00b_list_get(*probe->hot, i);
            ROCS_POSTING_STEPS(1);
            if (rocs_posting_list_contains_ordinal(list, ordinal) == settle) {
                return n00b_result_ok(bool, settle);
            }
        }
        return n00b_result_ok(bool, !settle);
    }

    if (probe->mapped == nullptr) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_STATE);
    }

    size_t len = n00b_list_len(*probe->mapped);
    if (len == 0) {
        return n00b_result_ok(bool, false);
    }
    for (size_t i = 0; i < len; i++) {
        n00b_store_map_posting_list_t *list = n00b_list_get(*probe->mapped, i);
        if (list == nullptr) {
            return n00b_result_ok(bool, false);
        }
        auto has_r = n00b_store_map_posting_list_contains(list, ordinal);
        if (n00b_result_is_err(has_r)) {
            return n00b_result_err(
                bool,
                rocs_index_map_err(n00b_result_get_err(has_r)));
        }
        if (n00b_result_get(has_r) == settle) {
            return n00b_result_ok(bool, settle);
        }
    }
    return n00b_result_ok(bool, !settle);
}

uint64_t
n00b_store_index_probe_width(n00b_store_index_probe_t *probe)
{
    if (probe == nullptr) {
        return 0;
    }
    if (probe->hot != nullptr) {
        return (uint64_t)n00b_list_len(*probe->hot);
    }
    if (probe->mapped != nullptr) {
        return (uint64_t)n00b_list_len(*probe->mapped);
    }
    return 0;
}

// Whether this probe answers membership by search or by scan.
//
// The cost model prices a membership test at ceil(log2(df)) steps, which is
// what a binary search over an ascending list costs. A sparse list that lost
// N00B_STORE_POSTINGS_ORDERED is scanned instead, at df steps, and a caller
// that priced the scan as a search picks the probe exactly where it is worst:
// the wider the term, the further the estimate is from the work.
//
// Dense lists answer from a bitmap bit and are always searchable. A null
// entry is a term the shard never indexed, which answers false for every
// ordinal without reading anything.
//
// A concurrent push can clear the bit after this returns, which downgrades
// the probe to a scan and costs speed only.
bool
n00b_store_index_probe_searchable(n00b_store_index_probe_t *probe)
{
    if (probe == nullptr) {
        return false;
    }

    if (probe->hot != nullptr) {
        size_t len = n00b_list_len(*probe->hot);
        for (size_t i = 0; i < len; i++) {
            n00b_store_posting_list_t *list = n00b_list_get(*probe->hot, i);
            if (list == nullptr
                || list->kind == N00B_STORE_POSTINGS_DENSE) {
                continue;
            }
            if ((n00b_atomic_load(&list->reserved)
                 & N00B_STORE_POSTINGS_ORDERED)
                == 0) {
                return false;
            }
        }
        return true;
    }

    if (probe->mapped == nullptr) {
        return false;
    }

    size_t len = n00b_list_len(*probe->mapped);
    for (size_t i = 0; i < len; i++) {
        n00b_store_map_posting_list_t *list = n00b_list_get(*probe->mapped, i);
        if (list != nullptr
            && !rocs_mapped_postings_advertise_order(list)) {
            return false;
        }
    }
    return true;
}

n00b_result_t(uint64_t)
n00b_store_index_df_hot(n00b_store_index_t *index,
                        n00b_store_shard_t *shard,
                        n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(uint64_t, ready);
    }
    if (shard == nullptr || value == nullptr || shard->columns == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_STATE);
    }
    // The catch-all unions the fields it covers, so the sum of their counts
    // bounds it. A record matching in two fields is counted twice, which makes
    // this an upper bound like every other df.
    if (index->catch_all) {
        auto terms_r = rocs_catch_all_terms(index, value, allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(terms_r));
        }
        if (n00b_result_get(terms_r) == nullptr) {
            return n00b_result_ok(uint64_t, 0);
        }
        auto key_r = rocs_catch_all_key(n00b_result_get(terms_r), allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(key_r));
        }

        n00b_store_posting_list_t **lists = (n00b_store_posting_list_t **)
            rocs_pointer_array(n00b_list_len(*index->catch_all_fields),
                               allocator);
        auto found_r = rocs_catch_all_hot_lists(index,
                                                shard,
                                                n00b_result_get(key_r),
                                                lists);
        if (n00b_result_is_err(found_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(found_r));
        }
        uint64_t sum = 0;
        for (size_t i = 0; i < n00b_result_get(found_r); i++) {
            sum += rocs_hot_posting_count(lists[i]);
        }
        return n00b_result_ok(uint64_t, sum);
    }

    auto keys_r = rocs_index_keys_for(index, value, keys, allocator);
    if (n00b_result_is_err(keys_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(keys_r));
    }

    bool                 found  = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns,
                                                index->field,
                                                &found);
    if (!found) {
        return n00b_result_ok(uint64_t, 0);
    }
    if (column == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_STATE);
    }

    return rocs_index_df_hot_keys(n00b_result_get(keys_r), column);
}

// Whether the posting lists this lookup would read advertise ascending order.
//
// For tests only. A reader falls back to a linear scan when the bit is clear,
// so a seal that stopped setting it would answer every query correctly while
// silently giving up the binary search, and nothing else would fail.
bool
n00b_store_index_sealed_is_ordered(n00b_store_index_t     *index,
                                   n00b_store_map_shard_t *shard,
                                   n00b_json_node_t       *value)
{
    auto keys_r = n00b_store_index_keys_new(index, value);
    if (n00b_result_is_err(keys_r)) {
        return false;
    }
    n00b_store_index_keys_t *keys = n00b_result_get(keys_r);

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return false;
    }
    auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                            index->field);
    if (n00b_result_is_err(column_r)) {
        return false;
    }
    n00b_option_t(n00b_store_map_dict_t *) column_opt =
        n00b_result_get(column_r);
    if (!n00b_option_is_set(column_opt)) {
        return false;
    }

    uint64_t len = n00b_store_index_keys_count(keys);
    if (len == 0) {
        return false;
    }

    for (uint64_t i = 0; i < len; i++) {
        auto pl_r = rocs_mapped_column_postings_find(
            n00b_option_get(column_opt),
            n00b_store_index_keys_at(keys, i));
        if (n00b_result_is_err(pl_r)) {
            return false;
        }
        n00b_option_t(n00b_store_map_posting_list_t *) pl_opt =
            n00b_result_get(pl_r);
        if (!n00b_option_is_set(pl_opt)) {
            return false;
        }
        if (!rocs_mapped_postings_advertise_order(n00b_option_get(pl_opt))) {
            return false;
        }
    }
    return true;
}

#ifdef N00B_DEBUG
uint64_t
n00b_store_index_sealed_clear_ordered(n00b_store_index_t     *index,
                                      n00b_store_map_shard_t *shard,
                                      n00b_json_node_t       *value)
{
    auto keys_r = n00b_store_index_keys_new(index, value);
    if (n00b_result_is_err(keys_r)) {
        return 0;
    }
    n00b_store_index_keys_t *keys = n00b_result_get(keys_r);

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return 0;
    }
    auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                            index->field);
    if (n00b_result_is_err(column_r)) {
        return 0;
    }
    n00b_option_t(n00b_store_map_dict_t *) column_opt =
        n00b_result_get(column_r);
    if (!n00b_option_is_set(column_opt)) {
        return 0;
    }

    uint64_t len     = n00b_store_index_keys_count(keys);
    uint64_t cleared = 0;
    for (uint64_t i = 0; i < len; i++) {
        auto pl_r = rocs_mapped_column_postings_find(
            n00b_option_get(column_opt),
            n00b_store_index_keys_at(keys, i));
        if (n00b_result_is_err(pl_r)) {
            continue;
        }
        n00b_option_t(n00b_store_map_posting_list_t *) pl_opt =
            n00b_result_get(pl_r);
        if (!n00b_option_is_set(pl_opt)) {
            continue;
        }
        if (rocs_mapped_postings_clear_order(n00b_option_get(pl_opt))) {
            cleared++;
        }
    }
    return cleared;
}

uint64_t
n00b_store_index_hot_skew_count(n00b_store_index_t *index,
                                n00b_store_shard_t *shard,
                                n00b_json_node_t   *value,
                                int64_t             delta)
{
    auto keys_r = n00b_store_index_keys_new(index, value);
    if (n00b_result_is_err(keys_r) || shard == nullptr
        || shard->columns == nullptr) {
        return 0;
    }
    n00b_store_index_keys_t *keys = n00b_result_get(keys_r);

    bool                 found  = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns,
                                                index->field,
                                                &found);
    if (!found || column == nullptr) {
        return 0;
    }

    uint64_t len    = n00b_store_index_keys_count(keys);
    uint64_t skewed = 0;
    for (uint64_t i = 0; i < len; i++) {
        auto pl_r = rocs_column_postings_find(column,
                                              n00b_store_index_keys_at(keys, i));
        if (n00b_result_is_err(pl_r)
            || !n00b_option_is_set(n00b_result_get(pl_r))) {
            continue;
        }
        n00b_store_posting_list_t *list = n00b_option_get(n00b_result_get(pl_r));
        n00b_atomic_store(&list->count,
                          (uint64_t)((int64_t)n00b_atomic_load(&list->count)
                                     + delta));
        skewed++;
    }
    return skewed;
}
#endif

n00b_result_t(uint64_t)
n00b_store_index_df_mapped(n00b_store_index_t     *index,
                           n00b_store_map_shard_t *shard,
                           n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
}
{
    n00b_err_t ready = rocs_index_hot_ready(index);
    if (ready != N00B_STORE_INDEX_OK) {
        return n00b_result_err(uint64_t, ready);
    }
    if (shard == nullptr || value == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        return n00b_result_err(uint64_t,
                               rocs_index_map_err(n00b_result_get_err(state_r)));
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(uint64_t, rocs_index_not_sealed(n00b_result_get(state_r)));
    }

    // Summed over the covered fields, as n00b_store_index_df_hot does.
    if (index->catch_all) {
        auto terms_r = rocs_catch_all_terms(index, value, allocator);
        if (n00b_result_is_err(terms_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(terms_r));
        }
        if (n00b_result_get(terms_r) == nullptr) {
            return n00b_result_ok(uint64_t, 0);
        }
        auto key_r = rocs_catch_all_key(n00b_result_get(terms_r), allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(key_r));
        }
        auto columns_r = n00b_store_map_shard_columns(shard);
        if (n00b_result_is_err(columns_r)) {
            return n00b_result_err(
                uint64_t,
                rocs_index_map_err(n00b_result_get_err(columns_r)));
        }

        n00b_store_map_posting_list_t **lists =
            (n00b_store_map_posting_list_t **)rocs_pointer_array(
                n00b_list_len(*index->catch_all_fields),
                allocator);
        auto found_r = rocs_catch_all_mapped_lists(index,
                                                   n00b_result_get(columns_r),
                                                   n00b_result_get(key_r),
                                                   lists);
        if (n00b_result_is_err(found_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(found_r));
        }
        uint64_t sum = 0;
        for (size_t i = 0; i < n00b_result_get(found_r); i++) {
            auto count_r = n00b_store_map_posting_list_len(lists[i]);
            if (n00b_result_is_err(count_r)) {
                return n00b_result_err(
                    uint64_t,
                    rocs_index_map_err(n00b_result_get_err(count_r)));
            }
            sum += n00b_result_get(count_r);
        }
        return n00b_result_ok(uint64_t, sum);
    }

    auto keys_r = rocs_index_keys_for(index, value, keys, allocator);
    if (n00b_result_is_err(keys_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(keys_r));
    }

    n00b_store_index_keys_t *resolved = n00b_result_get(keys_r);
    size_t                   len      = n00b_list_len(*resolved->keys);
    if (resolved->matches_nothing || len == 0) {
        return n00b_result_ok(uint64_t, 0);
    }

    auto columns_r = n00b_store_map_shard_columns(shard);
    if (n00b_result_is_err(columns_r)) {
        return n00b_result_err(
            uint64_t,
            rocs_index_map_err(n00b_result_get_err(columns_r)));
    }

    auto column_r = rocs_mapped_column_find(n00b_result_get(columns_r),
                                            index->field);
    if (n00b_result_is_err(column_r)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(column_r));
    }

    n00b_option_t(n00b_store_map_dict_t *) column_opt =
        n00b_result_get(column_r);
    if (!n00b_option_is_set(column_opt)) {
        return n00b_result_ok(uint64_t, 0);
    }
    n00b_store_map_dict_t *column = n00b_option_get(column_opt);

    uint64_t bound = UINT64_MAX;
    for (size_t i = 0; i < len; i++) {
        auto postings_r = rocs_mapped_column_postings_find(
            column,
            n00b_list_get(*resolved->keys, i));
        if (n00b_result_is_err(postings_r)) {
            return n00b_result_err(uint64_t, n00b_result_get_err(postings_r));
        }

        n00b_option_t(n00b_store_map_posting_list_t *) current_opt =
            n00b_result_get(postings_r);
        if (!n00b_option_is_set(current_opt)) {
            return n00b_result_ok(uint64_t, 0);
        }

        auto count_r =
            n00b_store_map_posting_list_len(n00b_option_get(current_opt));
        if (n00b_result_is_err(count_r)) {
            return n00b_result_err(
                uint64_t,
                rocs_index_map_err(n00b_result_get_err(count_r)));
        }

        uint64_t count = n00b_result_get(count_r);
        if (count < bound) {
            bound = count;
        }
    }

    return n00b_result_ok(uint64_t, bound);
}

// Read from posting headers, like the df it is built on, so asking costs a
// dict probe per term rather than a lookup. For a lookup of several terms, or
// the catch-all, the frequency is an upper bound on the match count rather
// than the count, and it is capped at the shard's records.
static n00b_store_index_stats_t
rocs_index_stats_from_counts(uint64_t record_count, uint64_t df)
{
    if (df > record_count) {
        df = record_count;
    }
    return (n00b_store_index_stats_t){
        .record_count       = record_count,
        .document_frequency = df,
        .selectivity        = record_count == 0
                                  ? 0.0
                                  : (double)df / (double)record_count,
    };
}

n00b_result_t(n00b_store_index_stats_t)
n00b_store_index_stats_hot(n00b_store_index_t *index,
                           n00b_store_shard_t *shard,
                           n00b_json_node_t   *value)
{
    if (shard == nullptr) {
        return n00b_result_err(n00b_store_index_stats_t,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto df_r = n00b_store_index_df_hot(index, shard, value);
    if (n00b_result_is_err(df_r)) {
        return n00b_result_err(n00b_store_index_stats_t,
                               n00b_result_get_err(df_r));
    }

    return n00b_result_ok(
        n00b_store_index_stats_t,
        rocs_index_stats_from_counts(shard->record_count,
                                     n00b_result_get(df_r)));
}

n00b_result_t(n00b_store_index_stats_t)
n00b_store_index_stats_mapped(n00b_store_index_t     *index,
                              n00b_store_map_shard_t *shard,
                              n00b_json_node_t       *value)
{
    if (shard == nullptr) {
        return n00b_result_err(n00b_store_index_stats_t,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto record_count_r = n00b_store_map_shard_records_len(shard);
    if (n00b_result_is_err(record_count_r)) {
        return n00b_result_err(
            n00b_store_index_stats_t,
            rocs_index_map_err(n00b_result_get_err(record_count_r)));
    }

    auto df_r = n00b_store_index_df_mapped(index, shard, value);
    if (n00b_result_is_err(df_r)) {
        return n00b_result_err(n00b_store_index_stats_t,
                               n00b_result_get_err(df_r));
    }

    return n00b_result_ok(
        n00b_store_index_stats_t,
        rocs_index_stats_from_counts(n00b_result_get(record_count_r),
                                     n00b_result_get(df_r)));
}

n00b_result_t(n00b_store_postings_t *)
n00b_store_postings_empty() _kargs
{
    uint64_t          shard_id   = 0;
    uint64_t          generation = 0;
    n00b_allocator_t *allocator  = nullptr;
}
{
    return rocs_empty_postings(shard_id, generation, .allocator = allocator);
}

n00b_result_t(uint64_t)
n00b_store_postings_len(n00b_store_postings_t *postings)
{
    if (postings == nullptr) {
        return n00b_result_err(uint64_t, N00B_STORE_INDEX_ERR_ARG);
    }

    return n00b_result_ok(uint64_t, postings->len);
}

const uint64_t *
n00b_store_postings_ordinals(n00b_store_postings_t *postings,
                             uint64_t              *len_out)
{
    if (postings == nullptr) {
        *len_out = 0;
        return nullptr;
    }
    *len_out = postings->len;
    return postings->ordinals;
}

static n00b_store_pos_t
rocs_postings_pos_at(n00b_store_postings_t *postings, uint64_t index)
{
    return (n00b_store_pos_t){
        .shard_id   = postings->shard_id,
        .ordinal    = postings->ordinals[index],
        .generation = postings->generation,
    };
}

n00b_result_t(n00b_option_t(n00b_store_pos_t))
n00b_store_postings_pos(n00b_store_postings_t *postings, uint64_t ordinal)
{
    if (postings == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_pos_t),
                               N00B_STORE_INDEX_ERR_ARG);
    }

    if (ordinal >= postings->len) {
        return n00b_result_ok(n00b_option_t(n00b_store_pos_t),
                              n00b_option_none(n00b_store_pos_t));
    }

    return n00b_result_ok(
        n00b_option_t(n00b_store_pos_t),
        n00b_option_set(n00b_store_pos_t,
                        rocs_postings_pos_at(postings, ordinal)));
}

n00b_result_t(n00b_option_t(n00b_store_posting_t))
n00b_store_postings_get(n00b_store_postings_t *postings, uint64_t ordinal)
{
    if (postings == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_posting_t),
                               N00B_STORE_INDEX_ERR_ARG);
    }

    if (ordinal >= postings->len) {
        return n00b_result_ok(n00b_option_t(n00b_store_posting_t),
                              n00b_option_none(n00b_store_posting_t));
    }

    // A view is built per call, so a lookup nobody reads views from builds none.
    n00b_store_pos_t     pos    = rocs_postings_pos_at(postings, ordinal);
    n00b_store_record_t *record = nullptr;
    if (postings->hot_shard != nullptr) {
        record = _rocs_record_view_new(pos,
                                       postings->hot_shard,
                                       nullptr,
                                       .allocator = postings->allocator);
    }
    else if (postings->mapped_shard != nullptr) {
        record = _rocs_record_view_new(pos,
                                       nullptr,
                                       postings->mapped_shard,
                                       .allocator = postings->allocator);
    }
    if (record == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_store_posting_t),
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_posting_t posting = {
        .pos    = pos,
        .record = record,
    };
    return n00b_result_ok(n00b_option_t(n00b_store_posting_t),
                          n00b_option_set(n00b_store_posting_t, posting));
}

n00b_result_t(n00b_store_pos_t)
n00b_store_record_pos(n00b_store_record_t *record)
{
    if (record == nullptr) {
        return n00b_result_err(n00b_store_pos_t, N00B_STORE_INDEX_ERR_ARG);
    }

    return n00b_result_ok(n00b_store_pos_t, record->pos);
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_hot_at(n00b_store_shard_t *shard,
                              uint64_t            ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (shard == nullptr || shard->records == nullptr) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    // Live callers bound ordinals by the post-fill publication watermark.
    if (ordinal >= n00b_store_shard_records_len(shard)) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (n00b_store_shard_record_at(shard, ordinal) == nullptr) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_record_t *view = _rocs_record_view_new(
        (n00b_store_pos_t){
            .shard_id   = shard->shard_id,
            .ordinal    = ordinal,
            .generation = shard->seal_ts,
        },
        shard,
        nullptr,
        .allocator = allocator);
    return n00b_result_ok(n00b_store_record_t *, view);
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_hot_pos(n00b_store_shard_t *shard,
                               n00b_store_pos_t    pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (shard == nullptr || shard->records == nullptr || pos.shard_id == 0) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (shard->state != N00B_SHARD_STATE_OPEN
        && shard->state != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }
    if (pos.shard_id != shard->shard_id) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    // Live callers bound ordinals by the post-fill publication watermark.
    if (pos.ordinal >= n00b_store_shard_records_len(shard)) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }
    if (n00b_store_shard_record_at(shard, pos.ordinal) == nullptr) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_store_record_t *view = _rocs_record_view_new(pos,
                                                      shard,
                                                      nullptr,
                                                      .allocator = allocator);
    return n00b_result_ok(n00b_store_record_t *, view);
}

static n00b_err_t
rocs_index_mapped_record_check(n00b_store_map_shard_t *shard, uint64_t ordinal)
{
    auto len_r = n00b_store_map_shard_records_len(shard);
    if (n00b_result_is_err(len_r)) {
        return rocs_index_map_err(n00b_result_get_err(len_r));
    }
    if (ordinal >= n00b_result_get(len_r)) {
        return N00B_STORE_INDEX_ERR_ARG;
    }
    auto ref_r = n00b_store_map_shard_record_ref(shard, ordinal, false);
    if (n00b_result_is_err(ref_r)) {
        return rocs_index_map_err(n00b_result_get_err(ref_r));
    }
    n00b_option_t(uint64_t) ref = n00b_result_get(ref_r);
    if (!n00b_option_is_set(ref)) {
        return N00B_STORE_INDEX_ERR_ARG;
    }
    if (n00b_option_get(ref) == 0) {
        if (getenv("ROCS_QUERY_DEBUG") != NULL) {
            auto shard_id_r = n00b_store_map_shard_id(shard);
            fprintf(stderr,
                    "rocs index: mapped pos empty record ref "
                    "shard=%llu ordinal=%llu\n",
                    (unsigned long long)n00b_result_get(shard_id_r),
                    (unsigned long long)ordinal);
        }
        return N00B_STORE_INDEX_ERR_STATE;
    }
    return N00B_STORE_INDEX_OK;
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_mapped_at(n00b_store_map_shard_t *shard,
                                 uint64_t                ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_store_pos_t pos = {};
    if (shard == nullptr) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        return n00b_result_err(n00b_store_record_t *,
                               rocs_index_map_err(n00b_result_get_err(state_r)));
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        if (getenv("ROCS_QUERY_DEBUG") != NULL) {
            auto shard_id_r = n00b_store_map_shard_id(shard);
            fprintf(stderr,
                    "rocs index: mapped pos state mismatch "
                    "shard=%llu ordinal=%llu state=%lld\n",
                    (unsigned long long)n00b_result_get(shard_id_r),
                    (unsigned long long)ordinal,
                    (long long)n00b_result_get(state_r));
        }
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_err_t err = rocs_index_mapped_record_check(shard, ordinal);
    if (err != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_record_t *, err);
    }

    auto shard_id_r = n00b_store_map_shard_id(shard);
    if (n00b_result_is_err(shard_id_r)) {
        return n00b_result_err(n00b_store_record_t *,
                               rocs_index_map_err(n00b_result_get_err(shard_id_r)));
    }
    auto generation_r = n00b_store_map_shard_seal_ts(shard);
    if (n00b_result_is_err(generation_r)) {
        return n00b_result_err(n00b_store_record_t *,
                               rocs_index_map_err(n00b_result_get_err(generation_r)));
    }

    pos = (n00b_store_pos_t){
        .shard_id   = n00b_result_get(shard_id_r),
        .ordinal    = ordinal,
        .generation = n00b_result_get(generation_r),
    };

    n00b_store_record_t *view = _rocs_record_view_new(pos,
                                                      nullptr,
                                                      shard,
                                                      .allocator = allocator);
    return n00b_result_ok(n00b_store_record_t *, view);
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_mapped_pos(n00b_store_map_shard_t *shard,
                                  n00b_store_pos_t        pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (shard == nullptr || pos.shard_id == 0) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        return n00b_result_err(n00b_store_record_t *,
                               rocs_index_map_err(n00b_result_get_err(state_r)));
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        if (getenv("ROCS_QUERY_DEBUG") != NULL) {
            fprintf(stderr,
                    "rocs index: mapped pos state mismatch "
                    "shard=%llu ordinal=%llu state=%lld\n",
                    (unsigned long long)pos.shard_id,
                    (unsigned long long)pos.ordinal,
                    (long long)n00b_result_get(state_r));
        }
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    auto shard_id_r = n00b_store_map_shard_id(shard);
    if (n00b_result_is_err(shard_id_r)) {
        return n00b_result_err(n00b_store_record_t *,
                               rocs_index_map_err(n00b_result_get_err(shard_id_r)));
    }
    if (n00b_result_get(shard_id_r) != pos.shard_id) {
        if (getenv("ROCS_QUERY_DEBUG") != NULL) {
            fprintf(stderr,
                    "rocs index: mapped pos shard mismatch "
                    "wanted=%llu mapped=%llu ordinal=%llu\n",
                    (unsigned long long)pos.shard_id,
                    (unsigned long long)n00b_result_get(shard_id_r),
                    (unsigned long long)pos.ordinal);
        }
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_STATE);
    }

    n00b_err_t err = rocs_index_mapped_record_check(shard, pos.ordinal);
    if (err != N00B_STORE_INDEX_OK) {
        return n00b_result_err(n00b_store_record_t *, err);
    }

    n00b_store_record_t *view = _rocs_record_view_new(
        pos,
        nullptr,
        shard,
        .allocator = allocator);
    return n00b_result_ok(n00b_store_record_t *, view);
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_owned_json(n00b_store_pos_t   pos,
                                  n00b_json_node_t  *json) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (json == nullptr || pos.shard_id == 0) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    n00b_store_record_t *view = _rocs_record_view_new(
        pos,
        nullptr,
        nullptr,
        .allocator = allocator);
    view->owned_json = json;
    return n00b_result_ok(n00b_store_record_t *, view);
}

n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_owned_text(n00b_store_pos_t  pos,
                                  n00b_string_t    *text) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (text == nullptr || pos.shard_id == 0) {
        return n00b_result_err(n00b_store_record_t *,
                               N00B_STORE_INDEX_ERR_ARG);
    }

    n00b_store_record_t *view = _rocs_record_view_new(
        pos,
        nullptr,
        nullptr,
        .allocator = allocator);
    view->owned_text = text;
    return n00b_result_ok(n00b_store_record_t *, view);
}

// A hot record's shard must still hold records for a view onto it to read one.
static n00b_err_t
rocs_record_hot_readable(n00b_store_record_t *record)
{
    n00b_store_shard_t *shard = record->hot_shard;
    if (shard->records == nullptr
        || (shard->state != N00B_SHARD_STATE_OPEN
            && shard->state != N00B_SHARD_STATE_SEALED)) {
        return N00B_STORE_INDEX_ERR_STATE;
    }
    if (record->pos.ordinal >= n00b_store_shard_records_len(shard)) {
        return N00B_STORE_INDEX_ERR_STATE;
    }
    return N00B_STORE_INDEX_OK;
}

n00b_result_t(n00b_json_node_t *)
n00b_store_record_view_json(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (record == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    if (record->owned_json != nullptr) {
        return n00b_result_ok(n00b_json_node_t *, record->owned_json);
    }

    // Text-backed record: parse for the caller that actually wants a node
    // graph, into the allocator that caller named. Deliberately not cached on
    // the record -- caching would hand a later caller a graph owned by an
    // earlier caller's allocator, which for a per-row scratch arena is a
    // use-after-reset. Every current caller asks once.
    if (record->owned_text != nullptr) {
        ROCS_COUNT(rocs_record_parses);
        const char       *err  = nullptr;
        n00b_json_node_t *node = n00b_json_parse(record->owned_text->data,
                                                 record->owned_text->u8_bytes,
                                                 &err,
                                                 .allocator = allocator);
        if (node == nullptr || err != nullptr) {
            return n00b_result_err(n00b_json_node_t *,
                                   N00B_STORE_INDEX_ERR_STATE);
        }
        return n00b_result_ok(n00b_json_node_t *, node);
    }

    if (record->hot_shard != nullptr) {
        n00b_err_t err = rocs_record_hot_readable(record);
        if (err != N00B_STORE_INDEX_OK) {
            return n00b_result_err(n00b_json_node_t *, err);
        }

        ROCS_COUNT(rocs_record_parses);
        return rocs_hot_shard_record_json(record->hot_shard,
                                          record->pos.ordinal,
                                          .allocator = allocator);
    }

    if (record->mapped_shard == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    ROCS_COUNT(rocs_record_parses);
    auto node_r =
        n00b_store_map_shard_record_json_copy(record->mapped_shard,
                                             record->pos.ordinal,
                                             .allocator = allocator);
    if (n00b_result_is_err(node_r)) {
        return n00b_result_err(n00b_json_node_t *,
                               rocs_index_map_err(n00b_result_get_err(node_r)));
    }

    return n00b_result_ok(n00b_json_node_t *, n00b_result_get(node_r));
}

// The record's stored compact JSON, borrowed: the hot shard's string, the
// sealed image's bytes, or the text the view owns. False when the record has
// no bytes to borrow, which is an owned graph or a sealed record the image
// does not hold as a string.
static bool
rocs_record_bytes(n00b_store_record_t *record,
                  const char         **data_out,
                  size_t              *len_out)
{
    if (record->owned_text != nullptr) {
        *data_out = record->owned_text->data;
        *len_out  = record->owned_text->u8_bytes;
        return true;
    }
    if (record->hot_shard != nullptr) {
        if (rocs_record_hot_readable(record) != N00B_STORE_INDEX_OK) {
            return false;
        }
        auto text_r = rocs_hot_shard_record_stored(record->hot_shard,
                                                   record->pos.ordinal);
        if (n00b_result_is_err(text_r)) {
            return false;
        }
        n00b_string_t *text = n00b_result_get(text_r);
        *data_out           = text->data;
        *len_out            = text->u8_bytes;
        return true;
    }
    if (record->mapped_shard != nullptr) {
        auto span_r = n00b_store_map_shard_record_span(record->mapped_shard,
                                                       record->pos.ordinal);
        if (n00b_result_is_err(span_r)) {
            return false;
        }
        n00b_store_byte_span_t span = n00b_result_get(span_r);
        *data_out                   = (const char *)span.data;
        *len_out                    = (size_t)span.byte_len;
        return true;
    }
    return false;
}

n00b_result_t(bool)
n00b_store_record_view_fields(n00b_store_record_t *record,
                              size_t               count,
                              n00b_string_t      **fields,
                              n00b_json_node_t   **values) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (record == nullptr || (count != 0 && (fields == nullptr
                                             || values == nullptr))) {
        return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
    }
    for (size_t i = 0; i < count; i++) {
        if (fields[i] == nullptr) {
            return n00b_result_err(bool, N00B_STORE_INDEX_ERR_ARG);
        }
        values[i] = nullptr;
    }
    if (count == 0) {
        return n00b_result_ok(bool, true);
    }

    const char *data = nullptr;
    size_t      len  = 0;
    if (record->owned_json == nullptr && count <= ROCS_JSON_SCAN_FIELDS_MAX
        && rocs_record_bytes(record, &data, &len)) {
        rocs_json_span_t spans[ROCS_JSON_SCAN_FIELDS_MAX];

        ROCS_COUNT(rocs_record_field_scans);
        if (rocs_json_scan_fields(data, len, count, fields, spans)
            == ROCS_JSON_SCAN_FOUND) {
            for (size_t i = 0; i < count; i++) {
                if (!spans[i].found) {
                    continue;
                }
                const char *err = nullptr;
                values[i]       = n00b_json_parse(data + spans[i].start,
                                            spans[i].len,
                                            &err,
                                            .allocator = allocator);
                if (values[i] == nullptr || err != nullptr) {
                    return n00b_result_err(bool, N00B_STORE_INDEX_ERR_STATE);
                }
            }
            return n00b_result_ok(bool, true);
        }
    }

    // The scan declined, or there are no bytes to scan: parse the record once
    // and read every field from the graph.
    auto json_r = n00b_store_record_view_json(record, .allocator = allocator);
    if (n00b_result_is_err(json_r)) {
        return n00b_result_err(bool, n00b_result_get_err(json_r));
    }
    n00b_json_node_t *json = n00b_result_get(json_r);
    for (size_t i = 0; i < count; i++) {
        values[i] = rocs_json_object_get_field(json,
                                               fields[i],
                                               .allocator = allocator);
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_option_t(n00b_json_node_t *))
n00b_store_record_view_field(n00b_store_record_t *record,
                             n00b_string_t       *field) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_json_node_t *value = nullptr;
    auto              ok_r  = n00b_store_record_view_fields(record,
                                                            1,
                                                            &field,
                                                            &value,
                                                            .allocator =
                                                                allocator);
    if (n00b_result_is_err(ok_r)) {
        return n00b_result_err(n00b_option_t(n00b_json_node_t *),
                               n00b_result_get_err(ok_r));
    }
    if (value == nullptr) {
        return n00b_result_ok(n00b_option_t(n00b_json_node_t *),
                              n00b_option_none(n00b_json_node_t *));
    }
    return n00b_result_ok(n00b_option_t(n00b_json_node_t *),
                          n00b_option_set(n00b_json_node_t *, value));
}

n00b_result_t(n00b_json_node_t *)
n00b_store_record_view_json_copy(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (record == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_STORE_INDEX_ERR_ARG);
    }

    auto json_r = n00b_store_record_view_json(record,
                                             .allocator = allocator);
    if (n00b_result_is_err(json_r) || record->owned_json == nullptr) {
        // Parsed just now into the caller's allocator, so it is already the
        // caller's own graph.
        return json_r;
    }

    return rocs_json_node_copy(n00b_result_get(json_r),
                               .allocator = allocator);
}

// Serialize a JSON node graph to a compact n00b string. The returned char*
// boundary is confined here and allocated from the caller's allocator.
static n00b_result_t(n00b_string_t *)
rocs_json_node_to_string(n00b_json_node_t *node, n00b_allocator_t *allocator)
{
    char *encoded = n00b_json_encode(node,
                                     .pretty = false,
                                     .allocator = allocator);
    if (encoded == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_STATE);
    }
    n00b_string_t *text = n00b_string_from_cstr(encoded, .allocator = allocator);
    n00b_free(encoded);
    return n00b_result_ok(n00b_string_t *, text);
}

n00b_result_t(n00b_string_t *)
n00b_store_record_view_json_string(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (record == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_ARG);
    }
    // The record already IS the stored compact JSON (a hot record copied
    // verbatim).
    if (record->owned_text != nullptr) {
        return n00b_result_ok(n00b_string_t *, record->owned_text);
    }

    // An owned graph is the only record without stored bytes, so it is the
    // only one encoded.
    if (record->owned_json != nullptr) {
        return rocs_json_node_to_string(record->owned_json, allocator);
    }

    // A hot shard holds each record as compact JSON; copy its bytes.
    if (record->hot_shard != nullptr) {
        n00b_err_t err = rocs_record_hot_readable(record);
        if (err != N00B_STORE_INDEX_OK) {
            return n00b_result_err(n00b_string_t *, err);
        }
        return rocs_hot_shard_record_text(record->hot_shard,
                                          record->pos.ordinal,
                                          .allocator = allocator);
    }

    if (record->mapped_shard == nullptr) {
        // Malformed/empty record: no backing for any path.
        return n00b_result_err(n00b_string_t *, N00B_STORE_INDEX_ERR_STATE);
    }

    // A sealed mapped record is persisted as a compact JSON string in the
    // read-only image. Return those bytes verbatim: no parse, no node graph,
    // no re-encode. This is the egress drain's hot path.
    auto str_r = n00b_store_map_shard_record_json_string(
        record->mapped_shard,
        record->pos.ordinal,
        .allocator = allocator);
    if (n00b_result_is_err(str_r)) {
        return n00b_result_err(n00b_string_t *,
                               rocs_index_map_err(n00b_result_get_err(str_r)));
    }
    return n00b_result_ok(n00b_string_t *, n00b_result_get(str_r));
}
