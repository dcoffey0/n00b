/**
 * @file internal/rocs/index.h
 * @brief Internal index and record-view helpers for rocs planner integration.
 *
 * These declarations are internal to rocs. They construct process-side index
 * descriptors for schema-derived planning and resolve opaque shard-aware
 * @c n00b_store_record_t handles for existing per-shard ordinals. They do not
 * expose a public hit/record API and never return raw mapped JSON pointers to
 * callers.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"
#include "adt/result.h"
#include "core/alloc.h"
#include "core/codegen_abi.h" // n00b_gc_struct_array_t
#include "parsers/json.h"
#include "rocs/index.h"
#include "rocs/map.h"
#include "rocs/shard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef n00b_list_t(n00b_string_t *) n00b_store_index_field_list_t;

/**
 * @brief Internal document-frequency/selectivity facts for one index lookup.
 *
 * These facts are planner/ranking inputs only. They are derived from hot or
 * mapped posting tables, do not compute ranked scores, and are not exposed
 * through public query/cache APIs.
 */
typedef struct {
    uint64_t record_count;
    uint64_t document_frequency;
    double   selectivity;
} n00b_store_index_stats_t;

/**
 * @brief Construct the internal hot catch-all full-text descriptor.
 *
 * @param fields Borrowed real schema field names opted into catch-all search.
 * @kw allocator Allocator for the returned descriptor.
 * @return Ok(index) on success, or a typed index error.
 *
 * The returned descriptor is process-side metadata for
 * @ref n00b_filter_any identity handling. It unions whole-token full-text
 * postings from the real schema fields in @p fields. It is not a public schema
 * field, does not advertise through @ref n00b_store_index_advertise, and never
 * exposes a fake field string such as "all".
 */
extern n00b_result_t(n00b_store_index_t *)
n00b_store_index_new_catch_all(n00b_store_index_field_list_t *fields) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Report whether an index descriptor is the internal catch-all.
 *
 * @param index Borrowed descriptor.
 * @return Ok(true) for internal catch-all descriptors, Ok(false) otherwise.
 */
extern n00b_result_t(bool)
n00b_store_index_is_catch_all(n00b_store_index_t *index);

/**
 * @brief Read the schema fields a catch-all descriptor unions.
 *
 * @param index Borrowed catch-all descriptor.
 * @return Ok(borrowed field list), or @c N00B_STORE_INDEX_ERR_ARG when @p index
 *         is null or is not the internal catch-all.
 *
 * The opt-in list is the only description of catch-all coverage; raw record
 * evaluation cannot reproduce it. That makes this the one way a checker can
 * build a reference answer for a catch-all predicate, so it is available in
 * every build: a reference implementation that cannot be compiled against
 * optimized code checks the wrong thing.
 */
extern n00b_result_t(n00b_store_index_field_list_t *)
n00b_store_index_catch_all_fields(n00b_store_index_t *index);

/**
 * @brief Derive internal posting frequency facts from an open hot shard.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed open hot shard.
 * @param value Query JSON value normalized by the same path as lookup.
 * @return Ok(stats) on success, or a typed index error.
 *
 * Read from posting headers through @ref n00b_store_index_df_hot, so it costs
 * what that does and no lookup. The frequency is therefore that function's
 * bound, capped at the record count: exact for one term, an upper bound for
 * several or for the catch-all.
 */
extern n00b_result_t(n00b_store_index_stats_t)
n00b_store_index_stats_hot(n00b_store_index_t *index,
                           n00b_store_shard_t *shard,
                           n00b_json_node_t   *value);

/**
 * @brief Derive internal posting frequency facts from a sealed mapped shard.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed sealed mapped shard view.
 * @param value Query JSON value normalized by the same path as lookup.
 * @return Ok(stats) on success, or a typed index error.
 *
 * Same contract as @ref n00b_store_index_stats_hot, through
 * @ref n00b_store_index_df_mapped.
 */
extern n00b_result_t(n00b_store_index_stats_t)
n00b_store_index_stats_mapped(n00b_store_index_t     *index,
                              n00b_store_map_shard_t *shard,
                              n00b_json_node_t       *value);

