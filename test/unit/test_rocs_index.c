/* test/unit/test_rocs_index.c - WP-004 index contracts. */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/codegen_abi.h"
#include "core/static_objects.h"
#include "core/atomic.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "util/assert.h"

#include <rocs/n00b_rocs.h>
#include <rocs/index.h>
#include "rocs_test_support.h"
#include "test_check.h"

static n00b_json_node_t *
record_with_level(n00b_string_t *level)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record,
                              r"level",
                              n00b_json_string_new_from_n00b(level));
    return record;
}

static n00b_json_node_t *
record_without_level(void)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record,
                              r"message",
                              n00b_json_string_new_from_n00b(r"no level"));
    return record;
}

static n00b_json_node_t *
record_with_count(int64_t count)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record, r"count", n00b_json_int_new(count));
    n00b_json_object_put_n00b(record, r"flag", n00b_json_bool_new(true));
    n00b_json_object_put_n00b(record, r"none", n00b_json_null_new());
    n00b_json_object_put_n00b(record, r"ratio", n00b_json_double_new(3.5));
    return record;
}

static n00b_json_node_t *
record_with_scalar(n00b_json_node_t *value)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record, r"scalar", value);
    return record;
}

static void
poison_live_bucket_sync_flags(n00b_dict_bucket_t *buckets, uint32_t last_slot)
{
    uint32_t sync_flags = N00B_HT_FLAG_MUTEX | N00B_HT_FLAG_COPYING
                        | N00B_HT_FLAG_MOVING;

    for (uint32_t i = 0; i <= last_slot; i++) {
        n00b_dict_bucket_t *bucket = &buckets[i];
        uint32_t flags = atomic_load_explicit(&bucket->flags,
                                              memory_order_relaxed);

        if (bucket->hv == (n00b_uint128_t)0
            || (flags & N00B_HT_FLAG_DELETED) != 0) {
            continue;
        }

        atomic_fetch_or_explicit(&bucket->flags,
                                 sync_flags,
                                 memory_order_relaxed);
    }
}

static void
poison_columns_sync_flags(n00b_store_columns_t *columns)
{
    CHECK(columns != nullptr);
    n00b_dict_store_t(n00b_string_t *, n00b_store_column_t *) *store =
        atomic_load_explicit(&columns->store, memory_order_relaxed);
    CHECK(store != nullptr);
    poison_live_bucket_sync_flags(store->buckets, store->last_slot);
}

static void
poison_column_sync_flags(n00b_store_column_t *column)
{
    CHECK(column != nullptr);
    n00b_dict_store_t(n00b_uint128_t, n00b_store_posting_list_t *) *store =
        atomic_load_explicit(&column->store, memory_order_relaxed);
    CHECK(store != nullptr);
    poison_live_bucket_sync_flags(store->buckets, store->last_slot);
}

static void
poison_index_sync_flags(n00b_store_shard_t *shard, n00b_string_t *field)
{
    CHECK(shard != nullptr);
    CHECK(shard->columns != nullptr);

    bool found = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns, field, &found);
    CHECK(found);
    CHECK(column != nullptr);

    poison_columns_sync_flags(shard->columns);
    poison_column_sync_flags(column);
}

static n00b_store_index_t *
term_index(n00b_string_t *field)
{
    auto index_r = n00b_store_index_new(field, N00B_STORE_INDEX_TERM);
    CHECK(n00b_result_is_ok(index_r));
    return n00b_result_get(index_r);
}

static n00b_store_index_t *
dense_term_index(n00b_string_t *field)
{
    auto index_r = n00b_store_index_new(field,
                                        N00B_STORE_INDEX_TERM,
                                        .postings = N00B_STORE_POSTINGS_DENSE);
    CHECK(n00b_result_is_ok(index_r));
    return n00b_result_get(index_r);
}

