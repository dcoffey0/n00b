#include "internal/rocs/plan.h"

#include "internal/rocs/plan_ir.h"
#include "util/assert.h"
#include "internal/rocs/eval.h"

#include "core/arena.h"
#include "core/buffer.h"
#include "internal/rocs/index.h"
#include "internal/rocs/json_field.h"
#include "internal/rocs/store.h"
#include "rocs/map.h"
#include "rocs/normalizer.h"
#include "text/strings/string_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool
rocs_plan_debug_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("ROCS_QUERY_DEBUG") != nullptr ? 1 : 0;
    }
    return enabled != 0;
}

typedef n00b_list_t(n00b_string_t *) _rocs_plan_route_list_t;

typedef enum : int32_t {
    _rocs_plan_ordset_op_union,
    _rocs_plan_ordset_op_intersection,
    _rocs_plan_ordset_op_difference,
} _rocs_plan_ordset_binary_op_t;

typedef struct {
    n00b_store_partition_policy_t *policy;
    n00b_string_t                 *field;
    n00b_allocator_t              *allocator;
} _rocs_plan_prune_ctx_t;

typedef struct {
    bool                    constrained;
    _rocs_plan_route_list_t *routes;
} _rocs_plan_prune_t;

#define N00B_ROCS_PLAN_BROAD_CANDIDATE_MIN_RECORDS UINT64_C(8)
#define N00B_ROCS_PLAN_BROAD_CANDIDATE_PERCENT UINT64_C(75)

n00b_err_t
_rocs_plan_store_err(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_ERR_ARG:
        return N00B_PLAN_ERR_ARG;
    default:
        return N00B_PLAN_ERR_STATE;
    }
}

n00b_err_t
_rocs_plan_map_err(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_MAP_ERR_ARG:
        return N00B_PLAN_ERR_ARG;
    default:
        return N00B_PLAN_ERR_STATE;
    }
}

static _rocs_plan_route_list_t *
_rocs_plan_route_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    _rocs_plan_route_list_t *routes = n00b_alloc_with_opts(
        _rocs_plan_route_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    *routes = n00b_list_new_private(n00b_string_t *,
                                    .allocator = allocator,
                                    .scan_kind = N00B_GC_SCAN_KIND_ALL);
    return routes;
}

static bool
_rocs_plan_route_list_contains(_rocs_plan_route_list_t *routes,
                               n00b_string_t           *route)
{
    if (routes == nullptr || route == nullptr) {
        return false;
    }

    size_t len = n00b_list_len(*routes);
    for (size_t i = 0; i < len; i++) {
        n00b_string_t *item = n00b_list_get(*routes, i);
        if (item != nullptr && n00b_unicode_str_eq(item, route)) {
            return true;
        }
    }
    return false;
}

static n00b_result_t(bool)
_rocs_plan_route_list_append_unique(_rocs_plan_route_list_t *routes,
                                    n00b_string_t           *route)
{
    if (routes == nullptr || route == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    if (_rocs_plan_route_list_contains(routes, route)) {
        return n00b_result_ok(bool, false);
    }

    n00b_list_push(*routes, route);
    return n00b_result_ok(bool, true);
}

static _rocs_plan_route_list_t *
_rocs_plan_route_list_intersection(_rocs_plan_route_list_t *left,
                                   _rocs_plan_route_list_t *right) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    _rocs_plan_route_list_t *out =
        _rocs_plan_route_list_new(.allocator = allocator);
    if (left == nullptr || right == nullptr) {
        return out;
    }

    size_t len = n00b_list_len(*left);
    for (size_t i = 0; i < len; i++) {
        n00b_string_t *route = n00b_list_get(*left, i);
        if (_rocs_plan_route_list_contains(right, route)) {
            auto append_r = _rocs_plan_route_list_append_unique(out, route);
            if (n00b_result_is_err(append_r)) {
                return out;
            }
        }
    }
    return out;
}

static _rocs_plan_prune_t
_rocs_plan_prune_unconstrained(void)
{
    return (_rocs_plan_prune_t){
        .constrained = false,
        .routes      = nullptr,
    };
}

static n00b_result_t(_rocs_plan_prune_t)
_rocs_plan_prune_for_predicate(_rocs_plan_prune_ctx_t *ctx,
                               n00b_plan_predicate_t  *predicate);

static bool
_rocs_plan_value_is_set(n00b_plan_value_t value)
{
    return n00b_variant_is_set(value);
}

bool
_rocs_plan_path_component_is_valid(n00b_plan_path_component_t *component)
{
    if (component == nullptr) {
        return false;
    }

    switch (component->kind) {
    case N00B_PLAN_PATH_KEY:
        return component->key != nullptr;
    case N00B_PLAN_PATH_INDEX:
        return true;
    }

    return false;
}

static n00b_plan_path_component_t *
_rocs_plan_path_component_copy(n00b_plan_path_component_t *component) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_path_component_t *copy = n00b_alloc_with_opts(
        n00b_plan_path_component_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    copy->kind  = component->kind;
    copy->key   = component->key;
    copy->index = component->index;
    return copy;
}

static n00b_err_t
_rocs_plan_check_target(n00b_plan_target_t *target, n00b_plan_leaf_op_t op)
{
    if (target == nullptr) {
        return N00B_PLAN_ERR_ARG;
    }

    if (target->kind == N00B_PLAN_TARGET_FIELD) {
        if (target->field == nullptr || target->field->u8_bytes == 0) {
            return N00B_PLAN_ERR_ARG;
        }
        return N00B_PLAN_OK;
    }

    if (target->kind == N00B_PLAN_TARGET_ANY) {
        if (op == N00B_PLAN_LEAF_CONTAINS) {
            return N00B_PLAN_OK;
        }
        return N00B_PLAN_ERR_ANY_UNSUPPORTED;
    }

    return N00B_PLAN_ERR_ARG;
}

static n00b_plan_predicate_t *
_rocs_plan_predicate_new(n00b_plan_predicate_kind_t kind) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_predicate_t *predicate = n00b_alloc_with_opts(
        n00b_plan_predicate_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    predicate->kind = kind;
    return predicate;
}

static n00b_result_t(n00b_plan_predicate_t *)
_rocs_plan_leaf_new(n00b_plan_target_t *target,
                    n00b_plan_leaf_op_t op) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_err_t target_err = _rocs_plan_check_target(target, op);
    if (target_err != N00B_PLAN_OK) {
        return n00b_result_err(n00b_plan_predicate_t *, target_err);
    }

    n00b_plan_predicate_t *predicate =
        _rocs_plan_predicate_new(N00B_PLAN_PREDICATE_LEAF,
                                 .allocator = allocator);
    predicate->leaf_op = op;
    predicate->target  = target;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

static n00b_err_t
_rocs_plan_check_children(n00b_plan_predicate_list_t *children)
{
    if (children == nullptr) {
        return N00B_PLAN_ERR_ARG;
    }

    size_t len = n00b_list_len(*children);
    if (len < 2) {
        return N00B_PLAN_ERR_EMPTY;
    }

    for (size_t i = 0; i < len; i++) {
        if (n00b_list_get(*children, i) == nullptr) {
            return N00B_PLAN_ERR_ARG;
        }
    }

    return N00B_PLAN_OK;
}

static n00b_result_t(uint64_t)
_rocs_plan_ordset_word_count(uint64_t record_count)
{
    uint64_t words = record_count >> 6;
    if ((record_count & UINT64_C(63)) != 0) {
        words++;
    }
    if (words > (uint64_t)SIZE_MAX / sizeof(uint64_t)) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(uint64_t, words);
}

// Members of the last word; every word before it is fully in the universe.
static uint64_t
_rocs_plan_ordset_tail_mask(uint64_t record_count)
{
    uint64_t bits = record_count & UINT64_C(63);
    if (bits == 0) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << bits) - UINT64_C(1);
}

#ifdef N00B_DEBUG
static _Atomic(uint64_t) rocs_ordset_bitmaps_allocated = 0;
static _Atomic(uint64_t) rocs_ordset_bitmaps_freed     = 0;

uint64_t
n00b_plan_ordset_bitmaps_allocated(void)
{
    return atomic_load_explicit(&rocs_ordset_bitmaps_allocated,
                                memory_order_relaxed);
}

uint64_t
n00b_plan_ordset_bitmaps_freed(void)
{
    return atomic_load_explicit(&rocs_ordset_bitmaps_freed,
                                memory_order_relaxed);
}

void
n00b_plan_ordset_bitmaps_reset(void)
{
    atomic_store_explicit(&rocs_ordset_bitmaps_allocated,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&rocs_ordset_bitmaps_freed, 0, memory_order_relaxed);
}
#endif

static void
_rocs_plan_ordset_free_storage(n00b_allocator_t *allocator, void *ptr)
{
    if (ptr == nullptr) {
        return;
    }
    if (allocator != nullptr) {
        n00b_free(ptr, .allocator = allocator);
    }
    else {
        n00b_free(ptr);
    }
}

// Any change to membership invalidates the ascending-ordinal cache.
static void
_rocs_plan_ordset_drop_cache(n00b_plan_ordset_t *set)
{
    _rocs_plan_ordset_free_storage(set->allocator, set->ord_cache);
    set->ord_cache = nullptr;
}

static void
_rocs_plan_ordset_drop_words(n00b_plan_ordset_t *set)
{
    if (set->words != nullptr) {
#ifdef N00B_DEBUG
        atomic_fetch_add_explicit(&rocs_ordset_bitmaps_freed,
                                  1,
                                  memory_order_relaxed);
#endif
        _rocs_plan_ordset_free_storage(set->allocator, set->words);
        set->words      = nullptr;
        set->word_count = 0;
    }
    set->count = 0;
    _rocs_plan_ordset_drop_cache(set);
}

// Materialize a lazily-allocated bitmap. Empty sets carry words == nullptr:
// query plans build one ordset per (shard, disjunct) and in a whole-store
// walk nearly all of them stay empty, so eagerly mapping a record_count-sized
// buffer for each dominated the walk.
static n00b_result_t(bool)
_rocs_plan_ordset_bits_ensure(n00b_plan_ordset_t *set)
{
    if (set->words != nullptr) {
        return n00b_result_ok(bool, true);
    }

    auto words_r = _rocs_plan_ordset_word_count(set->record_count);
    if (n00b_result_is_err(words_r)) {
        return n00b_result_err(bool, n00b_result_get_err(words_r));
    }
    uint64_t words = n00b_result_get(words_r);
    if (words == 0) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    set->words = n00b_alloc_array_with_opts(
        uint64_t,
        (size_t)words,
        &(n00b_alloc_opts_t){
            .allocator = set->allocator,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });
    if (set->words == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }
    set->word_count = words;
#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_ordset_bitmaps_allocated,
                              1,
                              memory_order_relaxed);
#endif
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
_rocs_plan_ordset_check(n00b_plan_ordset_t *set)
{
    if (set == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    auto words_r = _rocs_plan_ordset_word_count(set->record_count);
    if (n00b_result_is_err(words_r)) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    // A set without a bitmap is valid only while empty: count > 0 always
    // materializes the bitmap first (see _rocs_plan_ordset_bits_ensure).
    if (set->count > set->record_count
        || (set->words == nullptr && set->count != 0)
        || (set->words != nullptr
            && set->word_count != n00b_result_get(words_r))) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    return n00b_result_ok(bool, true);
}

static bool
_rocs_plan_ordset_bit_is_set(n00b_plan_ordset_t *set, uint64_t ordinal)
{
    if (set->words == nullptr) {
        return false;
    }
    return ((set->words[ordinal >> 6] >> (ordinal & UINT64_C(63))) & 1) != 0;
}

static bool
_rocs_plan_ordset_bit_insert(n00b_plan_ordset_t *set, uint64_t ordinal)
{
    uint64_t *word = &set->words[ordinal >> 6];
    uint64_t  mask = UINT64_C(1) << (ordinal & UINT64_C(63));

    if ((*word & mask) != 0) {
        return false;
    }

    *word |= mask;
    set->count++;
    _rocs_plan_ordset_drop_cache(set);
    return true;
}

static n00b_result_t(n00b_plan_ordset_t *)
_rocs_plan_ordset_new(uint64_t record_count, bool full) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto words_r = _rocs_plan_ordset_word_count(record_count);
    if (n00b_result_is_err(words_r)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(words_r));
    }

    uint64_t words = n00b_result_get(words_r);
    n00b_plan_ordset_t *set = n00b_alloc_with_opts(
        n00b_plan_ordset_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    set->record_count = record_count;
    set->count        = 0;
    set->allocator    = allocator;
    set->ord_cache    = nullptr;
    set->words        = nullptr;
    set->word_count   = 0;

    if (full && record_count != 0) {
        auto ensure_r = _rocs_plan_ordset_bits_ensure(set);
        if (n00b_result_is_err(ensure_r)) {
            return n00b_result_err(n00b_plan_ordset_t *,
                                   n00b_result_get_err(ensure_r));
        }
        memset(set->words, 0xFF, (size_t)words * sizeof(uint64_t));
        set->words[words - 1] = _rocs_plan_ordset_tail_mask(record_count);
        set->count            = record_count;
    }

    return n00b_result_ok(n00b_plan_ordset_t *, set);
}

n00b_result_t(n00b_plan_ordset_t *)
_rocs_plan_ordset_range(uint64_t record_count, uint64_t first) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto set_r = _rocs_plan_ordset_new(record_count,
                                       first == 0,
                                       .allocator = allocator);
    if (n00b_result_is_err(set_r) || first == 0 || first >= record_count) {
        return set_r;
    }

    n00b_plan_ordset_t *set = n00b_result_get(set_r);
    auto ensure_r = _rocs_plan_ordset_bits_ensure(set);
    if (n00b_result_is_err(ensure_r)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(ensure_r));
    }

    uint64_t words = (record_count + 63) >> 6;
    uint64_t lead  = first >> 6;
    memset(set->words + lead + 1,
           0xFF,
           (size_t)(words - lead - 1) * sizeof(uint64_t));
    set->words[lead]      = UINT64_MAX << (first & UINT64_C(63));
    set->words[words - 1] &= _rocs_plan_ordset_tail_mask(record_count);
    set->count            = record_count - first;
    return n00b_result_ok(n00b_plan_ordset_t *, set);
}

// Deep copy. Callers pass a checked set; an empty source yields an empty
// (bitmap-free) copy.
static n00b_result_t(n00b_plan_ordset_t *)
_rocs_plan_ordset_clone(n00b_plan_ordset_t *set) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto out_r = _rocs_plan_ordset_new(set->record_count,
                                       false,
                                       .allocator = allocator);
    if (n00b_result_is_err(out_r)) {
        return out_r;
    }

    n00b_plan_ordset_t *out = n00b_result_get(out_r);
    if (set->count != 0) {
        auto ensure_r = _rocs_plan_ordset_bits_ensure(out);
        if (n00b_result_is_err(ensure_r)) {
            return n00b_result_err(n00b_plan_ordset_t *,
                                   n00b_result_get_err(ensure_r));
        }
        memcpy(out->words,
               set->words,
               (size_t)((set->record_count + 63) >> 6) * sizeof(uint64_t));
        out->count = set->count;
    }

    return n00b_result_ok(n00b_plan_ordset_t *, out);
}

static uint64_t
_rocs_plan_ordset_apply(uint64_t                      *out,
                        const uint64_t                *left,
                        const uint64_t                *right,
                        uint64_t                       words,
                        _rocs_plan_ordset_binary_op_t  op)
{
    uint64_t count = 0;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t v = 0;
        switch (op) {
        case _rocs_plan_ordset_op_union:
            v = left[i] | right[i];
            break;
        case _rocs_plan_ordset_op_intersection:
            v = left[i] & right[i];
            break;
        case _rocs_plan_ordset_op_difference:
            v = left[i] & ~right[i];
            break;
        }
        out[i] = v;
        count += (uint64_t)__builtin_popcountll(v);
    }
    return count;
}

static n00b_result_t(bool)
_rocs_plan_ordset_check_pair(n00b_plan_ordset_t *left,
                             n00b_plan_ordset_t *right)
{
    auto left_ok = _rocs_plan_ordset_check(left);
    if (n00b_result_is_err(left_ok)) {
        return left_ok;
    }
    auto right_ok = _rocs_plan_ordset_check(right);
    if (n00b_result_is_err(right_ok)) {
        return right_ok;
    }
    if (left->record_count != right->record_count) {
        return n00b_result_err(bool, N00B_PLAN_ERR_UNIVERSE);
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(n00b_plan_ordset_t *)
_rocs_plan_ordset_binary(n00b_plan_ordset_t             *left,
                         n00b_plan_ordset_t             *right,
                         _rocs_plan_ordset_binary_op_t    op) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto pair_ok = _rocs_plan_ordset_check_pair(left, right);
    if (n00b_result_is_err(pair_ok)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(pair_ok));
    }

    // Empty operands take the allocation-free path. A whole-store walk builds
    // one empty set per (shard, disjunct) and then ORs them together, so both
    // the operands and the combined results must stay bitmap-free or the
    // combining tree re-creates the very buffers laziness removed.
    if (left->count == 0 || right->count == 0) {
        switch (op) {
        case _rocs_plan_ordset_op_union: {
            n00b_plan_ordset_t *src = left->count == 0 ? right : left;
            if (src->count == 0) {
                break;
            }
            return _rocs_plan_ordset_clone(src, .allocator = allocator);
        }
        case _rocs_plan_ordset_op_intersection:
            break;
        case _rocs_plan_ordset_op_difference:
            if (left->count != 0) {
                return _rocs_plan_ordset_clone(left, .allocator = allocator);
            }
            break;
        }
        return _rocs_plan_ordset_new(left->record_count,
                                     false,
                                     .allocator = allocator);
    }

    auto out_r = _rocs_plan_ordset_new(left->record_count,
                                       false,
                                       .allocator = allocator);
    if (n00b_result_is_err(out_r)) {
        return out_r;
    }

    n00b_plan_ordset_t *out      = n00b_result_get(out_r);
    auto                ensure_r = _rocs_plan_ordset_bits_ensure(out);
    if (n00b_result_is_err(ensure_r)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(ensure_r));
    }
    out->count = _rocs_plan_ordset_apply(out->words,
                                         left->words,
                                         right->words,
                                         (left->record_count + 63) >> 6,
                                         op);
    if (out->count == 0) {
        _rocs_plan_ordset_drop_words(out);
    }
    return n00b_result_ok(n00b_plan_ordset_t *, out);
}

