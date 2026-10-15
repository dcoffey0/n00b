/**
 * @file rocs/index.h
 * @brief Index descriptors and posting-view declarations for rocs.
 *
 * Indexes are query accelerators. They may make a predicate faster, but query
 * correctness must not depend on any index being present or able to serve a
 * predicate. The planner treats advertisements as hints and verifies residual
 * predicates through record views when needed.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/alloc.h"
#include "core/string.h"
#include "rocs/shard.h"

typedef struct n00b_store_index_t    n00b_store_index_t;
typedef struct n00b_store_postings_t n00b_store_postings_t;
typedef struct n00b_store_record_t   n00b_store_record_t;
typedef struct n00b_store_map_shard_t n00b_store_map_shard_t;

/**
 * @brief Stable record identity independent of process pointers.
 */
typedef struct {
    uint64_t shard_id;
    uint64_t ordinal;
    uint64_t generation;
    // Wall-clock seal timestamp (epoch ns) of the shard this position points
    // into. shard_id/ordinal/generation are position keys that reset when the
    // local store is rebuilt; seal_ts never resets or reuses, so it anchors a
    // resume across a shard-id rewind. Zero when unknown (e.g. a hot-tail
    // position or a legacy token predating this field), in which case resume
    // falls back to position-only semantics.
    uint64_t seal_ts;
} n00b_store_pos_t;

/**
 * @brief Public index kind tag.
 *
 * Dispatch is by this scalar tag and process-side implementation code. rocs
 * never marshals function pointers into shard images or index tables.
 */
typedef enum : int32_t {
    N00B_STORE_INDEX_NONE     = 0,
    N00B_STORE_INDEX_TERM     = 1,
    N00B_STORE_INDEX_FULLTEXT = 2,
    N00B_STORE_INDEX_NGRAM    = 3,
    N00B_STORE_INDEX_NUMERIC  = 4,
    N00B_STORE_INDEX_BOOL     = 5,
    N00B_STORE_INDEX_VECTOR   = 6,
} n00b_store_index_kind_t;

/** @brief Smallest configurable n-gram byte width accepted by rocs. */
#define N00B_STORE_NGRAM_MIN_N ((uint8_t)2)

/** @brief Default n-gram byte width used by rocs text indexes. */
#define N00B_STORE_NGRAM_DEFAULT_N ((uint8_t)3)

/** @brief Largest configurable n-gram byte width accepted by rocs. */
#define N00B_STORE_NGRAM_MAX_N ((uint8_t)16)

/**
 * @brief Predicate operator codes accepted by index advertisement.
 *
 * These values are the public contract for @ref n00b_store_index_advertise.
 * Planner and filter layers must map their own predicate enums to this enum
 * before asking an index for an acceleration hint.
 *
 * @c N00B_STORE_INDEX_OP_UNSPECIFIED preserves the legacy term-index default:
 * a matching term descriptor may advertise equality acceleration. Full-text
 * descriptors require an explicit @c N00B_STORE_INDEX_OP_CONTAINS because a
 * contains predicate has different correctness rules from exact equality.
 * N-gram descriptors advertise @c N00B_STORE_INDEX_OP_PREFIX as candidate
 * acceleration only. The planner may reuse that candidate contract for any
 * leaf it can reduce to a required literal, a regex among them, but callers
 * must verify the original residual predicate before treating candidates as
 * hits.
 */
typedef enum : int32_t {
    N00B_STORE_INDEX_OP_UNSPECIFIED = 0,
    N00B_STORE_INDEX_OP_EQ          = 1,
    N00B_STORE_INDEX_OP_CONTAINS    = 5,
    N00B_STORE_INDEX_OP_PREFIX      = 6,
} n00b_store_index_op_t;

/**
 * @brief Error domain for index and posting-view operations.
 */
typedef enum : int32_t {
    N00B_STORE_INDEX_OK          = 0,
    N00B_STORE_INDEX_ERR_ARG     = -1,
    N00B_STORE_INDEX_ERR_STATE   = -2,
    N00B_STORE_INDEX_ERR_KIND    = -3,
    N00B_STORE_INDEX_ERR_UNREADY = -4,
    N00B_STORE_INDEX_ERR_INTERNAL = -5,
} n00b_store_index_err_t;