static n00b_store_shard_t *
indexed_level_shard(n00b_store_index_t *index)
{
    auto shard_r = n00b_store_shard_new(.shard_id = UINT64_C(0xfeedface), .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    n00b_json_node_t *info    = record_with_level(r"info");
    n00b_json_node_t *error_a = record_with_level(r"error");
    n00b_json_node_t *error_b = record_with_level(r"error");
    n00b_json_node_t *missing = record_without_level();

    auto a0 = n00b_store_shard_append(shard, info);
    auto a1 = n00b_store_shard_append(shard, error_a);
    auto a2 = n00b_store_shard_append(shard, error_b);
    auto a3 = n00b_store_shard_append(shard, missing);
    CHECK(n00b_result_is_ok(a0));
    CHECK(n00b_result_is_ok(a1));
    CHECK(n00b_result_is_ok(a2));
    CHECK(n00b_result_is_ok(a3));

    auto i0 = n00b_store_index_add(index, shard, n00b_result_get(a0));
    auto i1 = n00b_store_index_add(index, shard, n00b_result_get(a1));
    auto i2 = n00b_store_index_add(index, shard, n00b_result_get(a2));
    auto i3 = n00b_store_index_add(index, shard, n00b_result_get(a3));
    CHECK(n00b_result_is_ok(i0));
    CHECK(n00b_result_is_ok(i1));
    CHECK(n00b_result_is_ok(i2));
    CHECK(n00b_result_is_ok(i3));
    CHECK(n00b_result_get(i0) == 1);
    CHECK(n00b_result_get(i1) == 1);
    CHECK(n00b_result_get(i2) == 1);
    CHECK(n00b_result_get(i3) == 0);

    return shard;
}

static n00b_store_posting_t
posting_at(n00b_store_postings_t *postings, uint64_t ordinal)
{
    auto posting_r = n00b_store_postings_get(postings, ordinal);
    CHECK(n00b_result_is_ok(posting_r));
    n00b_option_t(n00b_store_posting_t) opt = n00b_result_get(posting_r);
    CHECK(n00b_option_is_set(opt));
    return n00b_option_get(opt);
}

static void
check_len(n00b_store_postings_t *postings, uint64_t expected)
{
    auto len_r = n00b_store_postings_len(postings);
    CHECK(n00b_result_is_ok(len_r));
    CHECK(n00b_result_get(len_r) == expected);
}

static uint64_t
append_and_index_scalar(n00b_store_index_t *index,
                        n00b_store_shard_t *shard,
                        n00b_json_node_t   *value)
{
    auto append_r = n00b_store_shard_append(shard, record_with_scalar(value));
    CHECK(n00b_result_is_ok(append_r));

    uint64_t ordinal = n00b_result_get(append_r);
    auto     index_r = n00b_store_index_add(index, shard, ordinal);
    CHECK(n00b_result_is_ok(index_r));
    CHECK(n00b_result_get(index_r) == 1);
    return ordinal;
}

static void
check_hot_single_hit(n00b_store_index_t *index,
                     n00b_store_shard_t *shard,
                     n00b_json_node_t   *value,
                     uint64_t            shard_id,
                     uint64_t            ordinal)
{
    auto hit_r = n00b_store_index_lookup(index, shard, value);
    CHECK(n00b_result_is_ok(hit_r));
    n00b_store_postings_t *hits = n00b_result_get(hit_r);
    check_len(hits, 1);

    n00b_store_posting_t hit = posting_at(hits, 0);
    CHECK(hit.pos.shard_id == shard_id);
    CHECK(hit.pos.ordinal == ordinal);
    CHECK(hit.pos.generation == 0);
    CHECK(hit.record != nullptr);
}

static void
check_mapped_single_hit(n00b_store_index_t     *index,
                        n00b_store_map_shard_t *shard,
                        n00b_json_node_t       *value,
                        uint64_t                shard_id,
                        uint64_t                ordinal,
                        uint64_t                generation)
{
    auto hit_r = n00b_store_index_lookup_mapped(index, shard, value);
    CHECK(n00b_result_is_ok(hit_r));
    n00b_store_postings_t *hits = n00b_result_get(hit_r);
    check_len(hits, 1);

    n00b_store_posting_t hit = posting_at(hits, 0);
    CHECK(hit.pos.shard_id == shard_id);
    CHECK(hit.pos.ordinal == ordinal);
    CHECK(hit.pos.generation == generation);
    CHECK(hit.record != nullptr);
}

static void
test_public_contracts(void)
{
    static_assert((N00B_ROCS_CAPABILITIES & N00B_ROCS_CAP_STORE_INDEX_DECLS) != 0);
    static_assert(N00B_STORE_INDEX_NONE == 0);
    static_assert(N00B_STORE_INDEX_TERM == 1);
    // No brittle exact-sizeof assert: the field offsets below (and the symbolic
    // posting-layout check) validate the ABI that actually matters, and don't
    // need updating when a new trailing field is appended.
    static_assert(offsetof(n00b_store_pos_t, shard_id) == 0);
    static_assert(offsetof(n00b_store_pos_t, ordinal) == 8);
    static_assert(offsetof(n00b_store_pos_t, generation) == 16);
    static_assert(offsetof(n00b_store_posting_t, pos) == 0);
    static_assert(offsetof(n00b_store_posting_t, record)
                  == sizeof(n00b_store_pos_t));

    CHECK(n00b_store_index_err_str(N00B_STORE_INDEX_OK) != nullptr);
    CHECK(n00b_store_index_err_str(N00B_STORE_INDEX_ERR_ARG) != nullptr);
    CHECK(n00b_store_index_err_str(9999) != nullptr);

    static_assert(N00B_STORE_INDEX_OP_PREFIX == 6);
}

static void
test_index_descriptor_contract(void)
{
    n00b_string_t *field = r"message";
    n00b_string_t *other = r"level";

    auto null_field = n00b_store_index_new(nullptr, N00B_STORE_INDEX_TERM);
    CHECK(n00b_result_is_err(null_field));
    CHECK(n00b_result_get_err(null_field) == N00B_STORE_INDEX_ERR_ARG);

    auto none_kind = n00b_store_index_new(field, N00B_STORE_INDEX_NONE);
    CHECK(n00b_result_is_err(none_kind));
    CHECK(n00b_result_get_err(none_kind) == N00B_STORE_INDEX_ERR_KIND);

    auto bad_kind = n00b_store_index_new(field, (n00b_store_index_kind_t)99);
    CHECK(n00b_result_is_err(bad_kind));
    CHECK(n00b_result_get_err(bad_kind) == N00B_STORE_INDEX_ERR_KIND);

    auto index_r = n00b_store_index_new(field, N00B_STORE_INDEX_TERM);
    CHECK(n00b_result_is_ok(index_r));
    n00b_store_index_t *index = n00b_result_get(index_r);
    CHECK(index != nullptr);

    auto kind_r = n00b_store_index_kind(index);
    CHECK(n00b_result_is_ok(kind_r));
    CHECK(n00b_result_get(kind_r) == N00B_STORE_INDEX_TERM);

    auto postings_r = n00b_store_index_postings_kind(index);
    CHECK(n00b_result_is_ok(postings_r));
    CHECK(n00b_result_get(postings_r) == N00B_STORE_POSTINGS_SPARSE);

    auto field_r = n00b_store_index_field(index);
    CHECK(n00b_result_is_ok(field_r));
    CHECK(n00b_result_get(field_r) == field);

    n00b_store_advert_t same =
        n00b_store_index_advertise(index,
                                   field,
                                   N00B_STORE_INDEX_OP_UNSPECIFIED);
    CHECK(same.accelerates);
    CHECK(same.kind == N00B_STORE_INDEX_TERM);
    CHECK(same.selectivity_hint < 1.0);

    n00b_store_advert_t mismatch =
        n00b_store_index_advertise(index,
                                   other,
                                   N00B_STORE_INDEX_OP_UNSPECIFIED);
    CHECK(!mismatch.accelerates);
    CHECK(mismatch.kind == N00B_STORE_INDEX_NONE);

    n00b_store_advert_t null_ad =
        n00b_store_index_advertise(nullptr,
                                   field,
                                   N00B_STORE_INDEX_OP_UNSPECIFIED);
    CHECK(!null_ad.accelerates);
    CHECK(null_ad.kind == N00B_STORE_INDEX_NONE);

    auto null_kind = n00b_store_index_kind(nullptr);
    CHECK(n00b_result_is_err(null_kind));
    CHECK(n00b_result_get_err(null_kind) == N00B_STORE_INDEX_ERR_ARG);

    auto null_index_field = n00b_store_index_field(nullptr);
    CHECK(n00b_result_is_err(null_index_field));
    CHECK(n00b_result_get_err(null_index_field) == N00B_STORE_INDEX_ERR_ARG);

    auto dense_r = n00b_store_index_new(field,
                                        N00B_STORE_INDEX_TERM,
                                        .postings = N00B_STORE_POSTINGS_DENSE);
    CHECK(n00b_result_is_ok(dense_r));
    postings_r = n00b_store_index_postings_kind(n00b_result_get(dense_r));
    CHECK(n00b_result_is_ok(postings_r));
    CHECK(n00b_result_get(postings_r) == N00B_STORE_POSTINGS_DENSE);

    auto bad_dense_unready = n00b_store_index_new(
        field,
        N00B_STORE_INDEX_VECTOR,
        .postings = N00B_STORE_POSTINGS_DENSE);
    CHECK(n00b_result_is_err(bad_dense_unready));
    CHECK(n00b_result_get_err(bad_dense_unready) == N00B_STORE_INDEX_ERR_ARG);
}

static void
test_unimplemented_index_dispatch_contract(void)
{
    auto index_r = n00b_store_index_new(r"message", N00B_STORE_INDEX_VECTOR);
    CHECK(n00b_result_is_ok(index_r));
    n00b_store_index_t *index = n00b_result_get(index_r);

    auto kind_r = n00b_store_index_kind(index);
    CHECK(n00b_result_is_ok(kind_r));
    CHECK(n00b_result_get(kind_r) == N00B_STORE_INDEX_VECTOR);

    n00b_store_advert_t same =
        n00b_store_index_advertise(index,
                                   r"message",
                                   N00B_STORE_INDEX_OP_UNSPECIFIED);
    CHECK(!same.accelerates);
    CHECK(same.kind == N00B_STORE_INDEX_NONE);

    auto shard_r = n00b_store_shard_new(.shard_id = UINT64_C(0x5151), .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    auto append_r = n00b_store_shard_append(shard, record_with_level(r"info"));
    CHECK(n00b_result_is_ok(append_r));

    auto add_r = n00b_store_index_add(index, shard, n00b_result_get(append_r));
    CHECK(n00b_result_is_err(add_r));
    CHECK(n00b_result_get_err(add_r) == N00B_STORE_INDEX_ERR_UNREADY);

    auto hot_r = n00b_store_index_lookup(index,
                                         shard,
                                         n00b_json_string_new_from_n00b(r"info"));
    CHECK(n00b_result_is_err(hot_r));
    CHECK(n00b_result_get_err(hot_r) == N00B_STORE_INDEX_ERR_UNREADY);

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 19,
                                        .base_address = 0x7200u);
    CHECK(n00b_result_is_ok(seal_r));

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));

    auto mapped_r = n00b_store_index_lookup_mapped(
        index,
        n00b_result_get(root_r),
        n00b_json_string_new_from_n00b(r"info"));
    CHECK(n00b_result_is_err(mapped_r));
    CHECK(n00b_result_get_err(mapped_r) == N00B_STORE_INDEX_ERR_UNREADY);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