n00b_result_t(bool)
_rocs_plan_ordset_and_into(n00b_plan_ordset_t *dst, n00b_plan_ordset_t *src)
{
    auto pair_ok = _rocs_plan_ordset_check_pair(dst, src);
    if (n00b_result_is_err(pair_ok)) {
        return pair_ok;
    }
    if (dst == src || dst->count == 0) {
        return n00b_result_ok(bool, true);
    }
    if (src->count == 0) {
        _rocs_plan_ordset_drop_words(dst);
        return n00b_result_ok(bool, true);
    }

    dst->count = _rocs_plan_ordset_apply(dst->words,
                                         dst->words,
                                         src->words,
                                         (dst->record_count + 63) >> 6,
                                         _rocs_plan_ordset_op_intersection);
    _rocs_plan_ordset_drop_cache(dst);
    if (dst->count == 0) {
        _rocs_plan_ordset_drop_words(dst);
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
_rocs_plan_ordset_or_into(n00b_plan_ordset_t *dst, n00b_plan_ordset_t *src)
{
    auto pair_ok = _rocs_plan_ordset_check_pair(dst, src);
    if (n00b_result_is_err(pair_ok)) {
        return pair_ok;
    }
    if (dst == src || src->count == 0) {
        return n00b_result_ok(bool, true);
    }

    auto ensure_r = _rocs_plan_ordset_bits_ensure(dst);
    if (n00b_result_is_err(ensure_r)) {
        return ensure_r;
    }
    dst->count = _rocs_plan_ordset_apply(dst->words,
                                         dst->words,
                                         src->words,
                                         (dst->record_count + 63) >> 6,
                                         _rocs_plan_ordset_op_union);
    _rocs_plan_ordset_drop_cache(dst);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
_rocs_plan_ordset_complement_in_place(n00b_plan_ordset_t *set)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return ok;
    }
    if (set->record_count == 0) {
        return n00b_result_ok(bool, true);
    }

    auto ensure_r = _rocs_plan_ordset_bits_ensure(set);
    if (n00b_result_is_err(ensure_r)) {
        return ensure_r;
    }

    uint64_t words = (set->record_count + 63) >> 6;
    for (uint64_t i = 0; i < words; i++) {
        set->words[i] = ~set->words[i];
    }
    set->words[words - 1] &= _rocs_plan_ordset_tail_mask(set->record_count);
    set->count = set->record_count - set->count;
    _rocs_plan_ordset_drop_cache(set);
    if (set->count == 0) {
        _rocs_plan_ordset_drop_words(set);
    }
    return n00b_result_ok(bool, true);
}

bool
_rocs_plan_candidate_set_is_broad(n00b_plan_ordset_t *candidates)
{
    if (candidates == nullptr
        || candidates->record_count < N00B_ROCS_PLAN_BROAD_CANDIDATE_MIN_RECORDS
        || candidates->count == 0) {
        return false;
    }

    uint64_t whole = candidates->record_count / UINT64_C(100);
    uint64_t rem   = candidates->record_count % UINT64_C(100);
    uint64_t threshold =
        whole * N00B_ROCS_PLAN_BROAD_CANDIDATE_PERCENT
        + (rem * N00B_ROCS_PLAN_BROAD_CANDIDATE_PERCENT
           + UINT64_C(99)) / UINT64_C(100);
    if (threshold == 0) {
        threshold = 1;
    }

    return candidates->count >= threshold;
}

// Term, full-text, and n-gram selection differ only in the advertised operator
// and the accepted descriptor kind. The hint is a function of (index, field,
// op) with no value, so it cannot tell a selective literal from a common one.
n00b_store_index_t *
_rocs_plan_choose_index(n00b_plan_index_list_t *indexes,
                        n00b_string_t          *field,
                        n00b_store_index_op_t   op,
                        n00b_store_index_kind_t kind)
{
    if (indexes == nullptr || field == nullptr) {
        return nullptr;
    }

    n00b_store_index_t *best        = nullptr;
    double              best_hint   = 0.0;
    size_t              index_count = n00b_list_len(*indexes);
    for (size_t i = 0; i < index_count; i++) {
        n00b_store_index_t *index = n00b_list_get(*indexes, i);
        if (index == nullptr) {
            continue;
        }

        n00b_store_advert_t advert =
            n00b_store_index_advertise(index, field, (int64_t)op);
        if (!advert.accelerates || advert.kind != kind) {
            continue;
        }

        if (best == nullptr || advert.selectivity_hint < best_hint) {
            best      = index;
            best_hint = advert.selectivity_hint;
        }
    }

    return best;
}

static n00b_store_index_t *
_rocs_plan_choose_catch_all_index(n00b_plan_index_list_t *indexes)
{
    if (indexes == nullptr) {
        return nullptr;
    }

    size_t index_count = n00b_list_len(*indexes);
    for (size_t i = 0; i < index_count; i++) {
        n00b_store_index_t *index = n00b_list_get(*indexes, i);
        if (index == nullptr) {
            continue;
        }

        auto catch_all_r = n00b_store_index_is_catch_all(index);
        if (n00b_result_is_err(catch_all_r)
            || !n00b_result_get(catch_all_r)) {
            continue;
        }

        auto kind_r = n00b_store_index_kind(index);
        if (n00b_result_is_ok(kind_r)
            && n00b_result_get(kind_r) == N00B_STORE_INDEX_FULLTEXT) {
            return index;
        }
    }

    return nullptr;
}

// Posting entries walked into an ordinal set. An index scan reads no records,
// so a bound on n00b_plan_records_scanned alone cannot see a broad term walk
// millions of postings to build a set that rules almost nothing out.
//
// Added once per scan rather than once per entry, to keep the atomic out of a
// loop as long as the posting list. The early exits below are error paths,
// where the query fails and nothing reads this.
#ifdef N00B_DEBUG
static _Atomic(uint64_t) rocs_postings_walked = 0;

uint64_t
n00b_plan_postings_walked(void)
{
    return atomic_load_explicit(&rocs_postings_walked, memory_order_relaxed);
}

void
n00b_plan_postings_walked_reset(void)
{
    atomic_store_explicit(&rocs_postings_walked, 0, memory_order_relaxed);
}
#endif

n00b_result_t(n00b_plan_ordset_t *)
_rocs_plan_ordset_from_postings(n00b_store_postings_t *postings,
                                uint64_t               record_count) _kargs
{
    n00b_allocator_t    *allocator  = nullptr;
    n00b_plan_cancel_fn  cancel_cb  = nullptr;
    void                *cancel_ctx = nullptr;
    bool                 allow_unpublished = false;
}
{
    if (postings == nullptr) {
        return n00b_result_err(n00b_plan_ordset_t *, N00B_PLAN_ERR_ARG);
    }

    auto set_r = _rocs_plan_ordset_new(record_count,
                                      false,
                                      .allocator = allocator);
    if (n00b_result_is_err(set_r)) {
        return set_r;
    }
    n00b_plan_ordset_t *set = n00b_result_get(set_r);

    uint64_t        posting_count = 0;
    const uint64_t *ordinals      = n00b_store_postings_ordinals(postings,
                                                                 &posting_count);
#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_postings_walked,
                              posting_count,
                              memory_order_relaxed);
#endif
    for (uint64_t i = 0; i < posting_count; i++) {
        // A common term can have millions of postings. Poll on the same stride
        // the record loop uses so an abandoned query stops here too.
        if (cancel_cb != nullptr && (i & 0x3FF) == 0 && cancel_cb(cancel_ctx)) {
            return n00b_result_err(n00b_plan_ordset_t *,
                                   N00B_PLAN_ERR_CANCELED);
        }
        if (ordinals[i] >= record_count) {
            if (allow_unpublished) {
                continue;
            }
            return n00b_result_err(n00b_plan_ordset_t *,
                                   N00B_PLAN_ERR_ORDINAL);
        }

        auto insert_r = n00b_plan_ordset_insert(set, ordinals[i]);
        if (n00b_result_is_err(insert_r)) {
            return n00b_result_err(n00b_plan_ordset_t *,
                                   n00b_result_get_err(insert_r));
        }
    }

    return n00b_result_ok(n00b_plan_ordset_t *, set);
}

static n00b_result_t(n00b_json_node_t *)
_rocs_plan_ngram_query_node(n00b_string_t    *text,
                            n00b_store_index_t *index) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (text == nullptr || index == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }

    auto ngram_n_r = n00b_store_index_ngram_n(index);
    if (n00b_result_is_err(ngram_n_r)) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }

    n00b_json_node_t *query =
        n00b_json_string_new_from_n00b(text, .allocator = allocator);
    if (query == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }

    auto terms_r =
        n00b_store_normalize_text_ngrams(query,
                                         .ngram_n   = n00b_result_get(ngram_n_r),
                                         .allocator = allocator);
    if (n00b_result_is_err(terms_r)) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }
    if (n00b_list_len(*n00b_result_get(terms_r)) == 0) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_EMPTY);
    }

    return n00b_result_ok(n00b_json_node_t *, query);
}

#ifdef N00B_DEBUG
// Key sets actually built, not memo hits. Resolution is the dominant term in
// plan build, so this is what gates it: a wall-clock assertion here would move
// further under machine load than a regression would.
// Plans built, and shards folded into one. A fan-out that builds per shard
// rather than per partition shows up here as the two moving together; there
// is no other symptom, because a plan built too often is correct.
static _Atomic(uint64_t) rocs_plans_built     = 0;
static _Atomic(uint64_t) rocs_shards_collected = 0;

uint64_t
n00b_plan_plans_built(void)
{
    return atomic_load_explicit(&rocs_plans_built, memory_order_relaxed);
}

void
n00b_plan_plans_built_reset(void)
{
    atomic_store_explicit(&rocs_plans_built, 0, memory_order_relaxed);
}

uint64_t
n00b_plan_shards_collected(void)
{
    return atomic_load_explicit(&rocs_shards_collected, memory_order_relaxed);
}

void
n00b_plan_shards_collected_reset(void)
{
    atomic_store_explicit(&rocs_shards_collected, 0, memory_order_relaxed);
}

static _Atomic(uint64_t) rocs_plan_keys_resolved = 0;

uint64_t
n00b_plan_keys_resolved(void)
{
    return atomic_load_explicit(&rocs_plan_keys_resolved, memory_order_relaxed);
}

void
n00b_plan_keys_resolved_reset(void)
{
    atomic_store_explicit(&rocs_plan_keys_resolved, 0, memory_order_relaxed);
}
#endif

#ifdef N00B_DEBUG
// Forces every dedup digest in _rocs_plan_build_nary to the same value, so
// every operand of a group lands in one bucket.
//
// A collision is otherwise unreachable from a test: the digest is 64 bits over
// hashes nobody picks. The bucket walk is the part that has to be right when
// one happens, and with this on it is the part every dedup takes.
static _Atomic(bool) rocs_plan_dedup_collide = false;

void
n00b_plan_dedup_force_collision(bool on)
{
    atomic_store_explicit(&rocs_plan_dedup_collide, on, memory_order_relaxed);
}
#endif

n00b_store_index_keys_t *
n00b_plan_node_keys(n00b_plan_node_t *node)
{
    if (node == nullptr || node->kind != N00B_PLAN_NODE_INDEX_SCAN) {
        return nullptr;
    }

    n00b_store_index_keys_t *keys = atomic_load_explicit(&node->resolved,
                                                         memory_order_acquire);
    if (keys != nullptr) {
        return keys;
    }

#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_plan_keys_resolved, 1, memory_order_relaxed);
#endif
    auto keys_r = n00b_store_index_keys_new(node->index,
                                            node->key,
                                            .allocator = node->allocator);
    if (n00b_result_is_err(keys_r)) {
        // Not cached: a failure is a malformed value, which the lookup this
        // feeds reports for itself. Caching it would only save repeating work
        // for a query that is already on its way to an error.
        return nullptr;
    }
    keys = n00b_result_get(keys_r);

    // Release so a thread that acquires the pointer sees a built key set.
    n00b_store_index_keys_t *published = nullptr;
    if (atomic_compare_exchange_strong_explicit(&node->resolved,
                                                &published,
                                                keys,
                                                memory_order_release,
                                                memory_order_acquire)) {
        return keys;
    }
    // Lost the race. Both sides resolved the same (index, key) to an equal key
    // set, so either answers correctly; taking the published one keeps every
    // reader on a single object and leaves this one to be collected.
    return published;
}

n00b_result_t(uint64_t)
_rocs_plan_hot_record_count(n00b_store_shard_t *shard)
{
    if (shard == nullptr) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }
    if (shard->records == nullptr || shard->state != N00B_SHARD_STATE_OPEN) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_STATE);
    }

    uint64_t records_len = n00b_store_shard_records_len(shard);
    if (records_len != shard->record_count) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_STATE);
    }

    return n00b_result_ok(uint64_t, records_len);
}

n00b_result_t(uint64_t)
_rocs_plan_mapped_record_count(n00b_store_map_shard_t *shard)
{
    if (shard == nullptr) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }

    auto state_r = n00b_store_map_shard_state(shard);
    if (n00b_result_is_err(state_r)) {
        n00b_err_t err = n00b_result_get_err(state_r);
        return n00b_result_err(
            uint64_t,
            err == N00B_STORE_MAP_ERR_ARG ? N00B_PLAN_ERR_ARG
                                          : N00B_PLAN_ERR_STATE);
    }
    if (n00b_result_get(state_r) != N00B_SHARD_STATE_SEALED) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_STATE);
    }

    auto len_r = n00b_store_map_shard_records_len(shard);
    if (n00b_result_is_err(len_r)) {
        n00b_err_t err = n00b_result_get_err(len_r);
        return n00b_result_err(
            uint64_t,
            err == N00B_STORE_MAP_ERR_ARG ? N00B_PLAN_ERR_ARG
                                          : N00B_PLAN_ERR_STATE);
    }

    return n00b_result_ok(uint64_t, n00b_result_get(len_r));
}

n00b_err_t
_rocs_plan_index_err(n00b_err_t err)
{
    switch (err) {
    case N00B_STORE_INDEX_ERR_ARG:
        return N00B_PLAN_ERR_ARG;
    default:
        return N00B_PLAN_ERR_STATE;
    }
}

n00b_result_t(n00b_json_node_t *)
_rocs_plan_value_node(n00b_plan_value_t value)
{
    if (!_rocs_plan_value_is_set(value)
        || !n00b_variant_is_type(value, n00b_json_node_t *)) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }

    n00b_json_node_t *node = n00b_variant_get(value, n00b_json_node_t *);
    if (node == nullptr) {
        return n00b_result_err(n00b_json_node_t *, N00B_PLAN_ERR_STATE);
    }

    return n00b_result_ok(n00b_json_node_t *, node);
}

// Scalar comparison and deep equality over JSON values.
//
// Three callers need the same answers and must not disagree: the interpreter
// testing a record against a range or an equality, the rewriter deciding
// whether two bounds conflict, and the zone map deciding whether a shard's
// [min, max] can hold a value. A second implementation of either would make
// "cost may change, answers may not" (plan.h, rule 3) something nothing
// enforces.

bool
_rocs_plan_json_numeric(n00b_json_node_t *node, double *out)
{
    if (node == nullptr || out == nullptr) {
        return false;
    }
    if (n00b_json_is_int(node)) {
        *out = (double)n00b_json_as_i64(node);
        return true;
    }
    if (n00b_json_is_double(node)) {
        double value = n00b_json_as_f64(node);
        if (value != value) {
            return false;
        }
        *out = value;
        return true;
    }
    return false;
}

int32_t
_rocs_plan_cmp_int_double(int64_t i, double d)
{
    // Both ends of the int64 range are exact doubles, so a `d` outside it
    // orders against every int without a conversion.
    if (d >= 0x1p63) {
        return -1;
    }
    if (d < -0x1p63) {
        return 1;
    }
    // In range, so the truncation is defined, and exact because it is a
    // double's own integer part.
    int64_t whole = (int64_t)d;
    if (i != whole) {
        return i < whole ? -1 : 1;
    }
    double frac = d - (double)whole;
    return frac > 0 ? -1 : (frac < 0 ? 1 : 0);
}

n00b_result_t(bool)
_rocs_plan_json_order_cmp(n00b_json_node_t *value,
                          n00b_json_node_t *bound,
                          int32_t          *cmp)
{
    if (value == nullptr || bound == nullptr || cmp == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    bool value_int = n00b_json_is_int(value);
    bool bound_int = n00b_json_is_int(bound);
    if (value_int && bound_int) {
        int64_t l = n00b_json_as_i64(value);
        int64_t r = n00b_json_as_i64(bound);
        *cmp = l < r ? -1 : (l > r ? 1 : 0);
        return n00b_result_ok(bool, true);
    }

    double lv = 0.0;
    double rv = 0.0;
    if (_rocs_plan_json_numeric(value, &lv)
        && _rocs_plan_json_numeric(bound, &rv)) {
        if (value_int) {
            *cmp = _rocs_plan_cmp_int_double(n00b_json_as_i64(value), rv);
        }
        else if (bound_int) {
            *cmp = -_rocs_plan_cmp_int_double(n00b_json_as_i64(bound), lv);
        }
        else {
            *cmp = lv < rv ? -1 : (lv > rv ? 1 : 0);
        }
        return n00b_result_ok(bool, true);
    }

    if (n00b_json_is_string(value) && n00b_json_is_string(bound)) {
        n00b_string_t *l = n00b_json_as_string(value);
        n00b_string_t *r = n00b_json_as_string(bound);
        if (l == nullptr || r == nullptr) {
            return n00b_result_ok(bool, false);
        }
        int raw = n00b_unicode_str_cmp(l, r);
        *cmp = raw < 0 ? -1 : (raw > 0 ? 1 : 0);
        return n00b_result_ok(bool, true);
    }

    return n00b_result_ok(bool, false);
}

n00b_result_t(bool)
_rocs_plan_json_equal(n00b_allocator_t *allocator,
                      n00b_json_node_t *left,
                      n00b_json_node_t *right)
{
    if (left == nullptr || right == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    n00b_json_type_t left_type  = n00b_json_type(left);
    n00b_json_type_t right_type = n00b_json_type(right);
    if (left_type != right_type) {
        return n00b_result_ok(bool, false);
    }

    switch (left_type) {
    case N00B_JSON_NULL:
        return n00b_result_ok(bool, true);
    case N00B_JSON_BOOL:
        return n00b_result_ok(bool,
                              n00b_json_as_bool(left)
                                  == n00b_json_as_bool(right));
    case N00B_JSON_INT:
        return n00b_result_ok(bool,
                              n00b_json_as_i64(left)
                                  == n00b_json_as_i64(right));
    case N00B_JSON_DOUBLE: {
        double l = n00b_json_as_f64(left);
        double r = n00b_json_as_f64(right);
        return n00b_result_ok(bool, l == r);
    }
    case N00B_JSON_STRING: {
        n00b_string_t *l = n00b_json_as_string(left);
        n00b_string_t *r = n00b_json_as_string(right);
        return n00b_result_ok(bool,
                              l != nullptr && r != nullptr
                                  && n00b_unicode_str_eq(l, r));
    }
    case N00B_JSON_ARRAY: {
        size_t len = n00b_json_array_len(left);
        if (len != n00b_json_array_len(right)) {
            return n00b_result_ok(bool, false);
        }
        for (size_t i = 0; i < len; i++) {
            auto item_r =
                _rocs_plan_json_equal(allocator,
                                      n00b_json_array_get(left, i),
                                      n00b_json_array_get(right, i));
            if (n00b_result_is_err(item_r) || !n00b_result_get(item_r)) {
                return item_r;
            }
        }
        return n00b_result_ok(bool, true);
    }
    case N00B_JSON_OBJECT: {
        if (n00b_json_length(left) != n00b_json_length(right)) {
            return n00b_result_ok(bool, false);
        }

        auto entries_r =
            n00b_json_object_entries(left, .allocator = allocator);
        if (n00b_result_is_err(entries_r)) {
            return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
        }

        n00b_json_object_entry_list_t *entries = n00b_result_get(entries_r);
        size_t                         len     = n00b_list_len(*entries);
        for (size_t i = 0; i < len; i++) {
            n00b_json_object_entry_t *entry = n00b_list_get(*entries, i);
            if (entry == nullptr || entry->key == nullptr
                || entry->value == nullptr) {
                return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
            }

            n00b_json_node_t *other =
                n00b_json_object_get(right, entry->key);
            if (other == nullptr) {
                return n00b_result_ok(bool, false);
            }

            auto item_r = _rocs_plan_json_equal(allocator, entry->value, other);
            if (n00b_result_is_err(item_r) || !n00b_result_get(item_r)) {
                return item_r;
            }
        }

        return n00b_result_ok(bool, true);
    }
    }

    return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
}

static n00b_result_t(n00b_string_t *)
_rocs_plan_partition_route_for_value(_rocs_plan_prune_ctx_t *ctx,
                                     n00b_plan_value_t       value)
{
    if (ctx == nullptr || ctx->policy == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_PLAN_ERR_ARG);
    }

    auto value_r = _rocs_plan_value_node(value);
    if (n00b_result_is_err(value_r)) {
        return n00b_result_err(n00b_string_t *, n00b_result_get_err(value_r));
    }

    auto route_r = n00b_store_partition_route_value_for_plan(
        ctx->policy,
        n00b_result_get(value_r),
        .allocator = ctx->allocator);
    if (n00b_result_is_err(route_r)) {
        return n00b_result_err(n00b_string_t *,
                               _rocs_plan_store_err(
                                   n00b_result_get_err(route_r)));
    }
    return route_r;
}

static n00b_result_t(_rocs_plan_prune_t)
_rocs_plan_prune_for_leaf(_rocs_plan_prune_ctx_t *ctx,
                          n00b_plan_predicate_t  *predicate)
{
    if (ctx == nullptr || predicate == nullptr) {
        return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_ARG);
    }
    if (ctx->field == nullptr || predicate->target == nullptr
        || predicate->target->kind != N00B_PLAN_TARGET_FIELD
        || predicate->target->field == nullptr
        || !n00b_unicode_str_eq(predicate->target->field, ctx->field)) {
        return n00b_result_ok(_rocs_plan_prune_t,
                              _rocs_plan_prune_unconstrained());
    }

    // Under ingest-clock time partitioning the shard's partition key reflects
    // arrival time, not this field, so it cannot soundly prune an event-time
    // predicate (matching events may live in any arrival bucket).  Leave the
    // query unconstrained at the partition layer; the field index still filters,
    // and per-shard event-ts range pruning is the planned fast-follow.
    auto kind_r = n00b_store_partition_policy_get_kind(ctx->policy);
    if (n00b_result_is_ok(kind_r)
        && n00b_result_get(kind_r) == N00B_STORE_PARTITION_TIME) {
        auto src_r = n00b_store_partition_policy_get_time_source(ctx->policy);
        if (n00b_result_is_ok(src_r)
            && n00b_result_get(src_r)
                   == N00B_STORE_TIME_SOURCE_INGEST_CLOCK) {
            return n00b_result_ok(_rocs_plan_prune_t,
                                  _rocs_plan_prune_unconstrained());
        }
    }

    _rocs_plan_route_list_t *routes =
        _rocs_plan_route_list_new(.allocator = ctx->allocator);

    switch (predicate->leaf_op) {
    case N00B_PLAN_LEAF_EQ: {
        auto route_r = _rocs_plan_partition_route_for_value(ctx,
                                                            predicate->value);
        if (n00b_result_is_err(route_r)) {
            return n00b_result_err(_rocs_plan_prune_t,
                                   n00b_result_get_err(route_r));
        }
        auto append_r =
            _rocs_plan_route_list_append_unique(routes, n00b_result_get(route_r));
        if (n00b_result_is_err(append_r)) {
            return n00b_result_err(_rocs_plan_prune_t,
                                   n00b_result_get_err(append_r));
        }
        return n00b_result_ok(
            _rocs_plan_prune_t,
            ((_rocs_plan_prune_t){
                .constrained = true,
                .routes      = routes,
            }));
    }

    case N00B_PLAN_LEAF_IN: {
        if (predicate->values == nullptr) {
            return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_STATE);
        }
        size_t len = n00b_list_len(*predicate->values);
        if (len == 0) {
            return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_STATE);
        }
        for (size_t i = 0; i < len; i++) {
            auto route_r = _rocs_plan_partition_route_for_value(
                ctx,
                n00b_list_get(*predicate->values, i));
            if (n00b_result_is_err(route_r)) {
                return n00b_result_err(_rocs_plan_prune_t,
                                       n00b_result_get_err(route_r));
            }
            auto append_r = _rocs_plan_route_list_append_unique(
                routes,
                n00b_result_get(route_r));
            if (n00b_result_is_err(append_r)) {
                return n00b_result_err(_rocs_plan_prune_t,
                                       n00b_result_get_err(append_r));
            }
        }
        return n00b_result_ok(
            _rocs_plan_prune_t,
            ((_rocs_plan_prune_t){
                .constrained = true,
                .routes      = routes,
            }));
    }

    case N00B_PLAN_LEAF_RANGE:
    case N00B_PLAN_LEAF_EXISTS:
    case N00B_PLAN_LEAF_CONTAINS:
    case N00B_PLAN_LEAF_PREFIX:
    case N00B_PLAN_LEAF_SUBSTRING:
    case N00B_PLAN_LEAF_REGEX:
    case N00B_PLAN_LEAF_UNDER:
        return n00b_result_ok(_rocs_plan_prune_t,
                              _rocs_plan_prune_unconstrained());
    }

    return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_STATE);
}