/**
 * @brief The map-layer error most recently collapsed into an index error, on
 *        this thread.
 *
 * n00b#403: the index error enum is public and narrower than the map's, so
 * rocs_index_map_err() folds nine distinct @c N00B_STORE_MAP_ERR_* codes into
 * @c N00B_STORE_INDEX_ERR_STATE. That makes a @c -2 ambiguous between an I/O
 * failure, bad magic, a version or layout mismatch, a range, schema, backing
 * or cache error -- and a shard that is simply not sealed.
 *
 * This reports which one, for a log line or a breakpoint. Thread-local: on a
 * store with hundreds of shards, concurrent queries would otherwise overwrite
 * each other's.
 *
 * @return The @c N00B_STORE_MAP_ERR_* code, or 0 if none has been recorded.
 * @note Diagnostic only. The value returned by the failing call is unchanged;
 *       do not branch on this.
 */
extern n00b_err_t n00b_store_index_last_map_err(void);

/**
 * @brief The shard state that most recently produced a not-sealed rejection,
 *        on this thread.
 *
 * The other source of a @c -2, and the one n00b#403 hits: a shard whose state
 * is not @c N00B_SHARD_STATE_SEALED. It does not pass through
 * rocs_index_map_err(), so @c n00b_store_index_last_map_err() alone cannot
 * distinguish it.
 *
 * @return 0 OPEN / 1 SEALED / 2 DROPPED, or -1 if none has been recorded.
 * @note Diagnostic only.
 */
extern int n00b_store_index_last_unsealed_state(void);

/** @brief Reset both diagnostics for this thread. */
extern void n00b_store_index_clear_last_map_err(void);

/**
 * @brief Name of a @c N00B_STORE_MAP_ERR_* code.
 *
 * Static storage, no allocation, so it is safe from a crash path and callable
 * from a debugger.
 */
extern const char *n00b_store_map_err_name(n00b_err_t err);

/**
 * @brief One posting result.
 *
 * The record member is a shard-aware view handle. It is never a raw mapped JSON
 * pointer; public callers may inspect only its durable position in this phase.
 * Internal planner verification resolves it through rocs-private record-view
 * helpers.
 */
typedef struct {
    n00b_store_pos_t     pos;
    n00b_store_record_t *record;
} n00b_store_posting_t;

/**
 * @brief Planner-facing index advertisement.
 *
 * Advertisements are hints only. An incorrect hint may change performance, but
 * it must not change query semantics.
 */
typedef struct {
    bool                    accelerates;
    n00b_store_index_kind_t kind;
    double                  selectivity_hint;
} n00b_store_advert_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Static diagnostic string for an index error code.
 *
 * @param err A @c N00B_STORE_INDEX_* code, usually from a result error branch.
 * @return A n00b string naming the code, or @c UNKNOWN for an unrecognized
 *         value.
 */
extern n00b_string_t *n00b_store_index_err_str(n00b_err_t err);

/**
 * @brief Construct a process-side index descriptor.
 *
 * @param field Field this index is configured to serve. The pointer is
 *              retained, not copied.
 * @param kind  Scalar index kind tag.
 * @kw ngram_n N-gram byte width for @c N00B_STORE_INDEX_NGRAM descriptors.
 *             Defaults to @c N00B_STORE_NGRAM_DEFAULT_N. Non-NGRAM
 *             descriptors must use the default value.
 * @kw postings Physical posting representation for this descriptor. Defaults
 *              to sparse ordinal lists.
 * @kw allocator Allocator for the process-side descriptor.
 *
 * @return Ok(index) for recognized non-NONE kinds. Returns
 *         @c N00B_STORE_INDEX_ERR_ARG for null field and
 *         invalid n-gram sizing, and @c N00B_STORE_INDEX_ERR_KIND for unknown
 *         or NONE kinds.
 *
 * The returned descriptor contains no function pointers and is not shard
 * marshal state.
 */
extern n00b_result_t(n00b_store_index_t *)
n00b_store_index_new(n00b_string_t          *field,
                     n00b_store_index_kind_t kind) _kargs
{
    uint8_t                     ngram_n   = N00B_STORE_NGRAM_DEFAULT_N;
    n00b_store_postings_kind_t  postings  = N00B_STORE_POSTINGS_SPARSE;
    n00b_allocator_t           *allocator = nullptr;
};