static void
test_index_add_allocator_contract(void)
{
    n00b_store_index_t *index = term_index(r"scalar");

    auto shard_r = n00b_store_shard_new(.shard_id = UINT64_C(0x5153), .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    auto append_r = n00b_store_shard_append(
        shard,
        record_with_scalar(n00b_json_string_new_from_n00b(r"arena")));
    CHECK(n00b_result_is_ok(append_r));

    n00b_arena_t     *arena = n00b_new_arena(.size   = 1 << 20,
                                             .use_gc = false,
                                             .name   = "index-add-scratch-test");
    n00b_allocator_t *alloc = (n00b_allocator_t *)arena;
    uint32_t          before = n00b_atomic_load(&arena->alloc_count);

    auto add_r = n00b_store_index_add(index,
                                      shard,
                                      n00b_result_get(append_r),
                                      .allocator = alloc);
    CHECK(n00b_result_is_ok(add_r));
    CHECK(n00b_result_get(add_r) == 1);
    CHECK(n00b_atomic_load(&arena->alloc_count) > before);

    auto hit_r = n00b_store_index_lookup(
        index,
        shard,
        n00b_json_string_new_from_n00b(r"arena"));
    CHECK(n00b_result_is_ok(hit_r));
    check_len(n00b_result_get(hit_r), 1);
}

static void
test_empty_postings_contract(void)
{
    auto postings_r = n00b_store_postings_empty(.shard_id   = 0xfeed,
                                                .generation = 7);
    CHECK(n00b_result_is_ok(postings_r));
    n00b_store_postings_t *postings = n00b_result_get(postings_r);
    CHECK(postings != nullptr);

    auto len_r = n00b_store_postings_len(postings);
    CHECK(n00b_result_is_ok(len_r));
    CHECK(n00b_result_get(len_r) == 0);

    auto get0_r = n00b_store_postings_get(postings, 0);
    CHECK(n00b_result_is_ok(get0_r));
    CHECK(!n00b_option_is_set(n00b_result_get(get0_r)));

    auto get_far_r = n00b_store_postings_get(postings, UINT64_MAX);
    CHECK(n00b_result_is_ok(get_far_r));
    CHECK(!n00b_option_is_set(n00b_result_get(get_far_r)));

    auto null_len = n00b_store_postings_len(nullptr);
    CHECK(n00b_result_is_err(null_len));
    CHECK(n00b_result_get_err(null_len) == N00B_STORE_INDEX_ERR_ARG);

    auto null_get = n00b_store_postings_get(nullptr, 0);
    CHECK(n00b_result_is_err(null_get));
    CHECK(n00b_result_get_err(null_get) == N00B_STORE_INDEX_ERR_ARG);

    auto null_pos = n00b_store_record_pos(nullptr);
    CHECK(n00b_result_is_err(null_pos));
    CHECK(n00b_result_get_err(null_pos) == N00B_STORE_INDEX_ERR_ARG);
}

static void
test_hot_term_lookup(void)
{
    n00b_store_index_t *index = term_index(r"level");
    n00b_store_shard_t *shard = indexed_level_shard(index);

    auto hit_r = n00b_store_index_lookup(index,
                                         shard,
                                         n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(hit_r));
    n00b_store_postings_t *hits = n00b_result_get(hit_r);
    check_len(hits, 2);

    n00b_store_posting_t p0 = posting_at(hits, 0);
    n00b_store_posting_t p1 = posting_at(hits, 1);
    CHECK(p0.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p1.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p0.pos.ordinal == 1);
    CHECK(p1.pos.ordinal == 2);
    CHECK(p0.pos.generation == 0);
    CHECK(p1.pos.generation == 0);
    CHECK(p0.record != nullptr);
    CHECK(p1.record != nullptr);

    auto p0_pos_r = n00b_store_record_pos(p0.record);
    CHECK(n00b_result_is_ok(p0_pos_r));
    CHECK(n00b_result_get(p0_pos_r).ordinal == 1);

    auto miss_r = n00b_store_index_lookup(index,
                                          shard,
                                          n00b_json_string_new_from_n00b(r"warn"));
    CHECK(n00b_result_is_ok(miss_r));
    check_len(n00b_result_get(miss_r), 0);

    auto bad_add = n00b_store_index_add(index, shard, 99);
    CHECK(n00b_result_is_err(bad_add));
    CHECK(n00b_result_get_err(bad_add) == N00B_STORE_INDEX_ERR_ARG);
}

static void
test_mapped_term_lookup(void)
{
    n00b_store_index_t *index = term_index(r"level");
    n00b_store_shard_t *shard = indexed_level_shard(index);

    poison_index_sync_flags(shard, r"level");

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 77,
                                        .base_address = 0x6e00u);
    CHECK(n00b_result_is_ok(seal_r));

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));
    n00b_store_map_shard_t *mapped = n00b_result_get(root_r);

    auto hot_after_seal = n00b_store_index_lookup(index,
                                                  shard,
                                                  n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_err(hot_after_seal));
    CHECK(n00b_result_get_err(hot_after_seal) == N00B_STORE_INDEX_ERR_STATE);

    auto hit_r = n00b_store_index_lookup_mapped(index,
                                                mapped,
                                                n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(hit_r));
    n00b_store_postings_t *hits = n00b_result_get(hit_r);
    check_len(hits, 2);

    n00b_store_posting_t p0 = posting_at(hits, 0);
    n00b_store_posting_t p1 = posting_at(hits, 1);
    CHECK(p0.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p1.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p0.pos.ordinal == 1);
    CHECK(p1.pos.ordinal == 2);
    CHECK(p0.pos.generation == 77);
    CHECK(p1.pos.generation == 77);
    CHECK(p0.record != nullptr);
    CHECK(p1.record != nullptr);

    auto miss_r = n00b_store_index_lookup_mapped(index,
                                                 mapped,
                                                 n00b_json_string_new_from_n00b(r"warn"));
    CHECK(n00b_result_is_ok(miss_r));
    check_len(n00b_result_get(miss_r), 0);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