static n00b_result_t(_rocs_plan_prune_t)
_rocs_plan_prune_for_and(_rocs_plan_prune_ctx_t *ctx,
                         n00b_plan_predicate_t  *predicate)
{
    if (ctx == nullptr || predicate == nullptr || predicate->children == nullptr) {
        return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_ARG);
    }

    size_t len = n00b_list_len(*predicate->children);
    if (len < 2) {
        return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_STATE);
    }

    _rocs_plan_prune_t out = _rocs_plan_prune_unconstrained();
    for (size_t i = 0; i < len; i++) {
        auto child_r =
            _rocs_plan_prune_for_predicate(ctx,
                                           n00b_list_get(*predicate->children,
                                                         i));
        if (n00b_result_is_err(child_r)) {
            return child_r;
        }

        _rocs_plan_prune_t child = n00b_result_get(child_r);
        if (!child.constrained) {
            continue;
        }
        if (!out.constrained) {
            out = child;
            continue;
        }

        out.routes = _rocs_plan_route_list_intersection(
            out.routes,
            child.routes,
            .allocator = ctx->allocator);
    }

    return n00b_result_ok(_rocs_plan_prune_t, out);
}

static n00b_result_t(_rocs_plan_prune_t)
_rocs_plan_prune_for_predicate(_rocs_plan_prune_ctx_t *ctx,
                               n00b_plan_predicate_t  *predicate)
{
    if (ctx == nullptr || predicate == nullptr) {
        return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind == N00B_PLAN_PREDICATE_FALSE) {
        return n00b_result_ok(
            _rocs_plan_prune_t,
            ((_rocs_plan_prune_t){
                .constrained = true,
                .routes      = _rocs_plan_route_list_new(
                    .allocator = ctx->allocator),
            }));
    }
    if (ctx->field == nullptr) {
        return n00b_result_ok(_rocs_plan_prune_t,
                              _rocs_plan_prune_unconstrained());
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_LEAF:
        return _rocs_plan_prune_for_leaf(ctx, predicate);
    case N00B_PLAN_PREDICATE_AND:
        return _rocs_plan_prune_for_and(ctx, predicate);
    case N00B_PLAN_PREDICATE_FALSE:
        return n00b_result_ok(
            _rocs_plan_prune_t,
            ((_rocs_plan_prune_t){
                .constrained = true,
                .routes      = _rocs_plan_route_list_new(
                    .allocator = ctx->allocator),
            }));
    case N00B_PLAN_PREDICATE_OR:
    case N00B_PLAN_PREDICATE_NOT:
    case N00B_PLAN_PREDICATE_TRUE:
        return n00b_result_ok(_rocs_plan_prune_t,
                              _rocs_plan_prune_unconstrained());
    }

    return n00b_result_err(_rocs_plan_prune_t, N00B_PLAN_ERR_STATE);
}

static n00b_result_t(bool)
_rocs_plan_partition_may_match(_rocs_plan_prune_t  prune,
                               n00b_string_t      *partition_key)
{
    if (!prune.constrained) {
        return n00b_result_ok(bool, true);
    }
    if (prune.routes == nullptr || partition_key == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    return n00b_result_ok(
        bool,
        _rocs_plan_route_list_contains(prune.routes, partition_key));
}

static n00b_result_t(n00b_plan_predicate_t *)
_rocs_plan_bool_new(n00b_plan_predicate_kind_t  kind,
                    n00b_plan_predicate_list_t *children) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_err_t child_err = _rocs_plan_check_children(children);
    if (child_err != N00B_PLAN_OK) {
        return n00b_result_err(n00b_plan_predicate_t *, child_err);
    }

    n00b_plan_predicate_t *predicate =
        _rocs_plan_predicate_new(kind, .allocator = allocator);
    predicate->children = children;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

static n00b_plan_path_component_list_t *
_rocs_plan_path_component_list_copy(
    n00b_plan_path_component_list_t *components) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_path_component_list_t *copy =
        n00b_plan_path_component_list_new(.allocator = allocator);

    size_t len = n00b_list_len(*components);
    for (size_t i = 0; i < len; i++) {
        n00b_plan_path_component_t *component =
            n00b_list_get(*components, i);
        n00b_plan_path_component_t *component_copy =
            _rocs_plan_path_component_copy(component,
                                           .allocator = allocator);
        n00b_list_push(*copy, component_copy);
    }

    return copy;
}

n00b_string_t *
n00b_plan_err_str(n00b_err_t err)
{
    switch ((n00b_plan_err_t)err) {
    case N00B_PLAN_OK:
        return r"N00B_PLAN_OK";
    case N00B_PLAN_ERR_ARG:
        return r"N00B_PLAN_ERR_ARG";
    case N00B_PLAN_ERR_STATE:
        return r"N00B_PLAN_ERR_STATE";
    case N00B_PLAN_ERR_EMPTY:
        return r"N00B_PLAN_ERR_EMPTY";
    case N00B_PLAN_ERR_ANY_UNSUPPORTED:
        return r"N00B_PLAN_ERR_ANY_UNSUPPORTED";
    case N00B_PLAN_ERR_ORDINAL:
        return r"N00B_PLAN_ERR_ORDINAL";
    case N00B_PLAN_ERR_UNIVERSE:
        return r"N00B_PLAN_ERR_UNIVERSE";
    case N00B_PLAN_ERR_CANCELED:
        return r"N00B_PLAN_ERR_CANCELED";
    }

    return r"N00B_PLAN_ERR_UNKNOWN";
}

n00b_result_t(n00b_plan_target_t *)
n00b_plan_target_field(n00b_string_t *field) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (field == nullptr || field->u8_bytes == 0) {
        return n00b_result_err(n00b_plan_target_t *, N00B_PLAN_ERR_ARG);
    }

    n00b_plan_target_t *target = n00b_alloc_with_opts(
        n00b_plan_target_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    target->kind  = N00B_PLAN_TARGET_FIELD;
    target->field = field;
    return n00b_result_ok(n00b_plan_target_t *, target);
}

n00b_result_t(n00b_plan_target_t *)
n00b_plan_target_any() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_target_t *target = n00b_alloc_with_opts(
        n00b_plan_target_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    target->kind = N00B_PLAN_TARGET_ANY;
    return n00b_result_ok(n00b_plan_target_t *, target);
}

n00b_result_t(n00b_plan_target_kind_t)
n00b_plan_target_kind(n00b_plan_target_t *target)
{
    if (target == nullptr) {
        return n00b_result_err(n00b_plan_target_kind_t, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(n00b_plan_target_kind_t, target->kind);
}

n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_plan_target_field_name(n00b_plan_target_t *target)
{
    if (target == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_string_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (target->kind != N00B_PLAN_TARGET_FIELD) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_none(n00b_string_t *));
    }
    return n00b_result_ok(n00b_option_t(n00b_string_t *),
                          n00b_option_set(n00b_string_t *, target->field));
}

n00b_plan_predicate_list_t *
n00b_plan_predicate_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_predicate_list_t *list = n00b_alloc_with_opts(
        n00b_plan_predicate_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    *list = n00b_list_new_private(n00b_plan_predicate_t *,
                                  .allocator = allocator,
                                  .scan_kind = N00B_GC_SCAN_KIND_ALL);
    return list;
}

n00b_result_t(bool)
n00b_plan_predicate_list_append(n00b_plan_predicate_list_t *list,
                                n00b_plan_predicate_t      *child)
{
    if (list == nullptr || child == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    n00b_list_push(*list, child);
    return n00b_result_ok(bool, true);
}

n00b_plan_index_list_t *
n00b_plan_index_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_index_list_t *list = n00b_alloc_with_opts(
        n00b_plan_index_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    *list = n00b_list_new_private(n00b_store_index_t *,
                                  .allocator = allocator,
                                  .scan_kind = N00B_GC_SCAN_KIND_ALL);
    return list;
}

n00b_result_t(bool)
n00b_plan_index_list_append(n00b_plan_index_list_t *list,
                            n00b_store_index_t     *index)
{
    if (list == nullptr || index == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    n00b_list_push(*list, index);
    return n00b_result_ok(bool, true);
}

n00b_plan_value_list_t *
n00b_plan_value_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_value_list_t *list = n00b_alloc_with_opts(
        n00b_plan_value_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    *list = n00b_list_new_private(n00b_plan_value_t,
                                  .allocator = allocator);
    return list;
}

n00b_result_t(bool)
n00b_plan_value_list_append(n00b_plan_value_list_t *list,
                            n00b_plan_value_t      value)
{
    if (list == nullptr || !_rocs_plan_value_is_set(value)) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    n00b_list_push(*list, value);
    return n00b_result_ok(bool, true);
}

n00b_plan_path_component_list_t *
n00b_plan_path_component_list_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plan_path_component_list_t *list = n00b_alloc_with_opts(
        n00b_plan_path_component_list_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });

    *list = n00b_list_new_private(n00b_plan_path_component_t *,
                                  .allocator = allocator);
    return list;
}

n00b_result_t(bool)
n00b_plan_path_component_list_append_key(
    n00b_plan_path_component_list_t *list,
    n00b_string_t                   *key) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (list == nullptr || key == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    n00b_plan_path_component_t *component = n00b_alloc_with_opts(
        n00b_plan_path_component_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    component->kind = N00B_PLAN_PATH_KEY;
    component->key  = key;
    n00b_list_push(*list, component);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_plan_path_component_list_append_index(
    n00b_plan_path_component_list_t *list,
    uint64_t                         index) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (list == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    n00b_plan_path_component_t *component = n00b_alloc_with_opts(
        n00b_plan_path_component_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    component->kind  = N00B_PLAN_PATH_INDEX;
    component->index = index;
    n00b_list_push(*list, component);
    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_plan_path_t *)
n00b_plan_path_new(n00b_plan_path_component_list_t *components) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (components == nullptr) {
        return n00b_result_err(n00b_plan_path_t *, N00B_PLAN_ERR_ARG);
    }

    size_t len = n00b_list_len(*components);
    for (size_t i = 0; i < len; i++) {
        if (!_rocs_plan_path_component_is_valid(
                n00b_list_get(*components, i))) {
            return n00b_result_err(n00b_plan_path_t *, N00B_PLAN_ERR_ARG);
        }
    }

    n00b_plan_path_t *path = n00b_alloc_with_opts(
        n00b_plan_path_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    path->components =
        _rocs_plan_path_component_list_copy(components,
                                           .allocator = allocator);
    return n00b_result_ok(n00b_plan_path_t *, path);
}

n00b_result_t(uint64_t)
n00b_plan_path_component_count(n00b_plan_path_t *path)
{
    if (path == nullptr || path->components == nullptr) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(uint64_t,
                          (uint64_t)n00b_list_len(*path->components));
}

n00b_result_t(n00b_option_t(n00b_plan_path_component_t *))
n00b_plan_path_component_at(n00b_plan_path_t *path, uint64_t ordinal)
{
    if (path == nullptr || path->components == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_path_component_t *),
                               N00B_PLAN_ERR_ARG);
    }

    uint64_t len = (uint64_t)n00b_list_len(*path->components);
    if (ordinal >= len) {
        return n00b_result_ok(n00b_option_t(n00b_plan_path_component_t *),
                              n00b_option_none(n00b_plan_path_component_t *));
    }

    return n00b_result_ok(
        n00b_option_t(n00b_plan_path_component_t *),
        n00b_option_set(n00b_plan_path_component_t *,
                        n00b_list_get(*path->components, (size_t)ordinal)));
}

n00b_result_t(n00b_plan_path_component_kind_t)
n00b_plan_path_component_kind(n00b_plan_path_component_t *component)
{
    if (!_rocs_plan_path_component_is_valid(component)) {
        return n00b_result_err(n00b_plan_path_component_kind_t,
                               N00B_PLAN_ERR_ARG);
    }

    return n00b_result_ok(n00b_plan_path_component_kind_t, component->kind);
}

n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_plan_path_component_key(n00b_plan_path_component_t *component)
{
    if (!_rocs_plan_path_component_is_valid(component)) {
        return n00b_result_err(n00b_option_t(n00b_string_t *),
                               N00B_PLAN_ERR_ARG);
    }

    if (component->kind != N00B_PLAN_PATH_KEY) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_none(n00b_string_t *));
    }

    return n00b_result_ok(n00b_option_t(n00b_string_t *),
                          n00b_option_set(n00b_string_t *, component->key));
}

n00b_result_t(n00b_option_t(uint64_t))
n00b_plan_path_component_index(n00b_plan_path_component_t *component)
{
    if (!_rocs_plan_path_component_is_valid(component)) {
        return n00b_result_err(n00b_option_t(uint64_t), N00B_PLAN_ERR_ARG);
    }

    if (component->kind != N00B_PLAN_PATH_INDEX) {
        return n00b_result_ok(n00b_option_t(uint64_t),
                              n00b_option_none(uint64_t));
    }

    return n00b_result_ok(n00b_option_t(uint64_t),
                          n00b_option_set(uint64_t, component->index));
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_empty(uint64_t record_count) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_ordset_new(record_count,
                                false,
                                .allocator = allocator);
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_full(uint64_t record_count) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_ordset_new(record_count, true, .allocator = allocator);
}

n00b_result_t(uint64_t)
n00b_plan_ordset_record_count(n00b_plan_ordset_t *set)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(ok));
    }
    return n00b_result_ok(uint64_t, set->record_count);
}

n00b_result_t(uint64_t)
n00b_plan_ordset_count(n00b_plan_ordset_t *set)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(ok));
    }
    return n00b_result_ok(uint64_t, set->count);
}

n00b_result_t(bool)
n00b_plan_ordset_insert(n00b_plan_ordset_t *set, uint64_t ordinal)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(bool, n00b_result_get_err(ok));
    }
    if (ordinal >= set->record_count) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ORDINAL);
    }

    auto ensure_r = _rocs_plan_ordset_bits_ensure(set);
    if (n00b_result_is_err(ensure_r)) {
        return n00b_result_err(bool, n00b_result_get_err(ensure_r));
    }

    return n00b_result_ok(bool, _rocs_plan_ordset_bit_insert(set, ordinal));
}

void
n00b_plan_ordset_free(n00b_plan_ordset_t *set)
{
    if (set == nullptr) {
        return;
    }
    _rocs_plan_ordset_drop_words(set);
    _rocs_plan_ordset_free_storage(set->allocator, set);
}

n00b_result_t(bool)
n00b_plan_ordset_contains(n00b_plan_ordset_t *set, uint64_t ordinal)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(bool, n00b_result_get_err(ok));
    }
    if (ordinal >= set->record_count) {
        return n00b_result_ok(bool, false);
    }

    return n00b_result_ok(bool, _rocs_plan_ordset_bit_is_set(set, ordinal));
}

// Word `w` of the set. An empty set has no words and reads as all zero.
static inline uint64_t
_rocs_plan_ordset_word(n00b_plan_ordset_t *set, uint64_t w)
{
    return set->words == nullptr ? 0 : set->words[w];
}

uint64_t
_rocs_plan_ordset_next_set(n00b_plan_ordset_t *set, uint64_t from)
{
    if (set == nullptr || set->count == 0 || from >= set->record_count) {
        return UINT64_MAX;
    }
    uint64_t words = (set->record_count + 63) >> 6;
    uint64_t w     = from >> 6;
    uint64_t word  = _rocs_plan_ordset_word(set, w)
                  & (~UINT64_C(0) << (from & 63));
    for (;;) {
        if (word != 0) {
            uint64_t ordinal = (w << 6) + (uint64_t)__builtin_ctzll(word);
            return ordinal < set->record_count ? ordinal : UINT64_MAX;
        }
        if (++w >= words) {
            return UINT64_MAX;
        }
        word = _rocs_plan_ordset_word(set, w);
    }
}

uint64_t
_rocs_plan_ordset_prev_set(n00b_plan_ordset_t *set, uint64_t from)
{
    if (set == nullptr || set->count == 0 || set->record_count == 0) {
        return UINT64_MAX;
    }
    if (from >= set->record_count) {
        from = set->record_count - 1;
    }
    uint64_t w     = from >> 6;
    uint64_t shift = 63 - (from & 63);
    uint64_t word  = _rocs_plan_ordset_word(set, w) & (~UINT64_C(0) >> shift);
    for (;;) {
        if (word != 0) {
            return (w << 6) + 63 - (uint64_t)__builtin_clzll(word);
        }
        if (w == 0) {
            return UINT64_MAX;
        }
        word = _rocs_plan_ordset_word(set, --w);
    }
}

uint64_t
_rocs_plan_ordset_count_range(n00b_plan_ordset_t *set,
                              uint64_t            first,
                              uint64_t            end)
{
    if (set == nullptr || set->count == 0) {
        return 0;
    }
    if (end > set->record_count) {
        end = set->record_count;
    }
    if (first >= end) {
        return 0;
    }
    if (first == 0 && end == set->record_count) {
        return set->count;
    }

    uint64_t first_w = first >> 6;
    uint64_t last_w  = (end - 1) >> 6;
    uint64_t total   = 0;
    for (uint64_t w = first_w; w <= last_w; w++) {
        uint64_t word = _rocs_plan_ordset_word(set, w);
        if (w == first_w) {
            word &= ~UINT64_C(0) << (first & 63);
        }
        if (w == last_w) {
            word &= ~UINT64_C(0) >> (63 - ((end - 1) & 63));
        }
        total += (uint64_t)__builtin_popcountll(word);
    }
    return total;
}