typedef struct n00b_store_index_keys_t  n00b_store_index_keys_t;
typedef struct n00b_store_index_probe_t n00b_store_index_probe_t;

/**
 * @brief Normalize a value into terms and hash each into a column key.
 *
 * @param index Borrowed descriptor. Catch-all descriptors are rejected.
 * @param value Query JSON value.
 * @kw allocator Allocator for the returned key set.
 * @return Ok(keys) on success, or a typed index error.
 *
 * Holds nothing shard-specific, so one key set serves every shard a query
 * visits. Process-side query state, never shard marshal state.
 */
extern n00b_result_t(n00b_store_index_keys_t *)
n00b_store_index_keys_new(n00b_store_index_t *index,
                          n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Whether two resolved lookups name the same postings.
 *
 * @param a First key set, or null.
 * @param b Second key set, or null.
 * @return True when both are resolved and hash to the same keys in the same
 *         order. Null on either side answers false: unresolved is not a claim
 *         about equality.
 */
extern bool
n00b_store_index_keys_equal(n00b_store_index_keys_t *a,
                            n00b_store_index_keys_t *b);

/**
 * @brief How many terms a resolved lookup intersects.
 *
 * @param keys Resolved key set, or null.
 * @return Term count, or 0 for null or for a value matching nothing.
 */
extern uint64_t
n00b_store_index_keys_count(n00b_store_index_keys_t *keys);

/**
 * @brief Order-sensitive digest of a resolved key set.
 *
 * @param keys Resolved key set, or null.
 * @return A digest that differs when @ref n00b_store_index_keys_equal would
 *         answer false, and matches when it would answer true. Distinct key
 *         sets may still collide, so a match means "compare them properly",
 *         not "they are equal".
 */
extern uint64_t
n00b_store_index_keys_digest(n00b_store_index_keys_t *keys);

/**
 * @brief Read one resolved column key.
 *
 * @param keys Resolved key set. Must be non-null.
 * @param index Position below @ref n00b_store_index_keys_count.
 * @return The hashed key at that position.
 */
extern n00b_uint128_t
n00b_store_index_keys_at(n00b_store_index_keys_t *keys, uint64_t index);

/**
 * @brief Resolve a lookup into something that answers membership.
 *
 * @param index Borrowed descriptor.
 * @param shard Borrowed open hot shard.
 * @param value Query JSON value normalized by the same path as lookup.
 * @kw allocator Allocator for the probe and its borrowed posting handles.
 * @return Ok(probe) on success, or a typed index error.
 *
 * Answers per ordinal, where @ref n00b_store_index_lookup enumerates. A probe
 * for a term the shard never indexed answers false for every ordinal rather
 * than failing. A catch-all probe holds the term's list in each covered field
 * and answers whether any of them carries the ordinal.
 */
extern n00b_result_t(n00b_store_index_probe_t *)
n00b_store_index_probe_hot(n00b_store_index_t *index,
                           n00b_store_shard_t *shard,
                           n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
};

/**
 * @brief Resolve a sealed lookup into something that answers membership.
 *
 * Same contract as @ref n00b_store_index_probe_hot, over a mapped shard.
 */
extern n00b_result_t(n00b_store_index_probe_t *)
n00b_store_index_probe_mapped(n00b_store_index_t     *index,
                              n00b_store_map_shard_t *shard,
                              n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
};

/**
 * @brief Ask whether one ordinal is among a probe's matches.
 *
 * @param probe Probe from @ref n00b_store_index_probe_hot or its mapped twin.
 * @param ordinal Per-shard ordinal.
 * @return Ok(true) when every term of the lookup carries @p ordinal, or for a
 *         catch-all, when any covered field does.
 */
extern n00b_result_t(bool)
n00b_store_index_probe_contains(n00b_store_index_probe_t *probe,
                                uint64_t                  ordinal);

/**
 * @brief The most posting lists one membership test reads.
 *
 * @param probe Probe from @ref n00b_store_index_probe_hot or its mapped twin.
 * @return One per term for a lookup's intersection, one per covered field
 *         holding the term for a catch-all, and zero for a probe that
 *         matches nothing.
 */
extern uint64_t
n00b_store_index_probe_width(n00b_store_index_probe_t *probe);

/**
 * @brief Whether a probe answers membership by search rather than by scan.
 *
 * A dense list answers from one bitmap bit; a sparse list answers by binary
 * search when it advertises ascending order, and by a linear scan when it does
 * not. The cost model prices a membership test as a search, so a caller
 * choosing between probing and enumerating has to know which it would get.
 *
 * @param probe Probe from @ref n00b_store_index_probe_hot or its mapped twin.
 * @return True when every term of the lookup answers in logarithmic time.
 */
extern bool
n00b_store_index_probe_searchable(n00b_store_index_probe_t *probe);

/**
 * @brief Bound a lookup's matches without performing the lookup.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed open hot shard.
 * @param value Query JSON value normalized by the same path as lookup.
 * @kw allocator Allocator for the normalized query terms.
 * @return Ok(bound) on success, or a typed index error.
 *
 * Reads no postings and materializes no records: one dict probe and one field
 * read per normalized term. Pass @c keys to skip re-normalizing the value.
 *
 * Exact for a single-term lookup, and for a term the shard never indexed
 * (zero). A multi-term lookup intersects its terms, so the answer is the
 * smallest term's count and the true match count may be lower. A catch-all
 * unions its covered fields, so its answer is the sum of their counts, which
 * exceeds the match count by every record matching in more than one field.
 */
extern n00b_result_t(uint64_t)
n00b_store_index_df_hot(n00b_store_index_t *index,
                        n00b_store_shard_t *shard,
                        n00b_json_node_t   *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
};

/**
 * @brief Bound a sealed lookup's matches without performing the lookup.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed sealed mapped shard view.
 * @param value Query JSON value normalized by the same path as lookup.
 * @kw allocator Allocator for the normalized query terms.
 * @return Ok(bound) on success, or a typed index error.
 *
 * Same contract as @ref n00b_store_index_df_hot, reading the posting count out
 * of the mapped posting header.
 */
extern n00b_result_t(uint64_t)
n00b_store_index_df_mapped(n00b_store_index_t     *index,
                           n00b_store_map_shard_t *shard,
                           n00b_json_node_t       *value) _kargs
{
    n00b_allocator_t        *allocator = nullptr;
    n00b_store_index_keys_t *keys      = nullptr;
};

/**
 * @brief Materialize an index's physical column on an open shard.
 *
 * Ingest only creates a column when some record populates the field
 * (n00b_store_index_add returns early otherwise), so a declared-but-unpopulated
 * field leaves no column behind. That makes two states indistinguishable to a
 * sealed reader, and they need opposite answers:
 *
 *   A. the field was declared and no record here populated it -> an equality
 *      has zero matches and must resolve to an EMPTY exact set;
 *   B. the shard was sealed BEFORE the declaration existed -> records may
 *      populate it, no index was ever built, and the reader must scan.
 *
 * Calling this for every declared-indexed field before seal writes the empty
 * column that separates them: present-and-empty is A, absent is B (see
 * n00b_store_index_present_mapped, which is the reader half). Answering A by
 * "declared but no column" alone instead is what n00b#223 removed, because it
 * silently gives B the wrong answer (n00b#202).
 *
 * Idempotent, and never disturbs a column that already has postings.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed OPEN hot shard.
 * @return Ok(true) when a column was created, Ok(false) when one already
 *         existed or @p index is the virtual catch-all, or a typed index error
 *         (@c N00B_STORE_INDEX_ERR_STATE when the shard is not open).
 */
extern n00b_result_t(bool)
n00b_store_index_declare(n00b_store_index_t *index,
                         n00b_store_shard_t *shard);

/**
 * @brief Report whether a sealed shard carries an index's physical column.
 *
 * An absent column is ambiguous for sealed shards because the index may have
 * been declared after the shard was written. Internal catch-all descriptors
 * are virtual and always report present.
 *
 * @param index Borrowed process-side index descriptor.
 * @param shard Borrowed sealed mapped shard view.
 * @return Ok(true) when the column is present, Ok(false) when absent, or a
 *         typed index error.
 */
extern n00b_result_t(bool)
n00b_store_index_present_mapped(n00b_store_index_t     *index,
                                n00b_store_map_shard_t *shard);

extern n00b_result_t(bool)
n00b_store_index_present_hot(n00b_store_index_t *index,
                            n00b_store_shard_t *shard);

/**
 * @brief Construct an opaque record view for one open hot-shard ordinal.
 *
 * @param shard Borrowed open hot shard.
 * @param ordinal Per-shard ordinal to resolve.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error for invalid inputs,
 *         unreadable shard state, or out-of-range ordinal.
 *
 * The returned handle borrows @p shard and carries only shard-aware position
 * metadata. It does not copy or own the hot JSON record.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_hot_at(n00b_store_shard_t *shard,
                              uint64_t            ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Construct an opaque hot-shard record view for an explicit durable
 *        position.
 *
 * @param shard Borrowed in-memory hot-path shard that owns the record list.
 *              The shard may be the current OPEN hot shard or a just-sealed
 *              in-memory shard already handed to a live cursor hit.
 * @param pos   Durable position copied from the store tail. @c pos.shard_id
 *              must match @p shard and @c pos.ordinal must be readable.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error.
 *
 * Live query cursors use this helper so hot-tail hits report the durable store
 * generation captured by the authoritative scan rather than the legacy hot
 * posting generation. The returned handle borrows @p shard and remains an
 * in-memory hot-path view; it does not expose mapped storage, shard images, or
 * copy JSON. Callers must keep the containing cursor/hit lifetime inside the
 * borrowed shard lifetime.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_hot_pos(n00b_store_shard_t *shard,
                               n00b_store_pos_t    pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Construct an opaque record view for one sealed mapped-shard ordinal.
 *
 * @param shard Borrowed sealed mapped shard view.
 * @param ordinal Per-shard ordinal to resolve.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error for invalid inputs,
 *         unreadable mapped state, or out-of-range ordinal.
 *
 * The returned handle borrows @p shard. It records no raw mapped JSON pointer;
 * mapped record bytes remain hidden behind the rocs mapped access layer.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_mapped_at(n00b_store_map_shard_t *shard,
                                 uint64_t                ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Construct an opaque mapped record view with an explicit durable
 *        catalog position.
 *
 * @param shard Borrowed sealed mapped shard view.
 * @param pos Durable catalog position to attach to the returned record view.
 *            @c pos.shard_id must match @p shard and @c pos.ordinal must be
 *            readable in the mapped record list.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error for invalid inputs,
 *         unreadable mapped state, shard mismatch, or out-of-range ordinal.
 *
 * Query snapshot hits use this helper so public record positions retain the
 * catalog generation captured by the snapshot boundary rather than deriving a
 * generation from the mapped shard's seal timestamp.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_mapped_pos(n00b_store_map_shard_t *shard,
                                  n00b_store_pos_t        pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Construct an opaque record view backed by owned hot JSON.
 *
 * @param pos  Durable position copied into the returned record view.
 * @param json Owned materialized JSON graph for this record.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error.
 *
 * The returned handle does not borrow a hot shard or mapped shard. Query
 * output uses this for hot-tail deliveries that must remain valid after view
 * close or hot-shard rotation.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_owned_json(n00b_store_pos_t   pos,
                                  n00b_json_node_t  *json) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Build an owned record view over stored compact JSON text.
 *
 * As n00b_store_record_view_owned_json, but the view carries the record's
 * serialized bytes rather than a node graph. n00b_store_record_view_json_string
 * returns them verbatim; n00b_store_record_view_json parses them on demand, so
 * both consumers keep the behaviour they had with an owned graph.
 *
 * @param pos  Stable position the view reports.
 * @param text Owned copy of the record's compact JSON.
 * @kw allocator Allocator for the returned view handle.
 * @return Ok(record) on success, or a typed index error.
 */
extern n00b_result_t(n00b_store_record_t *)
n00b_store_record_view_owned_text(n00b_store_pos_t  pos,
                                  n00b_string_t    *text) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Copy a hot shard record's stored compact JSON bytes.
 *
 * @param shard   Open hot shard.
 * @param ordinal Record ordinal within the shard.
 * @kw allocator Allocator for the returned string.
 * @return Ok(text) on success, or a typed index error.
 */
extern n00b_result_t(n00b_string_t *)
rocs_hot_shard_record_text(n00b_store_shard_t *shard,
                           uint64_t            ordinal) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Recursively copy a hot JSON graph into the supplied allocator.
 *
 * @param node Borrowed JSON root.
 * @kw allocator Allocator for the returned JSON graph.
 * @return Ok(copied JSON) on success, or a typed index error.
 *
 * Internal store code uses this when moving parsed or caller-owned records into
 * the current hot-shard allocator at append time. The copy owns every object
 * key, string payload, array, object dictionary, and recursive node.
 */
extern n00b_result_t(n00b_json_node_t *)
rocs_json_node_copy(n00b_json_node_t *node) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Resolve a record view to a hot JSON node for verification.
 *
 * @param record Borrowed opaque record view.
 * @kw allocator Allocator used when a sealed mapped record must be
 *               materialized as a hot JSON graph.
 * @return Ok(node) on success, or a typed index error for invalid state.
 *
 * Hot record views backed by OPEN or SEALED in-memory hot-path shards return
 * the borrowed in-shard JSON node. Mapped record views return a newly
 * materialized hot JSON graph produced through internal rocs map helpers. The
 * function never returns a pointer into sealed mapped bytes.
 */
extern n00b_result_t(n00b_json_node_t *)
n00b_store_record_view_json(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Materialize a record view as a newly owned hot JSON graph.
 *
 * @param record Borrowed opaque record view.
 * @kw allocator Allocator for the returned JSON graph.
 * @return Ok(copied JSON) on success, or a typed index error.
 *
 * Hot, mapped, and already-owned record views all return a fresh recursive
 * JSON graph. The function never returns a raw pointer into mapped bytes and
 * never exposes shard/list/dict internals.
 */
extern n00b_result_t(n00b_json_node_t *)
n00b_store_record_view_json_copy(n00b_store_record_t *record) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Read several fields of one record without parsing the rest of it.
 *
 * @param record Borrowed opaque record view.
 * @param count  Number of fields in @p fields.
 * @param fields Field names, each resolved as @ref rocs_json_object_get_field
 *               resolves it: a key spelling the whole name, or else a dotted
 *               path through nested objects. Names must be distinct.
 * @param values Out: one entry per field, null when the record lacks it.
 * @kw allocator Allocator for the returned values.
 * @return Ok(true), or a typed index error.
 *
 * Scans the record's stored bytes (the hot shard's string or the sealed
 * image's, without copying them) once for all of @p fields and parses only
 * the values it finds. A record the scan declines, or one past
 * @c ROCS_JSON_SCAN_FIELDS_MAX fields, is parsed whole once instead, so
 * asking for many fields never costs more than one parse.
 */
extern n00b_result_t(bool)
n00b_store_record_view_fields(n00b_store_record_t *record,
                              size_t               count,
                              n00b_string_t      **fields,
                              n00b_json_node_t   **values) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Read one field of a record; @ref n00b_store_record_view_fields for a
 *        single name.
 *
 * @return Ok(some value), Ok(none) when the record lacks the field, or a
 *         typed index error.
 */
extern n00b_result_t(n00b_option_t(n00b_json_node_t *))
n00b_store_record_view_field(n00b_store_record_t *record,
                             n00b_string_t       *field) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Borrow a lookup's matching ordinals.
 *
 * @param postings Posting view returned by an index lookup.
 * @param len_out  Out: the number of ordinals.
 * @return The ordinals, ascending and unique, owned by @p postings; null when
 *         there are none.
 *
 * What a planner fills an ordinal set from, without a position or record view
 * per entry.
 */
extern const uint64_t *
n00b_store_postings_ordinals(n00b_store_postings_t *postings,
                             uint64_t              *len_out);

#ifdef N00B_DEBUG
// Record views built, whole records parsed, and records scanned for fields by
// the record-view layer since the last reset.
extern uint64_t
n00b_store_record_views_built(void);

extern uint64_t
n00b_store_record_parses(void);

extern uint64_t
n00b_store_record_field_scans(void);

extern void
n00b_store_record_counters_reset(void);
#endif

#ifdef __cplusplus
}
#endif

/**
 * @brief Whether a sealed lookup's posting lists advertise ascending order.
 *
 * For tests: a seal that stopped setting the bit still answers correctly, so
 * nothing else would notice the binary search going away.
 */
extern bool
n00b_store_index_sealed_is_ordered(n00b_store_index_t     *index,
                                   n00b_store_map_shard_t *shard,
                                   n00b_json_node_t       *value);

/**
 * @brief Clear the order bit on a sealed lookup's posting lists.
 *
 * Debug builds only: writes into the mapped image, and refuses any backing but
 * a writable copy. Produces the one shape a seal cannot, an ordered image that
 * does not say so, which is the only way to reach the linear-scan fallback.
 *
 * @return The number of posting lists whose bit was cleared.
 */
#ifdef N00B_DEBUG
extern uint64_t
n00b_store_index_sealed_clear_ordered(n00b_store_index_t     *index,
                                      n00b_store_map_shard_t *shard,
                                      n00b_json_node_t       *value);

/**
 * @brief Add @p delta to the maintained count of a hot lookup's posting lists.
 *
 * Debug builds only. Leaves the postings alone, so a test can show which
 * readers depend on the count agreeing with them.
 *
 * @return The number of posting lists changed.
 */
extern uint64_t
n00b_store_index_hot_skew_count(n00b_store_index_t *index,
                                n00b_store_shard_t *shard,
                                n00b_json_node_t   *value,
                                int64_t             delta);
#endif

/**
 * @brief Build an empty posting list in @p allocator.
 *
 * Neither representation carries a lock: a list has one writer, and readers
 * run lock-free against it (see rocs_posting_list_push).
 */
extern n00b_store_posting_list_t *
rocs_posting_list_new() _kargs
{
    n00b_store_postings_kind_t kind      = N00B_STORE_POSTINGS_SPARSE;
    n00b_allocator_t          *allocator = nullptr;
};

#ifdef N00B_DEBUG
/**
 * @brief Claim, and release, the right to push into a shard's posting lists.
 *
 * Debug-only. Aborts when a second thread claims a shard another thread holds,
 * which is the one arrangement the lock-free posting lists cannot survive.
 */
extern void rocs_index_writer_enter(n00b_store_shard_t *shard);
extern void rocs_index_writer_exit(n00b_store_shard_t *shard);

/**
 * @brief Park a hot posting-list reader between its length loads and its
 *        element reads, for a test that grows the list in that window.
 */
typedef void (*rocs_posting_read_hook_t)(n00b_store_posting_list_t *postings,
                                         void                      *ctx);
extern void rocs_posting_read_hook_set(rocs_posting_read_hook_t hook,
                                       void                    *ctx);
#else
#define rocs_index_writer_enter(shard) ((void)(shard))
#define rocs_index_writer_exit(shard)  ((void)(shard))
#endif

/**
 * @brief Add an ordinal to a posting list.
 *
 * The list's only writer: the caller holds the shard's writer claim, which
 * store ingest gets from commit_lock and n00b_store_index_add callers get by
 * not overlapping on a shard. Readers take no lock. The element or bit is
 * written first and the length is release-stored after it, and growth
 * publishes the copied array before the length and never frees the old one,
 * so a reader that acquires a length finds that many elements behind it.
 *
 * @param postings Borrowed posting list.
 * @param ordinal  Record ordinal to add.
 * @param unique   Whether an ordinal already present is rejected.
 * @return Ok(true) when the ordinal was added, Ok(false) when @p unique
 *         suppressed a duplicate, or a typed index error.
 */
extern n00b_result_t(bool)
rocs_posting_list_push(n00b_store_posting_list_t *postings,
                       uint64_t                   ordinal,
                       bool                       unique);

/**
 * @brief The GC/marshal scan shape of a posting list: only its two pointer
 *        words (`ordinals`, `flags`). n00b-lang/n00b#375.
 */
extern const n00b_gc_struct_array_t *
rocs_posting_list_scan_shape(void);

/**
 * @brief Stamp that shape onto a freshly allocated posting list's header, so
 *        a conservative scan can never take its packed `kind | reserved` word
 *        for a pointer. Both constructors call it; idempotent.
 */
extern void
rocs_posting_list_apply_scan(n00b_store_posting_list_t *postings);