static void
test_dense_term_lookup_hot_and_mapped(void)
{
    n00b_store_index_t *index = dense_term_index(r"level");
    n00b_store_shard_t *shard = indexed_level_shard(index);

    auto hot_r = n00b_store_index_lookup(index,
                                         shard,
                                         n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(hot_r));
    n00b_store_postings_t *hot = n00b_result_get(hot_r);
    check_len(hot, 2);
    CHECK(posting_at(hot, 0).pos.ordinal == 1);
    CHECK(posting_at(hot, 1).pos.ordinal == 2);

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 88,
                                        .base_address = 0x6e20u);
    CHECK(n00b_result_is_ok(seal_r));

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));

    auto mapped_r = n00b_store_index_lookup_mapped(
        term_index(r"level"),
        n00b_result_get(root_r),
        n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(mapped_r));
    n00b_store_postings_t *mapped = n00b_result_get(mapped_r);
    check_len(mapped, 2);
    CHECK(posting_at(mapped, 0).pos.ordinal == 1);
    CHECK(posting_at(mapped, 0).pos.generation == 88);
    CHECK(posting_at(mapped, 1).pos.ordinal == 2);
    CHECK(posting_at(mapped, 1).pos.generation == 88);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

static void
test_sealed_index_readback_with_fresh_descriptor(void)
{
    n00b_store_index_t *writer_index = term_index(r"level");
    n00b_store_shard_t *shard        = indexed_level_shard(writer_index);

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 177,
                                        .base_address = 0x6e10u);
    CHECK(n00b_result_is_ok(seal_r));

    auto late_add_r = n00b_store_index_add(writer_index, shard, 0);
    CHECK(n00b_result_is_err(late_add_r));
    CHECK(n00b_result_get_err(late_add_r) == N00B_STORE_INDEX_ERR_STATE);

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));
    n00b_store_map_shard_t *mapped = n00b_result_get(root_r);

    n00b_store_index_t *reader_index = term_index(r"level");
    auto hit_r = n00b_store_index_lookup_mapped(
        reader_index,
        mapped,
        n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(hit_r));
    n00b_store_postings_t *hits = n00b_result_get(hit_r);
    check_len(hits, 2);

    n00b_store_posting_t p0 = posting_at(hits, 0);
    n00b_store_posting_t p1 = posting_at(hits, 1);
    CHECK(p0.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p1.pos.shard_id == UINT64_C(0xfeedface));
    CHECK(p0.pos.ordinal == 1);
    CHECK(p1.pos.ordinal == 2);
    CHECK(p0.pos.generation == 177);
    CHECK(p1.pos.generation == 177);
    CHECK(p0.record != nullptr);
    CHECK(p1.record != nullptr);

    auto p0_pos_r = n00b_store_record_pos(p0.record);
    CHECK(n00b_result_is_ok(p0_pos_r));
    CHECK(n00b_result_get(p0_pos_r).ordinal == 1);
    CHECK(n00b_result_get(p0_pos_r).generation == 177);

    n00b_store_index_t *wrong_field = term_index(r"message");
    auto miss_r = n00b_store_index_lookup_mapped(
        wrong_field,
        mapped,
        n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(miss_r));
    check_len(n00b_result_get(miss_r), 0);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