n00b_result_t(n00b_option_t(uint64_t))
n00b_plan_ordset_next(n00b_plan_ordset_t *set, uint64_t from)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(n00b_option_t(uint64_t),
                               n00b_result_get_err(ok));
    }
    uint64_t ordinal = _rocs_plan_ordset_next_set(set, from);
    if (ordinal == UINT64_MAX) {
        return n00b_result_ok(n00b_option_t(uint64_t),
                              n00b_option_none(uint64_t));
    }
    return n00b_result_ok(n00b_option_t(uint64_t),
                          n00b_option_set(uint64_t, ordinal));
}

n00b_result_t(n00b_option_t(uint64_t))
n00b_plan_ordset_prev(n00b_plan_ordset_t *set, uint64_t from)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(n00b_option_t(uint64_t),
                               n00b_result_get_err(ok));
    }
    uint64_t ordinal = _rocs_plan_ordset_prev_set(set, from);
    if (ordinal == UINT64_MAX) {
        return n00b_result_ok(n00b_option_t(uint64_t),
                              n00b_option_none(uint64_t));
    }
    return n00b_result_ok(n00b_option_t(uint64_t),
                          n00b_option_set(uint64_t, ordinal));
}

n00b_result_t(uint64_t)
n00b_plan_ordset_count_range(n00b_plan_ordset_t *set,
                             uint64_t            first,
                             uint64_t            end)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(uint64_t, n00b_result_get_err(ok));
    }
    return n00b_result_ok(uint64_t,
                          _rocs_plan_ordset_count_range(set, first, end));
}

n00b_result_t(n00b_option_t(uint64_t))
n00b_plan_ordset_at(n00b_plan_ordset_t *set, uint64_t index)
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(n00b_option_t(uint64_t),
                               n00b_result_get_err(ok));
    }
    if (index >= set->count) {
        return n00b_result_ok(n00b_option_t(uint64_t),
                              n00b_option_none(uint64_t));
    }

    // Build the ascending-ordinal cache once (one pass over the words,
    // skipping empty ones), then serve every at() from it in O(1). This turns
    // an ordset_at(0..count-1) iteration from O(count * record_count) into
    // O(record_count / 64 + count).
    if (set->ord_cache == nullptr && set->count != 0) {
        uint64_t *cache = n00b_alloc_array_with_opts(
            uint64_t,
            set->count,
            &(n00b_alloc_opts_t){.allocator = set->allocator,
                                 .scan_kind = N00B_GC_SCAN_KIND_NONE});
        if (cache == nullptr) {
            return n00b_result_err(n00b_option_t(uint64_t),
                                   N00B_PLAN_ERR_STATE);
        }
        uint64_t seen  = 0;
        uint64_t words = (set->record_count + 63) >> 6;
        for (uint64_t w = 0; w < words && seen < set->count; w++) {
            uint64_t bits = set->words[w];
            while (bits != 0 && seen < set->count) {
                cache[seen++] = (w << 6) + (uint64_t)__builtin_ctzll(bits);
                bits &= bits - 1;
            }
        }
        if (seen != set->count) {
            return n00b_result_err(n00b_option_t(uint64_t),
                                   N00B_PLAN_ERR_STATE);
        }
        set->ord_cache = cache;
    }

    return n00b_result_ok(n00b_option_t(uint64_t),
                          n00b_option_set(uint64_t, set->ord_cache[index]));
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_union(n00b_plan_ordset_t *left,
                       n00b_plan_ordset_t *right) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_ordset_binary(left,
                                   right,
                                   _rocs_plan_ordset_op_union,
                                   .allocator = allocator);
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_intersection(n00b_plan_ordset_t *left,
                              n00b_plan_ordset_t *right) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_ordset_binary(left,
                                   right,
                                   _rocs_plan_ordset_op_intersection,
                                   .allocator = allocator);
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_difference(n00b_plan_ordset_t *left,
                            n00b_plan_ordset_t *right) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_ordset_binary(left,
                                   right,
                                   _rocs_plan_ordset_op_difference,
                                   .allocator = allocator);
}

n00b_result_t(n00b_plan_ordset_t *)
n00b_plan_ordset_complement(n00b_plan_ordset_t *set) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    auto ok = _rocs_plan_ordset_check(set);
    if (n00b_result_is_err(ok)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(ok));
    }

    // Complement of an empty set is the full universe; the empty operand has
    // no bitmap to invert.
    if (set->count == 0) {
        return _rocs_plan_ordset_new(set->record_count,
                                     true,
                                     .allocator = allocator);
    }

    auto out_r = _rocs_plan_ordset_clone(set, .allocator = allocator);
    if (n00b_result_is_err(out_r)) {
        return out_r;
    }
    auto flip_r = _rocs_plan_ordset_complement_in_place(n00b_result_get(out_r));
    if (n00b_result_is_err(flip_r)) {
        return n00b_result_err(n00b_plan_ordset_t *,
                               n00b_result_get_err(flip_r));
    }
    return out_r;
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_eq(n00b_plan_target_t *target,
                       n00b_plan_value_t   value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!_rocs_plan_value_is_set(value)) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_EQ,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->value = value;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_in(n00b_plan_target_t     *target,
                       n00b_plan_value_list_t *values) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (values == nullptr) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    size_t len = n00b_list_len(*values);
    if (len == 0) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_EMPTY);
    }
    for (size_t i = 0; i < len; i++) {
        if (!_rocs_plan_value_is_set(n00b_list_get(*values, i))) {
            return n00b_result_err(n00b_plan_predicate_t *,
                                   N00B_PLAN_ERR_ARG);
        }
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_IN,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->values = values;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_range(n00b_plan_target_t *target,
                          n00b_plan_value_t   lower,
                          n00b_plan_value_t   upper) _kargs
{
    bool              include_lower = true;
    bool              include_upper = true;
    n00b_allocator_t *allocator     = nullptr;
}
{
    if (!_rocs_plan_value_is_set(lower) || !_rocs_plan_value_is_set(upper)) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_RANGE,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->lower         = lower;
    predicate->upper         = upper;
    predicate->include_lower = include_lower;
    predicate->include_upper = include_upper;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_exists(n00b_plan_target_t *target) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_leaf_new(target,
                              N00B_PLAN_LEAF_EXISTS,
                              .allocator = allocator);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_contains(n00b_plan_target_t *target,
                             n00b_string_t      *term) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (term == nullptr || term->u8_bytes == 0) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_CONTAINS,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->text = term;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_substring(n00b_plan_target_t *target,
                              n00b_string_t      *text) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (text == nullptr || text->u8_bytes == 0) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_SUBSTRING,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->text                  = text;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_prefix(n00b_plan_target_t *target,
                           n00b_string_t      *prefix) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (prefix == nullptr || prefix->u8_bytes == 0) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_PREFIX,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->text = prefix;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_regex(n00b_plan_target_t *target,
                          n00b_regex_t       *regex) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (regex == nullptr) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_REGEX,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->regex = regex;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_under(n00b_plan_target_t *target,
                          n00b_plan_path_t   *path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (path == nullptr) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    auto leaf_r = _rocs_plan_leaf_new(target,
                                     N00B_PLAN_LEAF_UNDER,
                                     .allocator = allocator);
    if (n00b_result_is_err(leaf_r)) {
        return leaf_r;
    }

    n00b_plan_predicate_t *predicate = n00b_result_get(leaf_r);
    predicate->path = path;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_and(n00b_plan_predicate_list_t *children) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_bool_new(N00B_PLAN_PREDICATE_AND,
                              children,
                              .allocator = allocator);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_or(n00b_plan_predicate_list_t *children) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return _rocs_plan_bool_new(N00B_PLAN_PREDICATE_OR,
                              children,
                              .allocator = allocator);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_not(n00b_plan_predicate_t *child) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (child == nullptr) {
        return n00b_result_err(n00b_plan_predicate_t *, N00B_PLAN_ERR_ARG);
    }

    n00b_plan_predicate_t *predicate =
        _rocs_plan_predicate_new(N00B_PLAN_PREDICATE_NOT,
                                .allocator = allocator);
    predicate->child = child;
    return n00b_result_ok(n00b_plan_predicate_t *, predicate);
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_false() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return n00b_result_ok(
        n00b_plan_predicate_t *,
        _rocs_plan_predicate_new(N00B_PLAN_PREDICATE_FALSE,
                                 .allocator = allocator));
}

n00b_result_t(n00b_plan_predicate_t *)
n00b_plan_predicate_true() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return n00b_result_ok(
        n00b_plan_predicate_t *,
        _rocs_plan_predicate_new(N00B_PLAN_PREDICATE_TRUE,
                                 .allocator = allocator));
}

n00b_result_t(n00b_plan_predicate_kind_t)
n00b_plan_predicate_kind(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_plan_predicate_kind_t,
                               N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(n00b_plan_predicate_kind_t, predicate->kind);
}

n00b_result_t(n00b_plan_leaf_op_t)
n00b_plan_predicate_leaf_op(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_plan_leaf_op_t, N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF) {
        return n00b_result_err(n00b_plan_leaf_op_t, N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_plan_leaf_op_t, predicate->leaf_op);
}

n00b_result_t(n00b_option_t(n00b_plan_target_t *))
n00b_plan_predicate_target(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_target_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF) {
        return n00b_result_ok(n00b_option_t(n00b_plan_target_t *),
                              n00b_option_none(n00b_plan_target_t *));
    }
    if (predicate->target == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_target_t *),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_target_t *),
                          n00b_option_set(n00b_plan_target_t *,
                                          predicate->target));
}

n00b_result_t(uint64_t)
n00b_plan_predicate_child_count(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_AND:
    case N00B_PLAN_PREDICATE_OR:
        if (predicate->children == nullptr) {
            return n00b_result_err(uint64_t, N00B_PLAN_ERR_STATE);
        }
        return n00b_result_ok(uint64_t,
                              (uint64_t)n00b_list_len(*predicate->children));

    case N00B_PLAN_PREDICATE_NOT:
        return n00b_result_ok(uint64_t,
                              predicate->child == nullptr ? 0 : 1);

    case N00B_PLAN_PREDICATE_LEAF:
    case N00B_PLAN_PREDICATE_FALSE:
    case N00B_PLAN_PREDICATE_TRUE:
        return n00b_result_ok(uint64_t, 0);
    }

    return n00b_result_err(uint64_t, N00B_PLAN_ERR_STATE);
}

n00b_result_t(n00b_option_t(n00b_plan_predicate_t *))
n00b_plan_predicate_child_at(n00b_plan_predicate_t *predicate,
                             uint64_t               ordinal)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_predicate_t *),
                               N00B_PLAN_ERR_ARG);
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_AND:
    case N00B_PLAN_PREDICATE_OR: {
        if (predicate->children == nullptr) {
            return n00b_result_err(n00b_option_t(n00b_plan_predicate_t *),
                                   N00B_PLAN_ERR_STATE);
        }
        uint64_t len = (uint64_t)n00b_list_len(*predicate->children);
        if (ordinal >= len) {
            return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                                  n00b_option_none(n00b_plan_predicate_t *));
        }
        return n00b_result_ok(
            n00b_option_t(n00b_plan_predicate_t *),
            n00b_option_set(n00b_plan_predicate_t *,
                            n00b_list_get(*predicate->children,
                                          (size_t)ordinal)));
    }

    case N00B_PLAN_PREDICATE_NOT:
        if (ordinal == 0 && predicate->child != nullptr) {
            return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                                  n00b_option_set(n00b_plan_predicate_t *,
                                                  predicate->child));
        }
        return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                              n00b_option_none(n00b_plan_predicate_t *));

    case N00B_PLAN_PREDICATE_LEAF:
    case N00B_PLAN_PREDICATE_FALSE:
    case N00B_PLAN_PREDICATE_TRUE:
        return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                              n00b_option_none(n00b_plan_predicate_t *));
    }

    return n00b_result_err(n00b_option_t(n00b_plan_predicate_t *),
                           N00B_PLAN_ERR_STATE);
}

n00b_result_t(n00b_option_t(n00b_plan_value_t))
n00b_plan_predicate_value(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_EQ) {
        return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                              n00b_option_none(n00b_plan_value_t));
    }
    if (!_rocs_plan_value_is_set(predicate->value)) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                          n00b_option_set(n00b_plan_value_t,
                                          predicate->value));
}

n00b_result_t(n00b_option_t(n00b_plan_value_list_t *))
n00b_plan_predicate_values(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_list_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_IN) {
        return n00b_result_ok(n00b_option_t(n00b_plan_value_list_t *),
                              n00b_option_none(n00b_plan_value_list_t *));
    }
    if (predicate->values == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_list_t *),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_value_list_t *),
                          n00b_option_set(n00b_plan_value_list_t *,
                                          predicate->values));
}

n00b_result_t(n00b_option_t(n00b_plan_value_t))
n00b_plan_predicate_range_lower(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_RANGE) {
        return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                              n00b_option_none(n00b_plan_value_t));
    }
    if (!_rocs_plan_value_is_set(predicate->lower)) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                          n00b_option_set(n00b_plan_value_t,
                                          predicate->lower));
}

n00b_result_t(n00b_option_t(n00b_plan_value_t))
n00b_plan_predicate_range_upper(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_RANGE) {
        return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                              n00b_option_none(n00b_plan_value_t));
    }
    if (!_rocs_plan_value_is_set(predicate->upper)) {
        return n00b_result_err(n00b_option_t(n00b_plan_value_t),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_value_t),
                          n00b_option_set(n00b_plan_value_t,
                                          predicate->upper));
}

n00b_result_t(bool)
n00b_plan_predicate_range_include_lower(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_RANGE) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(bool, predicate->include_lower);
}

n00b_result_t(bool)
n00b_plan_predicate_range_include_upper(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_RANGE) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(bool, predicate->include_upper);
}

n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_plan_predicate_text(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_string_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || (predicate->leaf_op != N00B_PLAN_LEAF_CONTAINS
            && predicate->leaf_op != N00B_PLAN_LEAF_PREFIX
            && predicate->leaf_op != N00B_PLAN_LEAF_SUBSTRING)) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_none(n00b_string_t *));
    }
    if (predicate->text == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_string_t *),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_string_t *),
                          n00b_option_set(n00b_string_t *,
                                          predicate->text));
}

n00b_result_t(n00b_option_t(n00b_regex_t *))
n00b_plan_predicate_regex_handle(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_regex_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_REGEX) {
        return n00b_result_ok(n00b_option_t(n00b_regex_t *),
                              n00b_option_none(n00b_regex_t *));
    }
    if (predicate->regex == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_regex_t *),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_regex_t *),
                          n00b_option_set(n00b_regex_t *,
                                          predicate->regex));
}

n00b_result_t(n00b_option_t(n00b_plan_path_t *))
n00b_plan_predicate_path(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_path_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (predicate->kind != N00B_PLAN_PREDICATE_LEAF
        || predicate->leaf_op != N00B_PLAN_LEAF_UNDER) {
        return n00b_result_ok(n00b_option_t(n00b_plan_path_t *),
                              n00b_option_none(n00b_plan_path_t *));
    }
    if (predicate->path == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_path_t *),
                               N00B_PLAN_ERR_STATE);
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_path_t *),
                          n00b_option_set(n00b_plan_path_t *,
                                          predicate->path));
}

// ---------------------------------------------------------------------------
// Plan construction. Nothing below reads a shard, a posting list, or a record.
// Index choice uses n00b_store_index_advertise, which answers from descriptor
// metadata, so a plan can be built with no store open at all.
// ---------------------------------------------------------------------------

typedef struct {
    n00b_plan_index_list_t *indexes;
    n00b_allocator_t       *allocator;
    // What the predicate's leaves match on the shard this plan is for, or null
    // for a plan built without them, which decides nothing from counts.
    // Records the plan will run over. For a partition, the sum across its
    // shards: the estimator turns a count into a fraction of the whole, and
    // half a partition's total would misjudge every complement and union.
    uint64_t                record_count;
} _rocs_plan_build_ctx_t;

static n00b_plan_node_t *
_rocs_plan_node_new(_rocs_plan_build_ctx_t *ctx, n00b_plan_node_kind_t kind)
{
    n00b_plan_node_t *node = n00b_alloc_with_opts(
        n00b_plan_node_t,
        &(n00b_alloc_opts_t){
            .allocator = ctx->allocator,
        });
    node->kind       = kind;
    node->allocator  = ctx->allocator;
    node->planned_df = N00B_PLAN_DF_UNSEEDED;
    node->index     = nullptr;
    node->key       = nullptr;
    atomic_store_explicit(&node->resolved, nullptr, memory_order_relaxed);
    node->lossy     = false;
    node->recovery  = N00B_PLAN_RECOVER_ALL;
    node->fallback  = nullptr;
    node->predicate = nullptr;
    node->children  = nullptr;
    node->child     = nullptr;
    return node;
}

static n00b_plan_node_t *
_rocs_plan_node_record_scan(_rocs_plan_build_ctx_t *ctx,
                            n00b_plan_predicate_t  *predicate)
{
    n00b_plan_node_t *node = _rocs_plan_node_new(ctx,
                                                 N00B_PLAN_NODE_RECORD_SCAN);
    node->predicate        = predicate;
    return node;
}

static n00b_plan_node_t *
_rocs_plan_node_index_scan(_rocs_plan_build_ctx_t *ctx,
                           n00b_store_index_t     *index,
                           n00b_json_node_t       *key,
                           bool                    lossy,
                           n00b_plan_recovery_t    recovery,
                           n00b_plan_predicate_t  *fallback)
{
    n00b_plan_node_t *node = _rocs_plan_node_new(ctx,
                                                 N00B_PLAN_NODE_INDEX_SCAN);
    node->index            = index;
    node->key              = key;
    node->lossy            = lossy;
    node->recovery         = recovery;
    node->fallback         = fallback;

    // Handed the counts rather than reading them: the planner touches no
    // shard, so it stays a function of its arguments and the reads happen
    // where a caller can cancel them.
    //
    // No count here. n00b_plan_collect_* writes it onto this node, which is
    // why keys never have to match across two builds: there is only one plan,
    // and its own nodes carry what was read for them.
    return node;
}

static n00b_plan_node_list_t *
_rocs_plan_node_list_new(_rocs_plan_build_ctx_t *ctx)
{
    n00b_plan_node_list_t *list = n00b_alloc_with_opts(
        n00b_plan_node_list_t,
        &(n00b_alloc_opts_t){
            .allocator = ctx->allocator,
        });
    *list = n00b_list_new_private(n00b_plan_node_t *,
                                  .allocator = ctx->allocator,
                                  .scan_kind = N00B_GC_SCAN_KIND_ALL);
    return list;
}

// A lossy index scan narrows but does not decide, so it is paired with the
// record scan that settles it.
static n00b_plan_node_t *
_rocs_plan_node_lossy_pair(_rocs_plan_build_ctx_t *ctx,
                           n00b_store_index_t     *index,
                           n00b_json_node_t       *key,
                           n00b_plan_predicate_t  *predicate)
{
    n00b_plan_node_t *node = _rocs_plan_node_new(ctx,
                                                 N00B_PLAN_NODE_INTERSECT);
    node->children         = _rocs_plan_node_list_new(ctx);
    n00b_list_push(*node->children,
                     _rocs_plan_node_index_scan(ctx, index, key, true,
                                                N00B_PLAN_RECOVER_ALL,
                                                nullptr));
    n00b_list_push(*node->children,
                     _rocs_plan_node_record_scan(ctx, predicate));
    return node;
}