/**
 * @brief Read the scalar kind tag from an index descriptor.
 *
 * @param index Index descriptor returned by @c n00b_store_index_new.
 * @return Ok(kind) on success, or @c N00B_STORE_INDEX_ERR_ARG for a null
 *         descriptor.
 */
extern n00b_result_t(n00b_store_index_kind_t)
n00b_store_index_kind(n00b_store_index_t *index);

/**
 * @brief Read the configured n-gram byte width from an index descriptor.
 *
 * @param index Index descriptor returned by @c n00b_store_index_new.
 * @return Ok(width) for @c N00B_STORE_INDEX_NGRAM descriptors. Returns
 *         @c N00B_STORE_INDEX_ERR_ARG for null descriptors and
 *         @c N00B_STORE_INDEX_ERR_KIND for non-NGRAM descriptors.
 */
extern n00b_result_t(uint8_t)
n00b_store_index_ngram_n(n00b_store_index_t *index);

/**
 * @brief Read the configured posting representation from an index descriptor.
 *
 * @param index Index descriptor returned by @c n00b_store_index_new.
 * @return Ok(representation) on success, or @c N00B_STORE_INDEX_ERR_ARG for a
 *         null descriptor.
 */
extern n00b_result_t(n00b_store_postings_kind_t)
n00b_store_index_postings_kind(n00b_store_index_t *index);

/**
 * @brief Borrow the field configured on an index descriptor.
 *
 * @param index Index descriptor returned by @c n00b_store_index_new.
 * @return Ok(field) on success. The returned pointer is borrowed from the
 *         descriptor. Returns @c N00B_STORE_INDEX_ERR_ARG for a null descriptor
 *         or malformed descriptor with no field.
 */
extern n00b_result_t(n00b_string_t *)
n00b_store_index_field(n00b_store_index_t *index);

/**
 * @brief Ask an index whether it can accelerate a field/operator pair.
 *
 * Term descriptors advertise equality acceleration for their configured field.
 * Full-text descriptors advertise only named-field whole-token contains
 * acceleration. N-gram descriptors advertise named-field prefix candidate
 * acceleration; the planner must retain and verify the original
 * residual predicate. Unsupported operator/kind combinations return a
 * non-accelerating hint so the planner can scan and verify.
 *
 * @param index Index descriptor to inspect.
 * @param field Field being planned.
 * @param op    One of @ref n00b_store_index_op_t. Unknown operator codes return
 *              a non-accelerating hint. @c N00B_STORE_INDEX_OP_UNSPECIFIED is
 *              accepted only for the legacy term equality default.
 * @return Planner hint. Invalid inputs and field mismatches return a
 *         non-accelerating @c N00B_STORE_INDEX_NONE hint. A matching
 *         @c N00B_STORE_INDEX_TERM equality descriptor,
 *         @c N00B_STORE_INDEX_FULLTEXT contains descriptor, or
 *         @c N00B_STORE_INDEX_NGRAM prefix descriptor returns an
 *         accelerating hint with a heuristic selectivity. N-gram hints are
 *         never exact-answer contracts.
 */
extern n00b_store_advert_t
n00b_store_index_advertise(n00b_store_index_t *index,
                           n00b_string_t      *field,
                           int64_t             op);