static void
test_mapped_scalar_term_lookup(void)
{
    n00b_store_index_t *index = term_index(r"count");

    auto shard_r = n00b_store_shard_new(.shard_id = UINT64_C(0x5150), .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    auto a0 = n00b_store_shard_append(shard,
                                      record_with_count(INT64_C(0xfeedface)));
    auto a1 = n00b_store_shard_append(shard, record_with_count(7));
    CHECK(n00b_result_is_ok(a0));
    CHECK(n00b_result_is_ok(a1));

    auto i0 = n00b_store_index_add(index, shard, n00b_result_get(a0));
    auto i1 = n00b_store_index_add(index, shard, n00b_result_get(a1));
    CHECK(n00b_result_is_ok(i0));
    CHECK(n00b_result_is_ok(i1));
    CHECK(n00b_result_get(i0) == 1);
    CHECK(n00b_result_get(i1) == 1);

    auto hot_r = n00b_store_index_lookup(index,
                                         shard,
                                         n00b_json_int_new(INT64_C(0xfeedface)));
    CHECK(n00b_result_is_ok(hot_r));
    check_len(n00b_result_get(hot_r), 1);
    CHECK(posting_at(n00b_result_get(hot_r), 0).pos.ordinal == 0);

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 91,
                                        .base_address = 0x6e00u);
    CHECK(n00b_result_is_ok(seal_r));

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));

    auto mapped_r = n00b_store_index_lookup_mapped(
        index,
        n00b_result_get(root_r),
        n00b_json_int_new(INT64_C(0xfeedface)));
    CHECK(n00b_result_is_ok(mapped_r));
    n00b_store_postings_t *hits = n00b_result_get(mapped_r);
    check_len(hits, 1);

    n00b_store_posting_t p0 = posting_at(hits, 0);
    CHECK(p0.pos.shard_id == UINT64_C(0x5150));
    CHECK(p0.pos.ordinal == 0);
    CHECK(p0.pos.generation == 91);

    auto miss_r = n00b_store_index_lookup_mapped(index,
                                                 n00b_result_get(root_r),
                                                 n00b_json_int_new(9));
    CHECK(n00b_result_is_ok(miss_r));
    check_len(n00b_result_get(miss_r), 0);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