// One copy of each distinct index scan in a group. Called by every group that
// builds one, so an `IN` union and a written-out disjunction of the same
// equalities read each posting list once either way.
static n00b_plan_node_list_t *
_rocs_plan_dedup_index_operands(_rocs_plan_build_ctx_t *ctx,
                                n00b_plan_node_list_t  *indexed)
{
    // The same leaf written twice reads the same posting list twice. Nothing
    // upstream folds them: a predicate is whatever the caller built, and a
    // filter lowered from a generated query can easily name one condition many
    // times. Intersecting or unioning a set with itself changes nothing, so
    // one copy answers for all of them.
    //
    // Bucketed by a digest over the descriptor, the recovery and the resolved
    // keys, at one lookup per operand. Comparing every operand against every
    // survivor is quadratic in the group's width, and the width is largest
    // exactly where there is nothing to find: `field IN (...)` plans as a
    // union of distinct term scans. A digest match is still compared
    // properly, because distinct key sets can collide and a collision has to
    // leave both leaves in place.
    //
    // A bucket holds every survivor at its digest, not the first one. Keeping
    // one meant a colliding operand was kept without being recorded, so a
    // third copy of it compared against the wrong survivor, failed, and was
    // kept too: the dedup silently stopped working for exactly the keys that
    // collided. Buckets hold one entry unless a digest collides, so the walk
    // below is a single compare in every case anybody will see.
    //
    // `fallback` is deliberately not compared. It is null for every recovery
    // but RECOVER_RECORD_SCAN, where it is the leaf predicate that produced
    // the scan and evaluates by exact comparison, while the keys above are
    // normalized. Equal keys therefore imply an equivalent fallback only where
    // normalization keeps raw values apart, which holds for every node that
    // carries one: a fallback belongs to a term scan, and term normalization
    // does not casefold. The kinds that do, full-text and n-gram, get a null
    // fallback from _rocs_plan_node_lossy_pair and a sibling record scan that
    // dedup never touches. Both halves are pinned by tests, since a normalizer
    // that started folding term values would make this wrong with nothing here
    // to notice.
    size_t have = n00b_list_len(*indexed);
    if (have > 1) {
        n00b_plan_node_list_t *unique = _rocs_plan_node_list_new(ctx);
        n00b_dict_t(uint64_t, n00b_plan_node_list_t *) *seen_by_digest =
            n00b_alloc_with_opts(
                n00b_dict_t(uint64_t, n00b_plan_node_list_t *),
                &(n00b_alloc_opts_t){.allocator = ctx->allocator});
        n00b_dict_init(seen_by_digest,
                       .allocator       = ctx->allocator,
                       .locked          = false,
                       .key_scan_kind   = N00B_GC_SCAN_KIND_NONE,
                       .value_scan_kind = N00B_GC_SCAN_KIND_ALL);

        for (size_t i = 0; i < have; i++) {
            n00b_plan_node_t *operand = n00b_list_get(*indexed, i);
            bool              seen    = false;

            n00b_store_index_keys_t *okeys = n00b_plan_node_keys(operand);
            if (okeys != nullptr) {
                uint64_t digest = n00b_store_index_keys_digest(okeys);
                digest ^= (uint64_t)(uintptr_t)operand->index;
                digest = digest * UINT64_C(0x100000001b3)
                       + (uint64_t)operand->recovery
                       + (operand->lossy ? UINT64_C(1) : UINT64_C(0));
#ifdef N00B_DEBUG
                if (atomic_load_explicit(&rocs_plan_dedup_collide,
                                         memory_order_relaxed)) {
                    digest = 0;
                }
#endif

                bool                   found  = false;
                n00b_plan_node_list_t *bucket = n00b_dict_get(seen_by_digest,
                                                              digest,
                                                              &found);
                if (!found || bucket == nullptr) {
                    bucket = _rocs_plan_node_list_new(ctx);
                    n00b_dict_put(seen_by_digest, digest, bucket);
                }

                size_t bucket_len = n00b_list_len(*bucket);
                for (size_t j = 0; j < bucket_len; j++) {
                    n00b_plan_node_t *other = n00b_list_get(*bucket, j);
                    if (other != nullptr
                        && other->index == operand->index
                        && other->lossy == operand->lossy
                        && other->recovery == operand->recovery
                        && n00b_store_index_keys_equal(
                            n00b_plan_node_keys(other), okeys)) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    n00b_list_push(*bucket, operand);
                }
            }
            if (!seen) {
                n00b_list_push(*unique, operand);
            }
        }
        indexed = unique;
    }

    return indexed;
}

static n00b_result_t(n00b_plan_node_t *)
_rocs_plan_build_node(_rocs_plan_build_ctx_t *ctx,
                      n00b_plan_predicate_t  *predicate);

static n00b_result_t(n00b_plan_node_t *)
_rocs_plan_build_leaf(_rocs_plan_build_ctx_t *ctx,
                      n00b_plan_predicate_t  *predicate)
{
    if (predicate->target == nullptr) {
        return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
    }

    if (predicate->target->kind == N00B_PLAN_TARGET_ANY) {
        if (predicate->leaf_op != N00B_PLAN_LEAF_CONTAINS) {
            return n00b_result_err(n00b_plan_node_t *,
                                   N00B_PLAN_ERR_ANY_UNSUPPORTED);
        }
        n00b_store_index_t *index = _rocs_plan_choose_catch_all_index(
            ctx->indexes);
        if (index == nullptr || predicate->text == nullptr) {
            // No schema opt-in list means raw verification would match fields
            // the catch-all deliberately excludes, so the answer is empty.
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_new(ctx,
                                                      N00B_PLAN_NODE_EMPTY));
        }
        n00b_json_node_t *key = n00b_json_string_new_from_n00b(
            predicate->text,
            .allocator = ctx->allocator);
        if (key == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_new(ctx,
                                                      N00B_PLAN_NODE_EMPTY));
        }
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_index_scan(
                                  ctx, index, key, false,
                                  N00B_PLAN_RECOVER_EMPTY, nullptr));
    }

    n00b_string_t *field = predicate->target->field;
    if (field == nullptr) {
        return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
    }

    switch (predicate->leaf_op) {
    case N00B_PLAN_LEAF_EQ: {
        if (!_rocs_plan_value_is_set(predicate->value)) {
            return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
        }
        n00b_store_index_t *index = _rocs_plan_choose_index(
            ctx->indexes, field,
            N00B_STORE_INDEX_OP_EQ, N00B_STORE_INDEX_TERM);
        if (index == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        auto key_r = _rocs_plan_value_node(predicate->value);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_index_scan(
                                  ctx, index, n00b_result_get(key_r), false,
                                  N00B_PLAN_RECOVER_RECORD_SCAN, predicate));
    }

    case N00B_PLAN_LEAF_CONTAINS: {
        n00b_store_index_t *index = _rocs_plan_choose_index(
            ctx->indexes, field,
            N00B_STORE_INDEX_OP_CONTAINS, N00B_STORE_INDEX_FULLTEXT);
        if (index == nullptr || predicate->text == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        n00b_json_node_t *key = n00b_json_string_new_from_n00b(
            predicate->text,
            .allocator = ctx->allocator);
        if (key == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_index_scan(
                                  ctx, index, key, false,
                                  N00B_PLAN_RECOVER_RECORD_SCAN, predicate));
    }

    case N00B_PLAN_LEAF_PREFIX:
    case N00B_PLAN_LEAF_SUBSTRING:
    case N00B_PLAN_LEAF_REGEX: {
        n00b_store_index_t *index = _rocs_plan_choose_index(
            ctx->indexes, field,
            N00B_STORE_INDEX_OP_PREFIX, N00B_STORE_INDEX_NGRAM);
        if (index == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        n00b_string_t *literal = predicate->text;
        if (predicate->leaf_op == N00B_PLAN_LEAF_REGEX) {
            if (predicate->regex == nullptr) {
                return n00b_result_err(n00b_plan_node_t *,
                                       N00B_PLAN_ERR_STATE);
            }
            // Any required literal, not only one at the start. The n-gram
            // index already accepts an interior literal as a candidate
            // generator, which is what SUBSTRING hands it, so a regex whose
            // literal sits after an alternation rides the same lossy pair
            // instead of reading every record.
            n00b_option_t(n00b_string_t *) lit = n00b_regex_required_literal_anywhere(
                predicate->regex,
                .allocator = ctx->allocator);
            if (!n00b_option_is_set(lit)) {
                return n00b_result_ok(n00b_plan_node_t *,
                                      _rocs_plan_node_record_scan(ctx,
                                                                  predicate));
            }
            literal = n00b_option_get(lit);
        }
        if (literal == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        auto key_r = _rocs_plan_ngram_query_node(literal, index,
                                                 .allocator = ctx->allocator);
        if (n00b_result_is_err(key_r)) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_lossy_pair(
                                  ctx, index, n00b_result_get(key_r),
                                  predicate));
    }

    case N00B_PLAN_LEAF_IN: {
        // A union of term lookups, which is what an OR of the same equalities
        // already builds. Each branch carries the equality it came from as its
        // recovery, so a branch whose index turns out to be unusable verifies
        // itself rather than widening the union.
        if (predicate->values == nullptr) {
            return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
        }
        size_t value_count = n00b_list_len(*predicate->values);
        if (value_count == 0) {
            return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
        }
        if (value_count > ROCS_PLAN_IN_FANOUT_MAX) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }

        n00b_store_index_t *index = _rocs_plan_choose_index(
            ctx->indexes, field,
            N00B_STORE_INDEX_OP_EQ, N00B_STORE_INDEX_TERM);
        if (index == nullptr) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }

        n00b_plan_node_list_t *branches = _rocs_plan_node_list_new(ctx);
        for (size_t i = 0; i < value_count; i++) {
            n00b_plan_value_t value = n00b_list_get(*predicate->values, i);

            // One value the index cannot be keyed for would leave the union
            // short of the answer, so the whole leaf goes back to a single
            // pass over the records. Reachable for a value holding a null
            // node: n00b_plan_predicate_in rejects an unset one, but a
            // predicate assembled in this file rather than through that
            // constructor is answered here too.
            auto key_r = _rocs_plan_value_node(value);
            if (n00b_result_is_err(key_r)) {
                return n00b_result_ok(n00b_plan_node_t *,
                                      _rocs_plan_node_record_scan(ctx,
                                                                  predicate));
            }

            // Every value reaching here is one the eq constructor accepts, so
            // this fails only if the allocation behind it does. Scanning is
            // still the answer; there is no half-built union worth keeping.
            auto eq_r = n00b_plan_predicate_eq(predicate->target,
                                               value,
                                               .allocator = ctx->allocator);
            if (n00b_result_is_err(eq_r)) {
                return n00b_result_ok(n00b_plan_node_t *,
                                      _rocs_plan_node_record_scan(ctx,
                                                                  predicate));
            }

            n00b_list_push(*branches,
                           _rocs_plan_node_index_scan(
                               ctx, index, n00b_result_get(key_r), false,
                               N00B_PLAN_RECOVER_RECORD_SCAN,
                               n00b_result_get(eq_r)));
        }

        branches = _rocs_plan_dedup_index_operands(ctx, branches);
        if (n00b_list_len(*branches) == 1) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  n00b_list_get(*branches, 0));
        }

        n00b_plan_node_t *node = _rocs_plan_node_new(ctx,
                                                     N00B_PLAN_NODE_UNION);
        node->children         = branches;
        return n00b_result_ok(n00b_plan_node_t *, node);
    }

    default:
        // EXISTS, RANGE and UNDER have no index path.
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_record_scan(ctx, predicate));
    }
}

// INTERSECT and UNION are associative, so a child of the same kind belongs in
// its parent. Splicing it there lets execution resolve every index scan in the
// group before any record scan runs, instead of a nested group scanning records
// against its own candidates while a selective sibling sits unapplied.
//
// Record scans in the same group merge into one, so a group costs a single pass
// over the records rather than one pass per unindexed leaf.
static void
_rocs_plan_collect_operand(n00b_plan_node_t           *child,
                           n00b_plan_node_kind_t       kind,
                           n00b_plan_node_list_t      *indexed,
                           n00b_plan_predicate_list_t *scans,
                           n00b_err_t                 *failed)
{
    if (child == nullptr) {
        return;
    }
    if (child->kind == N00B_PLAN_NODE_RECORD_SCAN) {
        auto append_r = n00b_plan_predicate_list_append(scans,
                                                       child->predicate);
        if (n00b_result_is_err(append_r)) {
            *failed = n00b_result_get_err(append_r);
        }
        return;
    }
    if (child->kind == kind && child->children != nullptr) {
        size_t count = n00b_list_len(*child->children);
        for (size_t i = 0; i < count; i++) {
            _rocs_plan_collect_operand(n00b_list_get(*child->children, i),
                                       kind,
                                       indexed,
                                       scans,
                                       failed);
        }
        return;
    }
    n00b_list_push(*indexed, child);
}

// A lossy pair is an index scan that over-approximates beside the record scan
// that settles it. Recognising the shape lets a union of them share one pass.
static bool
_rocs_plan_split_lossy_pair(n00b_plan_node_t      *node,
                            n00b_plan_node_t     **index_out,
                            n00b_plan_predicate_t **predicate_out)
{
    if (node == nullptr || node->kind != N00B_PLAN_NODE_INTERSECT
        || node->children == nullptr
        || n00b_list_len(*node->children) != 2) {
        return false;
    }
    n00b_plan_node_t *first  = n00b_list_get(*node->children, 0);
    n00b_plan_node_t *second = n00b_list_get(*node->children, 1);
    if (first == nullptr || second == nullptr
        || first->kind != N00B_PLAN_NODE_INDEX_SCAN || !first->lossy
        || second->kind != N00B_PLAN_NODE_RECORD_SCAN) {
        return false;
    }
    *index_out     = first;
    *predicate_out = second->predicate;
    return true;
}

// Two lossy branches under a union read their overlap twice, once per pair.
// Since each index scan covers its own predicate, the union of the candidate
// sets covers the disjunction, so the group becomes one candidate set and one
// pass:
//
//     UNION[ INTERSECT[idx_a, scan_a], INTERSECT[idx_b, scan_b] ]
//  -> INTERSECT[ UNION[idx_a, idx_b], RECORD_SCAN(a OR b) ]
//
// Branches that need no record scan stay out of the group: folding an exact
// one in would filter its hits by the other branches' predicates.
static n00b_result_t(n00b_plan_node_t *)
_rocs_plan_group_lossy_union(_rocs_plan_build_ctx_t *ctx,
                             n00b_plan_node_list_t  *operands)
{
    n00b_plan_node_list_t      *pairs = _rocs_plan_node_list_new(ctx);
    n00b_plan_node_list_t      *rest  = _rocs_plan_node_list_new(ctx);
    n00b_plan_predicate_list_t *preds =
        n00b_plan_predicate_list_new(.allocator = ctx->allocator);

    size_t count = n00b_list_len(*operands);
    for (size_t i = 0; i < count; i++) {
        n00b_plan_node_t      *operand = n00b_list_get(*operands, i);
        n00b_plan_node_t      *index   = nullptr;
        n00b_plan_predicate_t *pred    = nullptr;
        if (_rocs_plan_split_lossy_pair(operand, &index, &pred)) {
            n00b_list_push(*pairs, index);
            auto append_r = n00b_plan_predicate_list_append(preds, pred);
            if (n00b_result_is_err(append_r)) {
                return n00b_result_err(n00b_plan_node_t *,
                                       n00b_result_get_err(append_r));
            }
            continue;
        }
        n00b_list_push(*rest, operand);
    }

    if (n00b_list_len(*pairs) < 2) {
        return n00b_result_ok(n00b_plan_node_t *, nullptr);
    }

    n00b_plan_node_t *candidates = _rocs_plan_node_new(ctx,
                                                       N00B_PLAN_NODE_UNION);
    candidates->children = pairs;

    auto merged_r = n00b_plan_predicate_or(preds, .allocator = ctx->allocator);
    if (n00b_result_is_err(merged_r)) {
        return n00b_result_err(n00b_plan_node_t *,
                               n00b_result_get_err(merged_r));
    }

    n00b_plan_node_t *grouped = _rocs_plan_node_new(ctx,
                                                    N00B_PLAN_NODE_INTERSECT);
    grouped->children = _rocs_plan_node_list_new(ctx);
    n00b_list_push(*grouped->children, candidates);
    n00b_list_push(*grouped->children,
                   _rocs_plan_node_record_scan(ctx,
                                               n00b_result_get(merged_r)));

    if (n00b_list_len(*rest) == 0) {
        return n00b_result_ok(n00b_plan_node_t *, grouped);
    }
    n00b_list_push(*rest, grouped);
    n00b_plan_node_t *node = _rocs_plan_node_new(ctx, N00B_PLAN_NODE_UNION);
    node->children         = rest;
    return n00b_result_ok(n00b_plan_node_t *, node);
}

// Order a group's operands by what they match on this shard, when the counts
// are known. The same rule execution orders by, called from the other side:
// deciding here means the plan carries the order rather than rediscovering it
// on every execution, and a plan that carries it can be read to see what will
// happen.
//
// Only operands whose count was collected take part. One whose count is
// unknown keeps its place, because moving it would be ordering on a number
// nobody has.
// How much a node narrows and what running it costs, from counts already on
// the tree. A leaf is its own count both ways, a complement is as wide as its
// child is narrow but costs what its child costs, and a group composes.
//
// Depth-bounded. Everything below the first level is read from nodes rather
// than from a shard, so this is a walk and not more I/O, but an arbitrarily
// deep tree is still an arbitrarily long walk.
#define ROCS_PLAN_ESTIMATE_DEPTH 6

static bool
_rocs_plan_estimate_node(_rocs_plan_build_ctx_t *ctx,
                         n00b_plan_node_t       *node,
                         int                     depth,
                         uint64_t               *size,
                         uint64_t               *cost)
{
    if (node == nullptr || depth > ROCS_PLAN_ESTIMATE_DEPTH) {
        return false;
    }

    switch (node->kind) {
    case N00B_PLAN_NODE_INDEX_SCAN:
        if (node->planned_df == UINT64_MAX
            || node->planned_df == N00B_PLAN_DF_UNSEEDED) {
            return false;
        }
        *size = *cost = node->planned_df;
        return true;

    case N00B_PLAN_NODE_COMPLEMENT: {
        uint64_t cs = 0;
        uint64_t cc = 0;
        if (!_rocs_plan_estimate_node(ctx, node->child, depth + 1, &cs, &cc)) {
            return false;
        }
        *size = n00b_plan_cost_complement_df(cs, ctx->record_count);
        *cost = cc;
        return true;
    }

    case N00B_PLAN_NODE_INTERSECT:
    case N00B_PLAN_NODE_UNION: {
        if (node->children == nullptr) {
            return false;
        }
        size_t len = n00b_list_len(*node->children);
        if (len == 0) {
            return false;
        }
        bool     intersect = node->kind == N00B_PLAN_NODE_INTERSECT;
        uint64_t acc_size  = 0;
        uint64_t acc_cost  = 0;
        for (size_t i = 0; i < len; i++) {
            uint64_t cs = 0;
            uint64_t cc = 0;
            // Every child. A group holding one operand nothing can bound is a
            // group nothing can bound, and ordering its parent from the rest
            // would order it by a number describing something else.
            if (!_rocs_plan_estimate_node(ctx,
                                          n00b_list_get(*node->children, i),
                                          depth + 1, &cs, &cc)) {
                return false;
            }
            acc_size = i == 0 ? cs
                     : intersect
                         ? n00b_plan_cost_intersect_size(acc_size, cs)
                         : n00b_plan_cost_union_size(acc_size, cs,
                                                     ctx->record_count);
            acc_cost = acc_cost > UINT64_MAX - cc ? UINT64_MAX : acc_cost + cc;
        }
        *size = acc_size;
        *cost = acc_cost;
        return true;
    }

    case N00B_PLAN_NODE_RECORD_SCAN: {
        // A scan reads the whole shard and keeps whatever its predicate keeps,
        // so its width is a fraction of the universe rather than a count read
        // off a node. Estimated rather than unknown because unknown is not
        // free: the sort stops at any operand it cannot bound rather than
        // reordering around one, so everything written behind such an operand
        // keeps its position however narrow it is.
        //
        // A group's own merged scan is pushed last and so blocks nothing.
        // What this buys is the nested group: an estimate needs every child,
        // so a union or intersection holding an unestimated scan would report
        // unknown to its parent and become the barrier there, in the middle of
        // a group whose other operands are well bounded.
        //
        // With no universe there is nothing to scale, and that case stays
        // unknown rather than estimating everything at zero.
        if (ctx->record_count == 0) {
            return false;
        }
        uint32_t sel = n00b_plan_cost_selectivity(node->predicate);
        *size = ctx->record_count / N00B_PLAN_SEL_SCALE * sel
              + ctx->record_count % N00B_PLAN_SEL_SCALE * sel
                    / N00B_PLAN_SEL_SCALE;

        // Every record decoded, and the predicate run on each. That is the
        // number that keeps a scan behind index scans it should not displace:
        // an index scan costs its posting count, and a scan costs the shard.
        uint64_t per = n00b_plan_cost_predicate(node->predicate);
        *cost = per != 0 && ctx->record_count > UINT64_MAX / per
                  ? UINT64_MAX
                  : ctx->record_count * per;
        return true;
    }

    case N00B_PLAN_NODE_EMPTY:
        return false;
    }
    return false;
}