/**
 * @brief Add one hot shard record to an index.
 *
 * @param index          Index descriptor. Hot shards support
 *                       @c N00B_STORE_INDEX_TERM,
 *                       @c N00B_STORE_INDEX_FULLTEXT, and
 *                       @c N00B_STORE_INDEX_NGRAM.
 * @param shard          Open hot shard containing the record.
 * @param record_ordinal Stable ordinal returned by @c n00b_store_shard_append.
 * @kw allocator         Allocator for normalization and hash scratch. Durable
 *                       column/posting storage remains owned by @p shard.
 *
 * @note Ordinals may arrive in any order. Every one appends in constant time
 *       whatever the order, so no arrival pattern makes indexing quadratic.
 *
 *       What arrival order decides is where the sort lands. A posting list is
 *       ascending until an ordinal arrives below its largest, which clears the
 *       list's order flag; readers binary-search a list that carries the flag
 *       and scan one that does not, and sealing sorts what reaches it so every
 *       image is written ascending. Indexing in append order therefore leaves
 *       the flag set and every hot read logarithmic. Indexing out of order
 *       answers identically and reads linearly on the hot shard until the
 *       shard is sealed, and it declines the query planner's index probe,
 *       which prices a membership test as a search.
 *
 * @return Ok(number of normalized terms appended). Records missing the
 *         descriptor's field, non-text values in text indexes, or fields that
 *         normalize to no terms return Ok(0). Unsupported index kinds return
 *         @c N00B_STORE_INDEX_ERR_UNREADY.
 *
 * The index descriptor's field is the single source of truth; this call does
 * not accept a second field argument. Hot insertion mutates only the shard's
 * typed column/posting containers.
 *
 * @warning Single writer per shard. Posting lists take no lock, so two calls
 *          on one shard must not overlap, and neither may a call overlap a
 *          store ingest into that shard. Readers may run alongside. Debug
 *          builds abort on an overlapping writer.
 */