static void
test_scalar_variant_term_distinctions(void)
{
    n00b_store_index_t *index = term_index(r"scalar");

    auto shard_r = n00b_store_shard_new(.shard_id = UINT64_C(0x5152), .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    uint64_t int_ordinal =
        append_and_index_scalar(index, shard, n00b_json_int_new(1));
    uint64_t string_ordinal =
        append_and_index_scalar(index,
                                shard,
                                n00b_json_string_new_from_n00b(r"1"));
    uint64_t double_ordinal =
        append_and_index_scalar(index, shard, n00b_json_double_new(1.0));
    uint64_t bool_ordinal =
        append_and_index_scalar(index, shard, n00b_json_bool_new(true));
    uint64_t null_ordinal =
        append_and_index_scalar(index, shard, n00b_json_null_new());

    check_hot_single_hit(index,
                         shard,
                         n00b_json_int_new(1),
                         UINT64_C(0x5152),
                         int_ordinal);
    check_hot_single_hit(index,
                         shard,
                         n00b_json_string_new_from_n00b(r"1"),
                         UINT64_C(0x5152),
                         string_ordinal);
    check_hot_single_hit(index,
                         shard,
                         n00b_json_double_new(1.0),
                         UINT64_C(0x5152),
                         double_ordinal);
    check_hot_single_hit(index,
                         shard,
                         n00b_json_bool_new(true),
                         UINT64_C(0x5152),
                         bool_ordinal);
    check_hot_single_hit(index,
                         shard,
                         n00b_json_null_new(),
                         UINT64_C(0x5152),
                         null_ordinal);

    auto miss_r = n00b_store_index_lookup(index,
                                          shard,
                                          n00b_json_bool_new(false));
    CHECK(n00b_result_is_ok(miss_r));
    check_len(n00b_result_get(miss_r), 0);

    auto seal_r = n00b_store_shard_seal(shard,
                                        .seal_ts      = 123,
                                        .base_address = 0x7300u);
    CHECK(n00b_result_is_ok(seal_r));

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);

    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));
    n00b_store_map_shard_t *mapped = n00b_result_get(root_r);

    check_mapped_single_hit(index,
                            mapped,
                            n00b_json_int_new(1),
                            UINT64_C(0x5152),
                            int_ordinal,
                            123);
    check_mapped_single_hit(index,
                            mapped,
                            n00b_json_string_new_from_n00b(r"1"),
                            UINT64_C(0x5152),
                            string_ordinal,
                            123);
    check_mapped_single_hit(index,
                            mapped,
                            n00b_json_double_new(1.0),
                            UINT64_C(0x5152),
                            double_ordinal,
                            123);
    check_mapped_single_hit(index,
                            mapped,
                            n00b_json_bool_new(true),
                            UINT64_C(0x5152),
                            bool_ordinal,
                            123);
    check_mapped_single_hit(index,
                            mapped,
                            n00b_json_null_new(),
                            UINT64_C(0x5152),
                            null_ordinal,
                            123);

    auto mapped_miss_r =
        n00b_store_index_lookup_mapped(index,
                                       mapped,
                                       n00b_json_bool_new(false));
    CHECK(n00b_result_is_ok(mapped_miss_r));
    check_len(n00b_result_get(mapped_miss_r), 0);

    auto close_r = n00b_store_map_close(map);
    CHECK(n00b_result_is_ok(close_r));
}