static void
_rocs_plan_order_group(_rocs_plan_build_ctx_t *ctx,
                       n00b_plan_node_list_t  *children,
                       bool                    widest)
{
    // Ordering reads counts, so it answers to the cost switch like every other
    // decision that does. Without this the switch turns off two of the three
    // plan-time choices and a control arm built with it off is still ordered.
    if (children == nullptr || !n00b_plan_cost_enabled()) {
        return;
    }
    size_t len = n00b_list_len(*children);
    if (len < 2) {
        return;
    }

    // Every child ordered, not just the first hoisted.
    //
    // The estimates below are the expensive part and every child needs one
    // either way, so sorting on them costs a pass over numbers already in
    // hand. The first operand matters most, because it is what shrinks the
    // accumulator enough for the rest to probe rather than walk, but the tail
    // is not free after that: a probe costs log2(df) per candidate, so the
    // order the remaining operands run in still decides how much each of them
    // charges. The merged record scans beside this are sorted in full for the
    // same reason.
    //
    // A child nothing can bound keeps its place. Moving it would be ordering
    // on a number nobody has.
    uint64_t *sizes = n00b_alloc_array_with_opts(
        uint64_t, len,
        &(n00b_alloc_opts_t){.allocator = ctx->allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});
    uint64_t *costs = n00b_alloc_array_with_opts(
        uint64_t, len,
        &(n00b_alloc_opts_t){.allocator = ctx->allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});
    bool *known = n00b_alloc_array_with_opts(
        bool, len,
        &(n00b_alloc_opts_t){.allocator = ctx->allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});

    // Both numbers, because they diverge and the tie-break needs the second.
    // A complement is the clearest case: it is as wide as its child is narrow,
    // so its size says "runs last" while its cost says "cheap". Ordering on
    // size alone puts it behind operands it should have preceded.
    for (size_t i = 0; i < len; i++) {
        uint64_t size = 0;
        uint64_t cost = 0;
        known[i] = _rocs_plan_estimate_node(ctx, n00b_list_get(*children, i),
                                            0, &size, &cost);
        sizes[i] = known[i] ? size : 0;
        costs[i] = known[i] ? cost : 0;
    }

    // A complement's size estimate is record_count minus its child's, so a NOT
    // over a rare term looks nearly as wide as the shard and sorts to the end.
    // Its execution cost is its child's, which is small, so that placement is
    // not what its cost says. Ordering a group holding one measurably reads
    // more postings than leaving it alone (test_rocs_plan_pathological, the
    // negated shapes), so such a group gets the single hoist instead: the
    // first operand still moves, and nothing else is reordered on a number
    // that describes the wrong thing.
    bool has_complement = false;
    for (size_t i = 0; i < len; i++) {
        n00b_plan_node_t *child = n00b_list_get(*children, i);
        if (child != nullptr && child->kind == N00B_PLAN_NODE_COMPLEMENT) {
            has_complement = true;
            break;
        }
    }

    if (has_complement) {
        size_t   best      = len;
        uint64_t best_size = 0;
        uint64_t best_cost = 0;
        for (size_t i = 0; i < len; i++) {
            if (!known[i]) {
                continue;
            }
            if (best == len
                || n00b_plan_cost_prefers(sizes[i], costs[i], best_size,
                                          best_cost, widest, true)) {
                best      = i;
                best_size = sizes[i];
                best_cost = costs[i];
            }
        }
        if (best != len && best != 0) {
            n00b_plan_node_t *winner = n00b_list_get(*children, best);
            for (size_t i = best; i > 0; i--) {
                n00b_list_set(*children, i, n00b_list_get(*children, i - 1));
            }
            n00b_list_set(*children, 0, winner);
        }
        return;
    }

    // Insertion sort, so equal children keep their written order: two operands
    // a count cannot separate should run in the order somebody wrote them.
    for (size_t i = 1; i < len; i++) {
        n00b_plan_node_t *node = n00b_list_get(*children, i);
        uint64_t          size = sizes[i];
        uint64_t          cost = costs[i];
        bool              has  = known[i];
        size_t            j    = i;

        while (j > 0) {
            if (!has || !known[j - 1]) {
                break;
            }
            if (!n00b_plan_cost_prefers(size, cost, sizes[j - 1],
                                        costs[j - 1], widest, true)) {
                break;
            }
            n00b_list_set(*children, j, n00b_list_get(*children, j - 1));
            sizes[j] = sizes[j - 1];
            costs[j] = costs[j - 1];
            known[j] = known[j - 1];
            j--;
        }
        n00b_list_set(*children, j, node);
        sizes[j] = size;
        costs[j] = cost;
        known[j] = has;
    }
}

// Apply the decisions that need counts, bottom up, to a plan already built.
//
// Children first: ordering a group estimates each child, and an estimate is
// only as settled as the child it describes. A group that an operand empties
// becomes EMPTY in place rather than being rebuilt, so every reference to it
// from an enclosing group stays valid.
// Fold one shard's count for a leaf into what the plan already holds.
//
// Unknown is contagious and is not zero. A shard that cannot answer for a leaf
// (no column for the field, so its records must be scanned) makes the total
// unknown for the whole partition, because a sum that quietly skipped it would
// read as "matches nothing" and settle an intersection whose rows are there.
// That is n00b#202 at partition scale.
static void
rocs_plan_df_accumulate(n00b_plan_node_t *node, uint64_t df, bool known)
{
    if (!known || node->planned_df == UINT64_MAX) {
        node->planned_df = UINT64_MAX;
        return;
    }
    if (node->planned_df == N00B_PLAN_DF_UNSEEDED) {
        node->planned_df = df;
        return;
    }
    // Saturating, and deliberately short of the two sentinels above it: a
    // total that landed on N00B_PLAN_DF_UNSEEDED would read as "never
    // collected" and a decision would skip the leaf it just counted.
    if (node->planned_df > UINT64_MAX - 2 - df) {
        node->planned_df = UINT64_MAX - 2;
        return;
    }
    node->planned_df += df;
}

static void
_rocs_plan_settle_node(_rocs_plan_build_ctx_t *ctx, n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return;
    }

    _rocs_plan_settle_node(ctx, node->child);

    if (node->children == nullptr) {
        return;
    }

    size_t len = n00b_list_len(*node->children);
    for (size_t i = 0; i < len; i++) {
        _rocs_plan_settle_node(ctx, n00b_list_get(*node->children, i));
    }

    if (!n00b_plan_cost_enabled()) {
        return;
    }

    // An operand matching nothing settles an intersection without running any
    // of it. Execution reaches the same answer, but only by running that
    // operand to find out.
    if (node->kind == N00B_PLAN_NODE_INTERSECT) {
        for (size_t i = 0; i < len; i++) {
            n00b_plan_node_t *child = n00b_list_get(*node->children, i);
            if (child == nullptr) {
                continue;
            }
            // Either an operand that matches nothing, or one already settled
            // to nothing. The second case is what makes this propagate: a
            // leaf that is not eq plans as a group of its own, so its zero
            // empties that group and the group has to empty its parent. Only
            // checking for a zero-count scan stops at the first level and
            // leaves the enclosing intersection running against nothing.
            bool empty = child->kind == N00B_PLAN_NODE_EMPTY
                      || (child->kind == N00B_PLAN_NODE_INDEX_SCAN
                          && child->planned_df == 0);
            if (empty) {
                node->kind     = N00B_PLAN_NODE_EMPTY;
                node->children = nullptr;
                node->child    = nullptr;
                return;
            }
        }
    }

    if (node->kind == N00B_PLAN_NODE_INTERSECT
        || node->kind == N00B_PLAN_NODE_UNION) {
        _rocs_plan_order_group(ctx,
                               node->children,
                               node->kind == N00B_PLAN_NODE_UNION);
    }
}

static n00b_result_t(n00b_plan_node_t *)
_rocs_plan_build_nary(_rocs_plan_build_ctx_t *ctx,
                      n00b_plan_predicate_t  *predicate,
                      n00b_plan_node_kind_t   kind)
{
    if (predicate->children == nullptr) {
        return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_ARG);
    }
    size_t count = n00b_list_len(*predicate->children);
    if (count < 2) {
        return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
    }

    n00b_plan_node_list_t      *indexed = _rocs_plan_node_list_new(ctx);
    n00b_plan_predicate_list_t *scans =
        n00b_plan_predicate_list_new(.allocator = ctx->allocator);
    n00b_err_t collect_err = N00B_PLAN_OK;

    for (size_t i = 0; i < count; i++) {
        auto child_r = _rocs_plan_build_node(
            ctx, n00b_list_get(*predicate->children, i));
        if (n00b_result_is_err(child_r)) {
            return child_r;
        }
        _rocs_plan_collect_operand(n00b_result_get(child_r),
                                   kind,
                                   indexed,
                                   scans,
                                   &collect_err);
    }
    if (collect_err != N00B_PLAN_OK) {
        return n00b_result_err(n00b_plan_node_t *, collect_err);
    }

    // A union with an unrestricted branch is already as wide as the shard, so
    // a lossy branch's index cannot narrow it. Dissolve those pairs into the
    // shared pass rather than reading their candidates a second time.
    if (kind == N00B_PLAN_NODE_UNION && n00b_list_len(*scans) > 0) {
        n00b_plan_node_list_t *kept = _rocs_plan_node_list_new(ctx);
        size_t                 have = n00b_list_len(*indexed);
        for (size_t i = 0; i < have; i++) {
            n00b_plan_node_t      *operand = n00b_list_get(*indexed, i);
            n00b_plan_node_t      *index   = nullptr;
            n00b_plan_predicate_t *pred    = nullptr;
            if (_rocs_plan_split_lossy_pair(operand, &index, &pred)) {
                auto append_r = n00b_plan_predicate_list_append(scans, pred);
                if (n00b_result_is_err(append_r)) {
                    return n00b_result_err(n00b_plan_node_t *,
                                           n00b_result_get_err(append_r));
                }
                continue;
            }
            n00b_list_push(*kept, operand);
        }
        indexed = kept;
    }

    indexed = _rocs_plan_dedup_index_operands(ctx, indexed);

    size_t scan_count = n00b_list_len(*scans);
    if (scan_count == 1) {
        n00b_list_push(*indexed,
                       _rocs_plan_node_record_scan(ctx,
                                                   n00b_list_get(*scans, 0)));
    }
    else if (scan_count > 1) {
        // The merged predicate is the plan's own, so ordering it here is a
        // plan-time choice rather than one execution has to repeat. Same rule
        // and same ordering the scan uses, called from the other side.
        // Gated with the rest of the cost model: the switch exists so one
        // process can run a query both ways and compare, and a half it does
        // not reach is a half that check cannot see.
        uint16_t order[64];
        if (scan_count <= 64 && n00b_plan_cost_enabled()) {
            n00b_plan_predicate_list_t *sorted = n00b_plan_predicate_list_new();
            n00b_plan_predicate_t       group  = {
                       .kind     = kind == N00B_PLAN_NODE_INTERSECT
                                       ? N00B_PLAN_PREDICATE_AND
                                       : N00B_PLAN_PREDICATE_OR,
                       .children = scans,
            };
            size_t n = n00b_plan_cost_order_children(&group, order, 64);
            if (n == scan_count && sorted != nullptr) {
                for (size_t i = 0; i < n; i++) {
                    if (n00b_result_is_err(n00b_plan_predicate_list_append(
                            sorted,
                            n00b_list_get(*scans, order[i])))) {
                        sorted = nullptr;
                        break;
                    }
                }
                if (sorted != nullptr) {
                    scans = sorted;
                }
            }
        }

        auto merged_r = kind == N00B_PLAN_NODE_INTERSECT
                            ? n00b_plan_predicate_and(scans,
                                                      .allocator = ctx->allocator)
                            : n00b_plan_predicate_or(scans,
                                                     .allocator = ctx->allocator);
        if (n00b_result_is_err(merged_r)) {
            return n00b_result_err(n00b_plan_node_t *,
                                   n00b_result_get_err(merged_r));
        }
        n00b_list_push(*indexed,
                       _rocs_plan_node_record_scan(ctx,
                                                   n00b_result_get(merged_r)));
    }

    if (kind == N00B_PLAN_NODE_UNION) {
        auto grouped_r = _rocs_plan_group_lossy_union(ctx, indexed);
        if (n00b_result_is_err(grouped_r)) {
            return grouped_r;
        }
        if (n00b_result_get(grouped_r) != nullptr) {
            return grouped_r;
        }
    }

    if (n00b_list_len(*indexed) == 1) {
        return n00b_result_ok(n00b_plan_node_t *, n00b_list_get(*indexed, 0));
    }

    // Nothing here reads a count. Ordering this group and settling it against
    // an operand that matches nothing are both decided later, by
    // n00b_plan_settle, once the counts are in. Building stays a function of
    // the predicate and the index list alone, which is what lets one plan
    // serve every shard in a partition.
    n00b_plan_node_t *node = _rocs_plan_node_new(ctx, kind);
    node->children         = indexed;
    return n00b_result_ok(n00b_plan_node_t *, node);
}

// True when some index scan in the subtree over-approximates.
static bool
_rocs_plan_tree_has_lossy(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return false;
    }
    if (node->kind == N00B_PLAN_NODE_INDEX_SCAN) {
        return node->lossy;
    }
    if (node->kind == N00B_PLAN_NODE_COMPLEMENT) {
        return _rocs_plan_tree_has_lossy(node->child);
    }
    if (node->children == nullptr) {
        return false;
    }
    size_t count = n00b_list_len(*node->children);
    for (size_t i = 0; i < count; i++) {
        if (_rocs_plan_tree_has_lossy(n00b_list_get(*node->children, i))) {
            return true;
        }
    }
    return false;
}

// Whether a subtree answers the predicate exactly, so complementing it is
// sound. Reading no records is not enough on its own: a lossy index scan
// over-approximates without touching one.
static bool
_rocs_plan_is_definite(n00b_plan_node_t *node)
{
    auto reads_r = n00b_plan_reads_no_records(node);

    return n00b_result_is_ok(reads_r) && n00b_result_get(reads_r)
        && !_rocs_plan_tree_has_lossy(node);
}

// An any-field predicate has no meaning against a single record: the leaf
// evaluator rejects it, so a negation wrapped around one would match
// everything. Such a predicate may only ever be answered from the catch-all
// index, never tested by a record scan.
static bool
_rocs_plan_predicate_mentions_any(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return false;
    }
    if (predicate->kind == N00B_PLAN_PREDICATE_LEAF) {
        return predicate->target != nullptr
            && predicate->target->kind == N00B_PLAN_TARGET_ANY;
    }
    if (predicate->kind == N00B_PLAN_PREDICATE_NOT) {
        return _rocs_plan_predicate_mentions_any(predicate->child);
    }
    if (predicate->children == nullptr) {
        return false;
    }
    size_t count = n00b_list_len(*predicate->children);
    for (size_t i = 0; i < count; i++) {
        if (_rocs_plan_predicate_mentions_any(
                n00b_list_get(*predicate->children, i))) {
            return true;
        }
    }
    return false;
}

static n00b_result_t(n00b_plan_node_t *)
_rocs_plan_build_node(_rocs_plan_build_ctx_t *ctx,
                      n00b_plan_predicate_t  *predicate)
{
    if (ctx == nullptr || predicate == nullptr) {
        return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_ARG);
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_LEAF:
        return _rocs_plan_build_leaf(ctx, predicate);
    case N00B_PLAN_PREDICATE_AND:
        return _rocs_plan_build_nary(ctx, predicate,
                                     N00B_PLAN_NODE_INTERSECT);
    case N00B_PLAN_PREDICATE_OR:
        return _rocs_plan_build_nary(ctx, predicate, N00B_PLAN_NODE_UNION);
    case N00B_PLAN_PREDICATE_NOT: {
        if (predicate->child == nullptr) {
            return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_ARG);
        }
        auto child_r = _rocs_plan_build_node(ctx, predicate->child);
        if (n00b_result_is_err(child_r)) {
            return child_r;
        }
        n00b_plan_node_t *child = n00b_result_get(child_r);

        // Only a definite set can be complemented. A child that reads records
        // negates through a record scan instead, which lets it merge with its
        // siblings into one pass rather than adding a pass of its own.
        bool definite = _rocs_plan_is_definite(child);

        if (!definite && !_rocs_plan_predicate_mentions_any(predicate)) {
            return n00b_result_ok(n00b_plan_node_t *,
                                  _rocs_plan_node_record_scan(ctx, predicate));
        }

        // Complementing an indefinite child costs a pass of its own, so only
        // an any-field predicate, which no record scan may hold, reaches here
        // without being definite. Stated separately from the condition above
        // so that relaxing one without the other is caught.
        n00b_assert(definite
                    || _rocs_plan_predicate_mentions_any(predicate));

        n00b_plan_node_t *node = _rocs_plan_node_new(
            ctx, N00B_PLAN_NODE_COMPLEMENT);
        node->child = child;
        return n00b_result_ok(n00b_plan_node_t *, node);
    }
    case N00B_PLAN_PREDICATE_FALSE:
        return n00b_result_ok(n00b_plan_node_t *,
                              _rocs_plan_node_new(ctx, N00B_PLAN_NODE_EMPTY));

    case N00B_PLAN_PREDICATE_TRUE: {
        // The whole shard, spelled with the vocabulary that already exists
        // rather than a seventh node kind: complementing an empty set yields
        // the full universe (n00b_plan_ordset_complement), so EMPTY under a
        // COMPLEMENT is the universe node. Execution needs no case for it and
        // settling sizes it from the record count like any other complement.
        n00b_plan_node_t *node = _rocs_plan_node_new(ctx,
                                                     N00B_PLAN_NODE_COMPLEMENT);
        node->child            = _rocs_plan_node_new(ctx,
                                                     N00B_PLAN_NODE_EMPTY);
        return n00b_result_ok(n00b_plan_node_t *, node);
    }
    }

    return n00b_result_err(n00b_plan_node_t *, N00B_PLAN_ERR_STATE);
}


#ifdef N00B_DEBUG
// Properties every plan must have, checked on the way out of the planner so
// that any test which builds a plan checks them. Compiled out of ordinary
// builds along with n00b_assert.
//
// Merging combines record scans that are siblings in one group, so that is
// what gets checked: no group may hold two of them. A scan nested in another
// subtree has no sibling to merge with, and the IR has no node meaning "test
// this predicate against an ordinal set", so those stay separate.
static void
_rocs_plan_audit(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return;
    }

    switch (node->kind) {
    case N00B_PLAN_NODE_RECORD_SCAN:
        n00b_assert(node->predicate != nullptr);
        // An any-field predicate evaluates false against a record, so a scan
        // that tested one would answer with silence, or with everything once
        // negated. Those may only be answered from the catch-all index.
        n00b_assert(!_rocs_plan_predicate_mentions_any(node->predicate));
        return;

    case N00B_PLAN_NODE_INDEX_SCAN:
        n00b_assert(node->index != nullptr);
        n00b_assert(node->key != nullptr);
        // A lossy scan over-approximates, so it is only ever an answer when
        // something else settles it. Recovering by reading records would make
        // it decide on its own.
        n00b_assert(!node->lossy
                    || node->recovery == N00B_PLAN_RECOVER_ALL);
        return;

    case N00B_PLAN_NODE_INTERSECT:
    case N00B_PLAN_NODE_UNION: {
        n00b_assert(node->children != nullptr);
        n00b_assert(n00b_list_len(*node->children) >= 2);
        uint64_t scans = 0;
        size_t   count = n00b_list_len(*node->children);
        for (size_t i = 0; i < count; i++) {
            n00b_plan_node_t *child = n00b_list_get(*node->children, i);
            if (child != nullptr
                && child->kind == N00B_PLAN_NODE_RECORD_SCAN) {
                scans++;
            }
            _rocs_plan_audit(child);
        }
        // Two sibling scans mean a merge was missed.
        n00b_assert(scans <= 1);
        return;
    }

    case N00B_PLAN_NODE_COMPLEMENT:
        n00b_assert(node->child != nullptr);
        _rocs_plan_audit(node->child);
        return;

    case N00B_PLAN_NODE_EMPTY:
        return;
    }

    n00b_assert(false);
}
#endif