extern n00b_result_t(uint64_t)
n00b_store_index_add(n00b_store_index_t *index,
                     n00b_store_shard_t *shard,
                     uint64_t            record_ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Look up a value in an open hot shard index.
 *
 * @param index Index descriptor whose field should be searched.
 * @param shard Open hot shard.
 * @param value Query JSON value. Term indexes accept scalar or composite JSON
 *              values; composites are flattened and matched as a conjunction
 *              of scalar terms under their normalized paths. Full-text indexes
 *              accept string values that normalize to at least one text lookup
 *              term; when normalization emits a folded full-string term plus
 *              split tokens, the first term is the lookup key. N-gram indexes
 *              accept only string values that produce at least one configured
 *              n-gram, and return candidate postings only.
 * @kw allocator Allocator for the returned posting view and record handles.
 *
 * @return Ok(postings). Misses are successful empty posting views. Invalid
 *         inputs, sealed hot shards, malformed index state, unsupported index
 *         kinds, non-text text-index query values, empty full-text query values,
 *         or too-short n-gram query values return typed index errors.
 *
 * @note For term indexes, a shard that does not carry the index field returns
 *       an empty view without normalizing @p value, so a malformed term query
 *       against such a shard is not reported as an error. Full-text and n-gram
 *       queries are normalized, and still report the errors above, unless the
 *       caller supplies resolved keys: resolving them ran the same
 *       normalization and reported the same errors then.
 */
extern n00b_result_t(n00b_store_postings_t *)
n00b_store_index_lookup(n00b_store_index_t *index,
                        n00b_store_shard_t *shard,
                        n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    // Opaque here; internal/rocs/index.h defines it. Callers that
    // already resolved the value hand it over so the walk does not
    // normalize and hash it again on every shard it visits.
    struct n00b_store_index_keys_t *keys = nullptr;
};

/**
 * @brief Look up a value in a sealed shard through mapped views.
 *
 * @param index Index descriptor whose field should be searched.
 * @param shard Borrowed mapped shard view for a sealed shard.
 * @param value Query JSON value. Term indexes accept scalar or composite JSON
 *              values; composites are flattened and matched as a conjunction
 *              of scalar terms under their normalized paths. Full-text and
 *              catch-all full-text descriptors accept string queries with at
 *              least one normalized lookup term, using the first term when a
 *              folded full-string term and split tokens are both produced.
 *              N-gram descriptors accept string queries that produce at least
 *              one configured n-gram and return candidates only.
 * @kw allocator Allocator for the returned posting view and record handles.
 *
 * @pre @p shard must name a sealed shard image opened through rocs map APIs.
 *      Mapped bytes must stay resident for the lifetime of returned record
 *      view handles.
 *
 * @return Ok(postings). Misses are successful empty posting views. Invalid
 *         inputs, non-sealed mapped shards, malformed mapped index state,
 *         unsupported index kinds, non-text text-index query values,
 *         empty full-text query values, or too-short n-gram query values return
 *         typed index errors.
 *
 * @note For term indexes, a shard that does not carry the index field returns
 *       an empty view without normalizing @p value, so a malformed term query
 *       against such a shard is not reported as an error. Full-text and n-gram
 *       queries are normalized, and still report the errors above, unless the
 *       caller supplies resolved keys: resolving them ran the same
 *       normalization and reported the same errors then.
 *
 * @post Returned record handles are shard-aware mapped views. They do not
 *       expose raw mapped JSON pointers to callers.
 *
 * This is the sealed-image counterpart to @c n00b_store_index_lookup. It uses
 * resolver-aware mapped dict/list APIs, resolves internal vaddrs, compares the
 * mapped field and term keys, and never unmarshals shard bytes.
 */
extern n00b_result_t(n00b_store_postings_t *)
n00b_store_index_lookup_mapped(n00b_store_index_t     *index,
                               n00b_store_map_shard_t *shard,
                               n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    // Opaque here; internal/rocs/index.h defines it. Callers that
    // already resolved the value hand it over so the walk does not
    // normalize and hash it again on every shard it visits.
    struct n00b_store_index_keys_t *keys = nullptr;
};

/**
 * @brief Construct a valid empty postings view.
 *
 * Empty views are successful, non-null values. They represent "no candidate
 * postings", not an error and not a missing index.
 *
 * @kw shard_id   Shard identity represented by this posting view.
 * @kw generation Store generation for durable positions.
 * @kw allocator  Allocator for the postings view.
 *
 * @return Ok(postings) containing a non-null empty postings view.
 */
extern n00b_result_t(n00b_store_postings_t *)
n00b_store_postings_empty() _kargs
{
    uint64_t          shard_id   = 0;
    uint64_t          generation = 0;
    n00b_allocator_t *allocator  = nullptr;
};

/**
 * @brief Return the number of postings in a view.
 *
 * @param postings Posting view returned by an index lookup or
 *                 @c n00b_store_postings_empty.
 * @return Ok(length) on success, or @c N00B_STORE_INDEX_ERR_ARG for a null or
 *         malformed view.
 */
extern n00b_result_t(uint64_t)
n00b_store_postings_len(n00b_store_postings_t *postings);

/**
 * @brief Borrow a posting position by ordinal within the posting view.
 *
 * Out-of-range ordinals return successful none. This accessor does not
 * materialize a record-view handle.
 *
 * @param postings Posting view returned by an index lookup or
 *                 @c n00b_store_postings_empty.
 * @param ordinal  Zero-based ordinal within the posting view.
 * @return Result wrapping some durable position for an in-range entry, none
 *         for out-of-range, or a typed index error for invalid view state.
 */
extern n00b_result_t(n00b_option_t(n00b_store_pos_t))
n00b_store_postings_pos(n00b_store_postings_t *postings, uint64_t ordinal);

/**
 * @brief Borrow a posting by ordinal within the posting view.
 *
 * Out-of-range ordinals return successful none.
 *
 * @param postings Posting view returned by an index lookup or
 *                 @c n00b_store_postings_empty.
 * @param ordinal  Zero-based ordinal within the posting view.
 * @return Result wrapping some posting for an in-range entry, none for
 *         out-of-range, or @c N00B_STORE_INDEX_ERR_ARG /
 *         @c N00B_STORE_INDEX_ERR_STATE for invalid view state.
 */
extern n00b_result_t(n00b_option_t(n00b_store_posting_t))
n00b_store_postings_get(n00b_store_postings_t *postings, uint64_t ordinal);

/**
 * @brief Read the durable position carried by a record-view handle.
 *
 * @param record Opaque shard-aware record-view handle.
 * @return Ok(position) on success, or @c N00B_STORE_INDEX_ERR_ARG for a null
 *         handle.
 */
extern n00b_result_t(n00b_store_pos_t)
n00b_store_record_pos(n00b_store_record_t *record);

/**
 * @brief Serialize a record view as a compact JSON string.
 *
 * @param record Borrowed opaque record view.
 * @kw allocator Allocator for the returned string.
 * @return Ok(string) on success, or a typed index error.
 *
 * For sealed mapped records and owned-text hot copies this returns the stored
 * compact JSON bytes verbatim. Other hot and owned records are encoded from
 * their materialized graph.
 */
extern n00b_result_t(n00b_string_t *)
n00b_store_record_view_json_string(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

#ifdef __cplusplus
}
#endif