// ============================================================================
// A posting list must never be scanned word-for-word (n00b-lang/n00b#375).
//
// Its first word packs `kind` with the `reserved` flag word, and a sealed
// sparse list carries N00B_STORE_POSTINGS_ORDERED there, so the word reads
// 0x1_0000_0000: pointer-shaped. A conservative every-word scan takes it for
// a pointer and the seal marshaller fails the shard with
// unsupported-static-pointer whenever that value falls inside a registered
// static range. n00b's own binaries were spared only because they carry the
// link-time type-map dictionary that promotes the scan to a precise one;
// downstream consumers linking without it (wax) lost every store flush.
//
// So the constructor stamps the precise shape itself. This checks the stamp
// on a real hot-shard posting list, and that a sealed image built from it
// still answers correctly (the packed word round-tripped untouched).
// ============================================================================

static n00b_store_posting_list_t *
first_posting_list_for(n00b_store_shard_t *shard, n00b_string_t *field)
{
    CHECK(shard != nullptr && shard->columns != nullptr);
    bool                 found  = false;
    n00b_store_column_t *column = n00b_dict_get(shard->columns, field, &found);
    CHECK(found && column != nullptr);
    n00b_store_posting_list_t *first = nullptr;
    n00b_dict_foreach(column, key, postings, {
        (void)key;
        if (first == nullptr && postings != nullptr
            && postings->kind == N00B_STORE_POSTINGS_SPARSE) {
            first = postings;
        }
    });
    CHECK(first != nullptr);
    return first;
}