// Walk a plan's index scans, reading what each matches. A leaf whose count
// cannot be read is left out rather than recorded as zero: zero means the term
// matches nothing, and a planner handed that for an unreadable count would
// short-circuit an intersect that should have run.
static bool
rocs_plan_collect_walk(n00b_plan_node_t    *node,
                       void                *shard,
                       bool                 mapped,
                       n00b_allocator_t    *allocator,
                       n00b_plan_cancel_fn  cancel_cb,
                       void                *cancel_ctx)
{
    if (node == nullptr) {
        return true;
    }
    if (cancel_cb != nullptr && cancel_cb(cancel_ctx)) {
        return false;
    }

    if (node->kind == N00B_PLAN_NODE_INDEX_SCAN && node->index != nullptr
        && node->key != nullptr) {
        n00b_store_index_keys_t *keys = n00b_plan_node_keys(node);
        // The catch-all resolves no keys but has a df of its own, read from
        // the value.
        auto catch_all_r = n00b_store_index_is_catch_all(node->index);
        if (keys == nullptr
            && !(n00b_result_is_ok(catch_all_r) && n00b_result_get(catch_all_r))) {
            rocs_plan_df_accumulate(node, 0, false);
            return true;
        }

        uint64_t df    = 0;
        bool     known = false;

        if (mapped) {
            auto df_r = n00b_store_index_df_mapped(
                node->index,
                (n00b_store_map_shard_t *)shard,
                node->key,
                .keys      = keys,
                .allocator = allocator);
            if (n00b_result_is_ok(df_r)) {
                df    = n00b_result_get(df_r);
                known = true;

                // A count of zero has two causes and the number does not say
                // which: the term is absent from a column that exists, or the
                // shard has no column for the field because it was sealed
                // before the field was declared indexed. The first is a real
                // zero. The second is a shard whose records must be scanned,
                // and treating it as zero settles an intersection whose rows
                // are there. Execution draws this line before acting on a
                // zero; a plan settled without it acts first.
                if (df == 0) {
                    auto present_r = n00b_store_index_present_mapped(
                        node->index,
                        (n00b_store_map_shard_t *)shard);
                    if (n00b_result_is_err(present_r)
                        || !n00b_result_get(present_r)) {
                        known = false;
                    }
                }
            }
        }
        else {
            auto df_r = n00b_store_index_df_hot(
                node->index,
                (n00b_store_shard_t *)shard,
                node->key,
                .keys      = keys,
                .allocator = allocator);
            if (n00b_result_is_ok(df_r)) {
                df    = n00b_result_get(df_r);
                known = true;

                if (df == 0) {
                    auto present_r = n00b_store_index_present_hot(
                        node->index,
                        (n00b_store_shard_t *)shard);
                    if (n00b_result_is_err(present_r)
                        || !n00b_result_get(present_r)) {
                        known = false;
                    }
                }
            }
        }

        rocs_plan_df_accumulate(node, df, known);
        return true;
    }

    if (!rocs_plan_collect_walk(node->child, shard, mapped, allocator,
                                cancel_cb, cancel_ctx)) {
        return false;
    }
    if (node->children != nullptr) {
        size_t len = n00b_list_len(*node->children);
        for (size_t i = 0; i < len; i++) {
            if (!rocs_plan_collect_walk(n00b_list_get(*node->children, i),
                                        shard, mapped, allocator,
                                        cancel_cb, cancel_ctx)) {
                return false;
            }
        }
    }
    return true;
}

// The same shape rocs_plan_collect_walk visits, asking only whether it would
// find anything to read.
static bool
rocs_plan_has_countable_leaf(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return false;
    }
    if (node->kind == N00B_PLAN_NODE_INDEX_SCAN && node->index != nullptr
        && node->key != nullptr) {
        return true;
    }
    if (rocs_plan_has_countable_leaf(node->child)) {
        return true;
    }
    if (node->children != nullptr) {
        size_t len = n00b_list_len(*node->children);
        for (size_t i = 0; i < len; i++) {
            if (rocs_plan_has_countable_leaf(n00b_list_get(*node->children,
                                                           i))) {
                return true;
            }
        }
    }
    return false;
}

// Settling acts only on INTERSECT and UNION, ordering their children and
// cutting an intersection with an empty operand. A plan with neither is left
// exactly as built, whatever the counts say.
static bool
rocs_plan_has_group(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return false;
    }
    if (node->kind == N00B_PLAN_NODE_INTERSECT
        || node->kind == N00B_PLAN_NODE_UNION) {
        return true;
    }
    return rocs_plan_has_group(node->child);
}

bool
n00b_plan_wants_counts(n00b_plan_node_t *plan)
{
    if (plan == nullptr || !n00b_plan_cost_enabled()) {
        return false;
    }
    return rocs_plan_has_group(plan) && rocs_plan_has_countable_leaf(plan);
}

/**
 * Fold one shard's counts into a plan.
 *
 * Called once per shard, so a partition's shards accumulate into one plan.
 * Reads the shard; the planner itself still touches none (plan.h rule 1), and
 * every read answers to the cancel hook.
 */
n00b_result_t(bool)
n00b_plan_collect_mapped(n00b_plan_node_t       *plan,
                         n00b_store_map_shard_t *shard) _kargs
{
    n00b_allocator_t    *allocator  = nullptr;
    n00b_plan_cancel_fn  cancel_cb  = nullptr;
    void                *cancel_ctx = nullptr;
}
{
    if (plan == nullptr || shard == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    if (!n00b_plan_cost_enabled()) {
        return n00b_result_ok(bool, true);
    }
#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_shards_collected, 1, memory_order_relaxed);
#endif

    if (!rocs_plan_collect_walk(plan, shard, true, allocator, cancel_cb,
                                cancel_ctx)) {
        return n00b_result_err(bool, N00B_PLAN_ERR_CANCELED);
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_plan_collect_hot(n00b_plan_node_t   *plan,
                      n00b_store_shard_t *shard) _kargs
{
    n00b_allocator_t    *allocator  = nullptr;
    n00b_plan_cancel_fn  cancel_cb  = nullptr;
    void                *cancel_ctx = nullptr;
}
{
    if (plan == nullptr || shard == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    if (!n00b_plan_cost_enabled()) {
        return n00b_result_ok(bool, true);
    }
#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_shards_collected, 1, memory_order_relaxed);
#endif

    if (!rocs_plan_collect_walk(plan, shard, false, allocator, cancel_cb,
                                cancel_ctx)) {
        return n00b_result_err(bool, N00B_PLAN_ERR_CANCELED);
    }
    return n00b_result_ok(bool, true);
}

/**
 * Decide what the counts decide, once, after every shard has been folded in.
 *
 * @p record_count is the total across the shards collected, because the
 * estimator turns a count into a fraction of the whole.
 */
n00b_result_t(bool)
n00b_plan_settle(n00b_plan_node_t *plan, uint64_t record_count) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (plan == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    _rocs_plan_build_ctx_t ctx = {
        .allocator    = allocator,
        .record_count = record_count,
    };

    _rocs_plan_settle_node(&ctx, plan);
    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_plan_node_t *)
n00b_plan_build(n00b_plan_predicate_t  *predicate,
                n00b_plan_index_list_t *indexes) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    bool              rewrite   = true;
}
{
#ifdef N00B_DEBUG
    atomic_fetch_add_explicit(&rocs_plans_built, 1, memory_order_relaxed);
#endif

    // Before index selection, because selection is per leaf: a duplicate leaf
    // picks the same index twice and a factored conjunct picks one once. The
    // rewrite reads no shard, so this stays inside rule 1.
    //
    // Idempotent, so the fan-out rewriting once up front and every build
    // rewriting again costs one walk that changes nothing, rather than a
    // second round of rewrites that could disagree with the first.
    if (rewrite) {
        auto rewritten_r = n00b_plan_rewrite(predicate,
                                             .allocator = allocator);
        if (n00b_result_is_err(rewritten_r)) {
            return n00b_result_err(n00b_plan_node_t *,
                                   n00b_result_get_err(rewritten_r));
        }
        predicate = n00b_result_get(rewritten_r);
    }

    _rocs_plan_build_ctx_t ctx = {
        .indexes   = indexes,
        .allocator = allocator,
    };
    auto plan_r = _rocs_plan_build_node(&ctx, predicate);

#ifdef N00B_DEBUG
    if (n00b_result_is_ok(plan_r)) {
        _rocs_plan_audit(n00b_result_get(plan_r));
    }
#endif

    return plan_r;
}

// ---------------------------------------------------------------------------
// Plan inspection. No shard, no data; a plan can be examined on its own.
// ---------------------------------------------------------------------------

n00b_result_t(n00b_plan_node_kind_t)
n00b_plan_node_kind(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return n00b_result_err(n00b_plan_node_kind_t, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(n00b_plan_node_kind_t, node->kind);
}

n00b_result_t(uint64_t)
n00b_plan_node_child_count(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return n00b_result_err(uint64_t, N00B_PLAN_ERR_ARG);
    }
    if (node->kind == N00B_PLAN_NODE_COMPLEMENT) {
        return n00b_result_ok(uint64_t, node->child == nullptr ? 0 : 1);
    }
    if (node->children == nullptr) {
        return n00b_result_ok(uint64_t, 0);
    }
    return n00b_result_ok(uint64_t, (uint64_t)n00b_list_len(*node->children));
}

n00b_result_t(n00b_option_t(n00b_plan_node_t *))
n00b_plan_node_child_at(n00b_plan_node_t *node, uint64_t index)
{
    if (node == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_node_t *),
                               N00B_PLAN_ERR_ARG);
    }
    if (node->kind == N00B_PLAN_NODE_COMPLEMENT) {
        if (index != 0 || node->child == nullptr) {
            return n00b_result_ok(n00b_option_t(n00b_plan_node_t *),
                                  n00b_option_none(n00b_plan_node_t *));
        }
        return n00b_result_ok(n00b_option_t(n00b_plan_node_t *),
                              n00b_option_set(n00b_plan_node_t *,
                                              node->child));
    }
    if (node->children == nullptr
        || index >= (uint64_t)n00b_list_len(*node->children)) {
        return n00b_result_ok(n00b_option_t(n00b_plan_node_t *),
                              n00b_option_none(n00b_plan_node_t *));
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_node_t *),
                          n00b_option_set(n00b_plan_node_t *,
                                          n00b_list_get(*node->children,
                                                        (size_t)index)));
}

static bool
_rocs_plan_tree_has(n00b_plan_node_t *node, n00b_plan_node_kind_t want)
{
    if (node == nullptr) {
        return false;
    }
    if (node->kind == want) {
        return true;
    }
    if (node->kind == N00B_PLAN_NODE_COMPLEMENT) {
        return _rocs_plan_tree_has(node->child, want);
    }
    if (node->children == nullptr) {
        return false;
    }
    size_t count = n00b_list_len(*node->children);
    for (size_t i = 0; i < count; i++) {
        if (_rocs_plan_tree_has(n00b_list_get(*node->children, i), want)) {
            return true;
        }
    }
    return false;
}

n00b_result_t(bool)
n00b_plan_uses_index(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(bool,
                          _rocs_plan_tree_has(node,
                                              N00B_PLAN_NODE_INDEX_SCAN));
}

n00b_result_t(bool)
n00b_plan_reads_no_records(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    return n00b_result_ok(bool,
                          !_rocs_plan_tree_has(node,
                                               N00B_PLAN_NODE_RECORD_SCAN));
}

static void
_rocs_plan_collect_record_scans(n00b_plan_node_t       *node,
                                n00b_plan_predicate_t **found,
                                uint64_t               *count)
{
    if (node == nullptr) {
        return;
    }
    if (node->kind == N00B_PLAN_NODE_RECORD_SCAN) {
        if (*count == 0) {
            *found = node->predicate;
        }
        *count += 1;
        return;
    }
    if (node->kind == N00B_PLAN_NODE_COMPLEMENT) {
        _rocs_plan_collect_record_scans(node->child, found, count);
        return;
    }
    if (node->children == nullptr) {
        return;
    }
    size_t n = n00b_list_len(*node->children);
    for (size_t i = 0; i < n; i++) {
        _rocs_plan_collect_record_scans(n00b_list_get(*node->children, i),
                                        found,
                                        count);
    }
}

n00b_result_t(n00b_option_t(n00b_plan_predicate_t *))
n00b_plan_sole_record_scan(n00b_plan_node_t *node)
{
    if (node == nullptr) {
        return n00b_result_err(n00b_option_t(n00b_plan_predicate_t *),
                               N00B_PLAN_ERR_ARG);
    }
    n00b_plan_predicate_t *found = nullptr;
    uint64_t               count = 0;
    _rocs_plan_collect_record_scans(node, &found, &count);
    if (count != 1) {
        return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                              n00b_option_none(n00b_plan_predicate_t *));
    }
    return n00b_result_ok(n00b_option_t(n00b_plan_predicate_t *),
                          n00b_option_set(n00b_plan_predicate_t *, found));
}

// ---------------------------------------------------------------------------
// Partition pruning.
//
// Answers which shards a predicate can possibly match, from the partition
// policy and each shard's route key. Both are catalog metadata, so this needs
// no shard contents.
// ---------------------------------------------------------------------------

struct n00b_plan_partition_filter_t {
    _rocs_plan_prune_t     prune;
    // Kept because the zone-map half of the decision is per shard: the routes
    // above are a function of the partition policy alone, while the bounds a
    // predicate is tested against belong to whichever shard is being
    // considered.
    n00b_plan_predicate_t *predicate;
    n00b_allocator_t      *allocator;
};

// ---------------------------------------------------------------------------
// Zone pruning.
//
// The routes above answer "is this shard in a partition the predicate can
// reach". This answers the question the partition key cannot: "does this
// shard's own data fall anywhere near what the predicate asks for". Under
// ingest-clock partitioning they are very different questions. Arrival
// buckets say nothing about event time, so routing leaves an event-time query
// unconstrained and every shard gets opened.
//
// Everything here is one-sided. `false` means proven impossible and is the
// only answer that skips a shard; every uncertainty answers `true` and costs
// a read. That is why an unknown leaf, a NOT, a missing zone map and an
// unorderable comparison all fall through to "may match".
// ---------------------------------------------------------------------------

// Whether `value` could lie inside a shard's recorded [min, max]. True
// whenever it cannot be ruled out.
static bool
_rocs_plan_zone_value_possible(n00b_json_node_t  *min,
                               n00b_json_node_t  *max,
                               n00b_plan_value_t  value)
{
    auto node_r = _rocs_plan_value_node(value);
    if (n00b_result_is_err(node_r)) {
        return true;
    }
    n00b_json_node_t *node = n00b_result_get(node_r);

    int32_t cmp = 0;
    auto    lo_r = _rocs_plan_json_order_cmp(node, min, &cmp);
    if (n00b_result_is_err(lo_r) || !n00b_result_get(lo_r)) {
        return true;
    }
    if (cmp < 0) {
        return false;
    }

    auto hi_r = _rocs_plan_json_order_cmp(node, max, &cmp);
    if (n00b_result_is_err(hi_r) || !n00b_result_get(hi_r)) {
        return true;
    }
    return cmp <= 0;
}

// Whether the leaf's requested interval overlaps a shard's recorded one.
//
// Two intervals miss each other when one ends before the other starts. The
// bound's own inclusivity decides only the touching case, where they meet at
// exactly one point.
static bool
_rocs_plan_zone_range_possible(n00b_json_node_t      *min,
                               n00b_json_node_t      *max,
                               n00b_plan_predicate_t *predicate)
{
    auto lower_r = _rocs_plan_value_node(predicate->lower);
    auto upper_r = _rocs_plan_value_node(predicate->upper);
    if (n00b_result_is_err(lower_r) || n00b_result_is_err(upper_r)) {
        return true;
    }

    // Asked-for lower bound against the shard's greatest value.
    int32_t cmp  = 0;
    auto    hi_r = _rocs_plan_json_order_cmp(n00b_result_get(lower_r),
                                             max,
                                             &cmp);
    if (n00b_result_is_err(hi_r) || !n00b_result_get(hi_r)) {
        return true;
    }
    if (predicate->include_lower ? cmp > 0 : cmp >= 0) {
        return false;
    }

    // Asked-for upper bound against the shard's least value.
    auto lo_r = _rocs_plan_json_order_cmp(n00b_result_get(upper_r), min, &cmp);
    if (n00b_result_is_err(lo_r) || !n00b_result_get(lo_r)) {
        return true;
    }
    return predicate->include_upper ? cmp >= 0 : cmp > 0;
}

static bool
_rocs_plan_zone_may_match(n00b_store_catalog_entry_t *entry,
                          n00b_plan_predicate_t      *predicate,
                          n00b_allocator_t           *allocator)
{
    if (entry == nullptr || predicate == nullptr) {
        return true;
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_FALSE:
        return false;

    case N00B_PLAN_PREDICATE_TRUE:
        return true;

    case N00B_PLAN_PREDICATE_AND: {
        // One impossible conjunct is enough: nothing the others match can
        // satisfy a condition this shard holds no value for.
        if (predicate->children == nullptr) {
            return true;
        }
        size_t len = n00b_list_len(*predicate->children);
        for (size_t i = 0; i < len; i++) {
            if (!_rocs_plan_zone_may_match(entry,
                                           n00b_list_get(*predicate->children,
                                                         i),
                                           allocator)) {
                return false;
            }
        }
        return true;
    }

    case N00B_PLAN_PREDICATE_OR: {
        // Every branch has to be impossible before the disjunction is.
        if (predicate->children == nullptr) {
            return true;
        }
        size_t len = n00b_list_len(*predicate->children);
        for (size_t i = 0; i < len; i++) {
            if (_rocs_plan_zone_may_match(entry,
                                          n00b_list_get(*predicate->children,
                                                        i),
                                          allocator)) {
                return true;
            }
        }
        return len == 0;
    }

    case N00B_PLAN_PREDICATE_NOT:
        // A shard holding no value in [lo, hi] is exactly a shard where the
        // negation holds everywhere, so a bound that rules the child out says
        // nothing about the parent.
        return true;

    case N00B_PLAN_PREDICATE_LEAF:
        break;
    }

    if (predicate->target == nullptr
        || predicate->target->kind != N00B_PLAN_TARGET_FIELD
        || predicate->target->field == nullptr) {
        return true;
    }

    switch (predicate->leaf_op) {
    case N00B_PLAN_LEAF_EQ:
    case N00B_PLAN_LEAF_IN:
    case N00B_PLAN_LEAF_RANGE:
        break;

    case N00B_PLAN_LEAF_EXISTS:
    case N00B_PLAN_LEAF_CONTAINS:
    case N00B_PLAN_LEAF_PREFIX:
    case N00B_PLAN_LEAF_SUBSTRING:
    case N00B_PLAN_LEAF_REGEX:
    case N00B_PLAN_LEAF_UNDER:
        // None of these is an interval. A prefix bounds a string range in
        // principle, but only under the same collation the bounds were
        // recorded with, and that is a stronger claim than this file can
        // make today.
        return true;
    }

    // Fetched once per leaf: an IN compares every candidate against the same
    // two bounds.
    n00b_json_node_t *min = nullptr;
    n00b_json_node_t *max = nullptr;

    auto have_r = n00b_store_catalog_entry_zone_bounds(entry,
                                                       predicate->target->field,
                                                       &min,
                                                       &max,
                                                       .allocator = allocator);
    if (n00b_result_is_err(have_r) || !n00b_result_get(have_r)) {
        return true;
    }

    if (predicate->leaf_op == N00B_PLAN_LEAF_RANGE) {
        return _rocs_plan_zone_range_possible(min, max, predicate);
    }
    if (predicate->leaf_op == N00B_PLAN_LEAF_EQ) {
        return _rocs_plan_zone_value_possible(min, max, predicate->value);
    }

    // IN: one candidate inside the interval keeps the shard.
    if (predicate->values == nullptr) {
        return true;
    }
    size_t len = n00b_list_len(*predicate->values);
    for (size_t i = 0; i < len; i++) {
        if (_rocs_plan_zone_value_possible(
                min,
                max,
                n00b_list_get(*predicate->values, i))) {
            return true;
        }
    }
    return len == 0;
}

n00b_result_t(n00b_plan_partition_filter_t *)
n00b_plan_partition_filter(n00b_store_t          *store,
                           n00b_plan_predicate_t *predicate) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (store == nullptr || predicate == nullptr) {
        return n00b_result_err(n00b_plan_partition_filter_t *,
                               N00B_PLAN_ERR_ARG);
    }

    auto policy_r = n00b_store_partition_policy_for_plan(store);
    if (n00b_result_is_err(policy_r)) {
        return n00b_result_err(n00b_plan_partition_filter_t *,
                               _rocs_plan_store_err(
                                   n00b_result_get_err(policy_r)));
    }

    n00b_string_t *partition_field = nullptr;
    auto field_r =
        n00b_store_partition_policy_field_for_plan(n00b_result_get(policy_r));
    if (n00b_result_is_err(field_r)) {
        return n00b_result_err(n00b_plan_partition_filter_t *,
                               _rocs_plan_store_err(
                                   n00b_result_get_err(field_r)));
    }
    n00b_option_t(n00b_string_t *) field_opt = n00b_result_get(field_r);
    if (n00b_option_is_set(field_opt)) {
        partition_field = n00b_option_get(field_opt);
    }

    _rocs_plan_prune_ctx_t prune_ctx = {
        .policy    = n00b_result_get(policy_r),
        .field     = partition_field,
        .allocator = allocator,
    };
    auto prune_r = _rocs_plan_prune_for_predicate(&prune_ctx, predicate);
    if (n00b_result_is_err(prune_r)) {
        return n00b_result_err(n00b_plan_partition_filter_t *,
                               n00b_result_get_err(prune_r));
    }

    n00b_plan_partition_filter_t *filter = n00b_alloc_with_opts(
        n00b_plan_partition_filter_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    filter->prune     = n00b_result_get(prune_r);
    filter->predicate = predicate;
    filter->allocator = allocator;
    return n00b_result_ok(n00b_plan_partition_filter_t *, filter);
}

n00b_result_t(bool)
n00b_plan_partition_may_match(n00b_plan_partition_filter_t *filter,
                              n00b_string_t                *partition_key)
{
    if (filter == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }
    return _rocs_plan_partition_may_match(filter->prune, partition_key);
}

n00b_result_t(bool)
n00b_plan_shard_may_match(n00b_plan_partition_filter_t *filter,
                          n00b_store_catalog_entry_t   *entry)
{
    if (filter == nullptr || entry == nullptr) {
        return n00b_result_err(bool, N00B_PLAN_ERR_ARG);
    }

    auto key_r = n00b_store_catalog_entry_get_partition_key(entry);
    if (n00b_result_is_err(key_r)) {
        return n00b_result_err(bool, N00B_PLAN_ERR_STATE);
    }

    // Routing first: it is a string compare against a list the filter already
    // holds, where the zone walk builds a JSON node per bound per leaf.
    auto route_r = _rocs_plan_partition_may_match(filter->prune,
                                                  n00b_result_get(key_r));
    if (n00b_result_is_err(route_r) || !n00b_result_get(route_r)) {
        return route_r;
    }

    return n00b_result_ok(bool,
                          _rocs_plan_zone_may_match(entry,
                                                    filter->predicate,
                                                    filter->allocator));
}

// ---------------------------------------------------------------------------
// Cost model.
//
// Every decision here is a function of numbers alone. Nothing reads a shard,
// which is what keeps rule 1 checkable: the executor supplies the counts it
// has already read and asks what to do with them. That split is also what
// makes the policy testable, since a table of inputs exercises it without
// building a store.
//
// The executor still owns the data questions, which need a shard to answer:
// what a posting count is, how many candidates a set holds.
// ---------------------------------------------------------------------------

// Cost of one step of each kind, in the units the predicate costs above use.
//
// NOT MEASURED. These are reasoned from the shape of each residency, and
// nothing in the tree derives or checks them:
//
//   A sealed image is a contiguous array, so walking it should beat walking a
//   hot list behind a lock. Searching it touches scattered pages that can
//   fault, so its search should cost more. The two run opposite ways, which is
//   the whole reason hot and mapped carry separate numbers. One unit per step
//   cannot express that, and would be wrong for one residency or the other.
//
// The ratios below encode that reasoning and no evidence, so treat the mapped
// coefficients as a starting shape. Deriving them needs a benchmark that
// reports walk and search separately against the same records in both
// residencies; bench_rocs_plan_ab runs both residencies but sums the two
// counters into one figure, so it cannot.
#define ROCS_COST_WALK_STEP_HOT      10
#define ROCS_COST_WALK_STEP_MAPPED    7
#define ROCS_COST_SEARCH_STEP_HOT    20
#define ROCS_COST_SEARCH_STEP_MAPPED 52
// Also unmeasured. Enumerating a set loads each 64-bit word once and takes a
// count-trailing-zeros per member, so a word is priced as one hot walk step.
#define ROCS_COST_BITMAP_WORD        10

static _Atomic(int) rocs_plan_cost_state = -1;

bool
n00b_plan_cost_enabled(void)
{
    int state = atomic_load_explicit(&rocs_plan_cost_state,
                                     memory_order_relaxed);
    if (state < 0) {
        state = getenv("ROCS_PLAN_NO_COST") != nullptr ? 0 : 1;
        atomic_store_explicit(&rocs_plan_cost_state,
                              state,
                              memory_order_relaxed);
    }
    return state != 0;
}

void
n00b_plan_cost_set_enabled(bool enabled)
{
    atomic_store_explicit(&rocs_plan_cost_state,
                          enabled ? 1 : 0,
                          memory_order_relaxed);
}

// Relative cost of testing one leaf against one record, in the same abstract
// units as the scan steps above. Ratios are what matter: a regex over text is
// tens of times an integer compare.
//
// Named rather than inline so calibration has one table to replace.
#define ROCS_COST_FIELD_LOOKUP  1
#define ROCS_COST_SCALAR_EQ     1
#define ROCS_COST_STRING_EQ     3
#define ROCS_COST_RANGE         2
#define ROCS_COST_PREFIX        5
#define ROCS_COST_UNDER         7
#define ROCS_COST_SUBSTRING     11
#define ROCS_COST_CONTAINS      11
#define ROCS_COST_REGEX         40

// What fraction of a shard each leaf is expected to keep, per thousand.
//
// Three of these are the index layer's own advertised hints (index.c: 0.10 for
// a term equality, 0.05 for a full-text contains, 0.20 for an n-gram prefix),
// restated here in the planner's units. Taking them from there rather than
// inventing a second set means a future change to what the index believes
// about a term lookup moves both numbers at once.
//
// The others are reasoned from shape, and the ordering only needs them to be
// right relative to each other. An existence test keeps nearly every record,
// which is the whole reason cost alone is the wrong rank. A range keeps more
// than an equality and much less than an existence test. A path containment
// behaves like a range: it names a subtree rather than a value.
//
// NOT MEASURED. Nothing in the tree derives or checks them, exactly as with
// the scan-step costs above.
#define ROCS_SEL_EXISTS        900
#define ROCS_SEL_EQ            100
#define ROCS_SEL_RANGE         300
#define ROCS_SEL_PREFIX        200
#define ROCS_SEL_UNDER         300
#define ROCS_SEL_SUBSTRING     200
#define ROCS_SEL_CONTAINS       50
#define ROCS_SEL_REGEX         200

// Children one call to n00b_plan_cost_order_children can hold costs for. Its
// callers cap at 64 already (eval.c's ROCS_ORDER_MAX, and the merged record
// scans in _rocs_plan_build_nary); a group past it is evaluated in plan order.
#define ROCS_COST_ORDER_MAX     64

static uint64_t
rocs_cost_value_compare(n00b_plan_value_t value)
{
    auto node_r = _rocs_plan_value_node(value);
    if (n00b_result_is_err(node_r)) {
        return ROCS_COST_STRING_EQ;
    }
    n00b_json_node_t *node = n00b_result_get(node_r);
    if (node == nullptr) {
        return ROCS_COST_STRING_EQ;
    }
    switch (n00b_json_type(node)) {
    case N00B_JSON_NULL:
    case N00B_JSON_BOOL:
    case N00B_JSON_INT:
    case N00B_JSON_DOUBLE:
        return ROCS_COST_SCALAR_EQ;
    default:
        // Strings compare by unicode equality; arrays and objects recurse.
        return ROCS_COST_STRING_EQ;
    }
}

uint64_t
n00b_plan_cost_predicate(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return 0;
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_FALSE:
    case N00B_PLAN_PREDICATE_TRUE:
        return 0;

    case N00B_PLAN_PREDICATE_NOT:
        return n00b_plan_cost_predicate(predicate->child);

    case N00B_PLAN_PREDICATE_AND:
    case N00B_PLAN_PREDICATE_OR: {
        // The whole group, not the cheapest member: a group is only reached by
        // a caller who will evaluate it, and short-circuiting is what the
        // ordering below is for rather than something to assume here.
        uint64_t total = 0;
        if (predicate->children != nullptr) {
            size_t len = n00b_list_len(*predicate->children);
            for (size_t i = 0; i < len; i++) {
                total += n00b_plan_cost_predicate(
                    n00b_list_get(*predicate->children, i));
            }
        }
        return total;
    }

    case N00B_PLAN_PREDICATE_LEAF:
        break;
    }

    uint64_t cost = ROCS_COST_FIELD_LOOKUP;
    switch (predicate->leaf_op) {
    case N00B_PLAN_LEAF_EXISTS:
        return cost;
    case N00B_PLAN_LEAF_EQ:
        return cost + rocs_cost_value_compare(predicate->value);
    case N00B_PLAN_LEAF_RANGE:
        return cost + ROCS_COST_RANGE;
    case N00B_PLAN_LEAF_IN: {
        // One comparison per candidate value, short-circuiting on a match.
        uint64_t n = predicate->values == nullptr
                       ? 0
                       : (uint64_t)n00b_list_len(*predicate->values);
        return cost + n * ROCS_COST_SCALAR_EQ;
    }
    case N00B_PLAN_LEAF_PREFIX:
        return cost + ROCS_COST_PREFIX;
    case N00B_PLAN_LEAF_UNDER:
        return cost + ROCS_COST_UNDER;
    case N00B_PLAN_LEAF_SUBSTRING:
        return cost + ROCS_COST_SUBSTRING;
    case N00B_PLAN_LEAF_CONTAINS:
        return cost + ROCS_COST_CONTAINS;
    case N00B_PLAN_LEAF_REGEX:
        return cost + ROCS_COST_REGEX;
    }
    return cost;
}

uint32_t
n00b_plan_cost_selectivity(n00b_plan_predicate_t *predicate)
{
    if (predicate == nullptr) {
        return N00B_PLAN_SEL_SCALE;
    }

    switch (predicate->kind) {
    case N00B_PLAN_PREDICATE_FALSE:
        return 0;

    case N00B_PLAN_PREDICATE_TRUE:
        return N00B_PLAN_SEL_SCALE;

    case N00B_PLAN_PREDICATE_NOT:
        return N00B_PLAN_SEL_SCALE
             - n00b_plan_cost_selectivity(predicate->child);

    case N00B_PLAN_PREDICATE_AND: {
        // The narrowest operand, which is the bound n00b_plan_cost_intersect_size
        // applies to sizes. Multiplying the conjuncts would assume they are
        // independent, which reads as narrower than the truth whenever the
        // fields correlate.
        uint32_t narrowest = N00B_PLAN_SEL_SCALE;
        if (predicate->children != nullptr) {
            size_t len = n00b_list_len(*predicate->children);
            for (size_t i = 0; i < len; i++) {
                uint32_t child = n00b_plan_cost_selectivity(
                    n00b_list_get(*predicate->children, i));
                if (child < narrowest) {
                    narrowest = child;
                }
            }
        }
        return narrowest;
    }

    case N00B_PLAN_PREDICATE_OR: {
        // The sum, capped at the shard, matching n00b_plan_cost_union_size.
        uint32_t total = 0;
        if (predicate->children != nullptr) {
            size_t len = n00b_list_len(*predicate->children);
            for (size_t i = 0; i < len; i++) {
                total += n00b_plan_cost_selectivity(
                    n00b_list_get(*predicate->children, i));
                if (total >= N00B_PLAN_SEL_SCALE) {
                    return N00B_PLAN_SEL_SCALE;
                }
            }
        }
        return total;
    }

    case N00B_PLAN_PREDICATE_LEAF:
        break;
    }

    switch (predicate->leaf_op) {
    case N00B_PLAN_LEAF_EXISTS:
        return ROCS_SEL_EXISTS;
    case N00B_PLAN_LEAF_EQ:
        return ROCS_SEL_EQ;
    case N00B_PLAN_LEAF_IN: {
        // One equality per value, unioned, so the same cap a disjunction of
        // them would reach.
        uint64_t n = predicate->values == nullptr
                       ? 0
                       : (uint64_t)n00b_list_len(*predicate->values);
        uint64_t total = n * ROCS_SEL_EQ;
        return total >= N00B_PLAN_SEL_SCALE ? N00B_PLAN_SEL_SCALE
                                            : (uint32_t)total;
    }
    case N00B_PLAN_LEAF_RANGE:
        return ROCS_SEL_RANGE;
    case N00B_PLAN_LEAF_PREFIX:
        return ROCS_SEL_PREFIX;
    case N00B_PLAN_LEAF_UNDER:
        return ROCS_SEL_UNDER;
    case N00B_PLAN_LEAF_SUBSTRING:
        return ROCS_SEL_SUBSTRING;
    case N00B_PLAN_LEAF_CONTAINS:
        return ROCS_SEL_CONTAINS;
    case N00B_PLAN_LEAF_REGEX:
        return ROCS_SEL_REGEX;
    }

    return N00B_PLAN_SEL_SCALE;
}

// Whether `a` is likelier to end the group per unit of cost than `b`.
//
// The rank is (chance this child ends the group) / cost, compared by
// cross-multiplication so the arithmetic stays in integers. A division would
// round two close ranks to the same value and let the sort keep whichever it
// saw first. Both factors are bounded, the chance by N00B_PLAN_SEL_SCALE and
// the cost by the leaf table, so the products cannot overflow.
//
// A zero cost would make the rank infinite; only TRUE, FALSE and an empty
// group price at zero, and clamping to one keeps them ahead of everything
// without a special case.
static bool
rocs_cost_rank_prefers(uint32_t stop_a,
                       uint64_t cost_a,
                       uint32_t stop_b,
                       uint64_t cost_b)
{
    uint64_t a = cost_a == 0 ? 1 : cost_a;
    uint64_t b = cost_b == 0 ? 1 : cost_b;
    return (uint64_t)stop_a * b > (uint64_t)stop_b * a;
}

size_t
n00b_plan_cost_order_children(n00b_plan_predicate_t *predicate,
                              uint16_t              *order,
                              size_t                 cap)
{
    if (predicate == nullptr || order == nullptr || cap == 0
        || predicate->children == nullptr) {
        return 0;
    }
    size_t len = n00b_list_len(*predicate->children);
    if (len > cap || len > ROCS_COST_ORDER_MAX) {
        return 0;
    }

    // Costed once each, up front. A child's cost is a walk of its whole
    // subtree, so pricing it inside the comparison below would charge that
    // walk once per comparison rather than once per child, and a group of
    // nested children would pay the subtree size times the square of the
    // group size to sort numbers that never change.
    uint64_t costs[ROCS_COST_ORDER_MAX];
    uint32_t stops[ROCS_COST_ORDER_MAX];
    for (size_t i = 0; i < len; i++) {
        n00b_plan_predicate_t *child = n00b_list_get(*predicate->children, i);
        order[i]   = (uint16_t)i;
        costs[i]   = n00b_plan_cost_predicate(child);
        // How much of the shard this child throws out. A disjunction stops at
        // its first true rather than its first false, so what ends it soonest
        // is the child that keeps the most; reading the rank off the other end
        // for an OR is what makes one comparison serve both.
        uint32_t sel = n00b_plan_cost_selectivity(child);
        stops[i]     = predicate->kind == N00B_PLAN_PREDICATE_OR
                         ? sel
                         : (uint32_t)(N00B_PLAN_SEL_SCALE - sel);
    }

    // Insertion sort, stable, so children of equal cost keep the order the
    // caller wrote them in. Groups are small and this runs once per scan.
    for (size_t i = 1; i < len; i++) {
        uint16_t v = order[i];
        size_t   j = i;
        while (j > 0
               && rocs_cost_rank_prefers(stops[v],
                                         costs[v],
                                         stops[order[j - 1]],
                                         costs[order[j - 1]])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    return len;
}

uint64_t
n00b_plan_cost_intersect_size(uint64_t a, uint64_t b)
{
    // An intersection is no larger than its smallest operand. A true bound,
    // not an estimate: assuming the operands are independent would give
    // a * b / record_count, which is smaller and wrong whenever they
    // correlate, and correlated fields are the normal case in event data.
    return a < b ? a : b;
}

uint64_t
n00b_plan_cost_union_size(uint64_t a, uint64_t b, uint64_t record_count)
{
    // A union is no larger than the sum of its operands, and no larger than
    // the shard. Saturating, because two counts near the maximum are a bad
    // reason to report a union of nothing.
    uint64_t sum = (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
    return sum < record_count ? sum : record_count;
}

uint64_t
n00b_plan_cost_complement_df(uint64_t child_df, uint64_t record_count)
{
    // A complement holds every record its child does not, so a selective child
    // makes a broad complement. Ordering a group without this reads a negated
    // leaf as unknown and can run the widest operand of an intersect first,
    // which is the ordering exactly inverted.
    return child_df >= record_count ? 0 : record_count - child_df;
}

bool
n00b_plan_cost_term_covers_shard(bool lossy, uint64_t df, uint64_t record_count)
{
    // Only a lossy scan. Declining an exact one means reading records instead
    // of walking postings, which is more work however broad the term is.
    return lossy && record_count != 0 && df >= record_count;
}

bool
n00b_plan_cost_probe_possible(uint64_t candidates, uint64_t record_count)
{
    if (candidates == 0 || record_count == 0) {
        return false;
    }
    uint64_t steps = 64 - (uint64_t)__builtin_clzll(record_count);
    if (steps == 0 || candidates > UINT64_MAX / steps) {
        return false;
    }
    return candidates * steps < record_count;
}

uint64_t
n00b_plan_cost_bitmap_walk(uint64_t record_count, bool ordinals_cached)
{
    // One read per 64-bit word of the set. A word is the unit because that is
    // what enumerating a bitmap actually reads.
    return ordinals_cached ? 0 : (record_count / 64) * ROCS_COST_BITMAP_WORD;
}

uint64_t
n00b_plan_cost_walk_step(bool mapped)
{
    return mapped ? ROCS_COST_WALK_STEP_MAPPED : ROCS_COST_WALK_STEP_HOT;
}

uint64_t
n00b_plan_cost_search_step(bool mapped)
{
    return mapped ? ROCS_COST_SEARCH_STEP_MAPPED : ROCS_COST_SEARCH_STEP_HOT;
}

bool
n00b_plan_cost_probe_beats_walk(uint64_t df,
                                uint64_t candidates,
                                uint64_t bitmap_walk,
                                uint64_t terms,
                                bool     mapped,
                                bool     searchable)
{
    if (df == 0 || candidates == 0 || terms == 0) {
        return false;
    }
    // The arithmetic below prices a membership test at ceil(log2(df)) steps,
    // which is what a binary search costs. A posting list that cannot be
    // searched is scanned, at df steps, and the two diverge fastest exactly
    // where this would otherwise say yes: a wide term against a narrow
    // candidate set. Walking is then cheaper than the probe by the same
    // margin the estimate claimed for the probe.
    if (!searchable) {
        return false;
    }
    // One search per term per candidate. A membership test asks every term of
    // the lookup whether it carries the ordinal, so a value reducing to
    // several tokens costs that many searches rather than one.
    uint64_t steps = 64 - (uint64_t)__builtin_clzll(df);
    if (steps == 0 || terms > UINT64_MAX / steps) {
        return false;
    }
    uint64_t per_candidate = terms * steps * n00b_plan_cost_search_step(mapped);
    if (candidates > UINT64_MAX / per_candidate) {
        return false;
    }
    uint64_t probe_cost = candidates * per_candidate;
    if (UINT64_MAX - probe_cost < bitmap_walk) {
        return false;
    }

    uint64_t walk_step = n00b_plan_cost_walk_step(mapped);
    if (df > UINT64_MAX / walk_step) {
        return true;
    }
    // `df` understates the walk for a multi-term lookup, which resolves every
    // term rather than only the smallest. Leaving it understated biases toward
    // walking, which is the direction that cannot answer wrongly.
    return probe_cost + bitmap_walk < df * walk_step;
}

bool
n00b_plan_cost_prefers(uint64_t bound,
                       uint64_t cost,
                       uint64_t best,
                       uint64_t best_cost,
                       bool     widest,
                       bool     have_pick)
{
    // Size decides, and execution cost breaks the tie in the same direction.
    // For a leaf the two are one number; above a leaf they diverge, and a
    // complement is the case that needs the second: it is as wide as its child
    // is narrow while costing exactly what that child costs.
    return !have_pick
        || (widest ? bound > best : bound < best)
        || (bound == best && (widest ? cost > best_cost : cost < best_cost));
}