static void
test_posting_list_is_precisely_scanned(void)
{
    n00b_store_index_t *index = term_index(r"level");
    n00b_store_shard_t *shard = indexed_level_shard(index);

    n00b_store_posting_list_t *postings = first_posting_list_for(shard, r"level");

    // The header the GC and the marshaller read from: a callback scan over
    // exactly the two pointer words, never DEFAULT/ALL.
    bool                pin = false;
    n00b_alloc_info_t   ai  = n00b_find_alloc_info(postings,
                                                   .scan_for_header = true,
                                                   .pin             = &pin);
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;
    if (ai.kind == n00b_alloc_inline) {
        CHECK(ai.hdr.in_line != nullptr);
        scan_kind = (n00b_gc_scan_kind_t)ai.hdr.in_line->scan_kind;
        scan_cb   = ai.hdr.in_line->scan_cb;
        scan_user = ai.hdr.in_line->scan_user;
    }
    else {
        CHECK(ai.kind == n00b_alloc_oob && ai.hdr.oob != nullptr);
        scan_kind = (n00b_gc_scan_kind_t)ai.hdr.oob->scan_kind;
        scan_cb   = ai.hdr.oob->scan_cb;
        scan_user = ai.hdr.oob->scan_user;
    }
    n00b_alloc_hdr_unpin(pin);
    CHECK(scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(scan_cb == n00b_gc_scan_cb_struct_field);
    CHECK(scan_user == (void *)rocs_posting_list_scan_shape());
    const n00b_gc_struct_array_t *shape = rocs_posting_list_scan_shape();
    CHECK(shape->stride == 1 && shape->offset == 2 && shape->count == 2);
    CHECK(offsetof(n00b_store_posting_list_t, ordinals) == 2 * sizeof(void *));
    CHECK(offsetof(n00b_store_posting_list_t, flags) == 3 * sizeof(void *));

    // Seal (which orders the list, setting the packed bit) and read back
    // through the mapped view: both "error" records and the one "info".
    auto seal_r = n00b_store_shard_seal(shard, .seal_ts = 23, .base_address = 0x7300u);
    CHECK(n00b_result_is_ok(seal_r));
    CHECK((n00b_atomic_load(&postings->reserved) & N00B_STORE_POSTINGS_ORDERED) != 0);

    auto open_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(open_r));
    n00b_store_map_t *map = n00b_result_get(open_r);
    auto root_r = n00b_store_map_root(map);
    CHECK(n00b_result_is_ok(root_r));

    auto errors_r = n00b_store_index_lookup_mapped(index,
                                                   n00b_result_get(root_r),
                                                   n00b_json_string_new_from_n00b(r"error"));
    CHECK(n00b_result_is_ok(errors_r));
    check_len(n00b_result_get(errors_r), 2);
    auto info_r = n00b_store_index_lookup_mapped(index,
                                                 n00b_result_get(root_r),
                                                 n00b_json_string_new_from_n00b(r"info"));
    CHECK(n00b_result_is_ok(info_r));
    check_len(n00b_result_get(info_r), 1);

    CHECK(n00b_result_is_ok(n00b_store_map_close(map)));
    printf("  [PASS] posting_list_is_precisely_scanned\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    test_public_contracts();
    test_index_descriptor_contract();
    test_unimplemented_index_dispatch_contract();
    test_index_add_allocator_contract();
    test_empty_postings_contract();
    test_hot_term_lookup();
    test_mapped_term_lookup();
    test_dense_term_lookup_hot_and_mapped();
    test_sealed_index_readback_with_fresh_descriptor();
    test_mapped_scalar_term_lookup();
    test_scalar_variant_term_distinctions();
    test_posting_list_is_precisely_scanned();
    n00b_shutdown();
    return 0;
}
