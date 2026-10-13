/**
 * @file internal/rocs/store.h
 * @brief Internal store/catalog helpers for rocs implementation.
 *
 * These declarations expose the narrow store internals needed by rocs
 * implementation modules. They are not part of the public rocs API and must
 * not be included from <rocs/n00b_rocs.h>.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"
#include "adt/list.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/alloc.h"
#include "parsers/json.h"
#include "internal/rocs/plan.h"
#include "rocs/store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef n00b_list_t(uint64_t)         n00b_store_shard_id_list_t;
typedef n00b_list_t(n00b_store_pos_t) n00b_store_pos_list_t;
typedef n00b_list_t(n00b_store_catalog_entry_t *)
    n00b_store_catalog_entry_list_t;

/**
 * @brief Owned catalog-visible sealed-shard metadata copied for planning.
 *
 * Carries only what query boundaries use. The partition key is copied into
 * the allocator supplied to @ref n00b_store_catalog_visible_snapshot or
 * @ref n00b_store_tail_snapshot and is not borrowed from the store catalog.
 * The value never contains store catalog-entry pointers, resident shard
 * handles, mapped shard handles, raw mapped containers, or record views.
 */
typedef struct {
    uint64_t       shard_id;
    uint64_t       generation;
    uint64_t       schema_generation;
    uint64_t       record_count;
    uint64_t       seal_ts;
    n00b_string_t *partition_key;
} n00b_store_catalog_snapshot_entry_t;

/** @brief Internal list of owned catalog snapshot entry values. */
typedef n00b_list_t(n00b_store_catalog_snapshot_entry_t)
    n00b_store_catalog_snapshot_t;

/**
 * @brief Narrow a broad process pin to a copied sealed-shard id set.
 *
 * @param pin Pin returned by @ref n00b_store_pin_acquire.
 * @param shard_ids Sealed shard identifiers owned by the caller. Order does
 *        not matter; duplicates and zero entries are ignored. The pin keeps
 *        its own copy, so the caller's list may be discarded or reused.
 * @return Ok(true) when the pin now protects exactly the snapshot shard ids,
 *         or a typed store error.
 *
 * Cost is O(n log n) in the number of ids (n00b#400).
 *
 * Query views use a broad pin while validating resume/as-of and copying their
 * boundary. Once the boundary is copied, the broad pin is narrowed so retention
 * and manual shard drop block only the shards actually in that snapshot. The
 * pin still contributes one active resource pin for close/lifecycle accounting.
 */
extern n00b_result_t(bool)
n00b_store_pin_narrow_to_shards(n00b_store_pin_t          *pin,
                                n00b_store_shard_id_list_t *shard_ids);

/**
 * @brief Best-effort Linux cgroup memory limit probe for service defaults.
 *
 * Returns none when cgroup files are unavailable, report "max", or contain an
 * unusable value. Non-Linux hosts therefore fall back to bounded constants.
 */
extern n00b_option_t(uint64_t)
rocs_store_cgroup_memory_limit(void);

/**
 * @brief Copied hot-tail scan result for live-query catch-up.
 *
 * `matches` contains durable positions copied out of the current hot shard.
 * `last_observed` advances over every committed hot record scanned, including
 * non-matches. No record views, shard handles, mapped containers, commit
 * payloads, or catalog-entry pointers are exposed.
 */
typedef struct {
    n00b_store_pos_list_t *matches;
    bool                   has_last_observed;
    n00b_store_pos_t       last_observed;
    uint64_t               scanned_records;
} n00b_store_hot_tail_scan_t;

/**
 * @brief Coherent copied store tail boundary for live-query catch-up.
 *
 * `sealed` is the catalog-visible sealed-shard snapshot copied while the store
 * commit/catalog lock was held. `hot_through` is set only when the same locked
 * snapshot observed at least one record in the current hot shard. It is an
 * inclusive upper bound for a later capped hot scan; it is not a shard handle
 * and exposes no record views, mapped containers, or catalog-entry pointers.
 */
typedef struct {
    n00b_store_catalog_snapshot_t *sealed;
    bool                           has_hot_through;
    n00b_store_pos_t               hot_through;
} n00b_store_tail_snapshot_t;

/**
 * @brief Copy the current catalog-visible sealed-shard metadata.
 *
 * @param store Open store whose catalog is being planned.
 * @kw allocator Allocator for the returned list and copied strings.
 * @return Ok(snapshot) for an open store, or a typed store error.
 *
 * @pre @p store is non-null and open.
 * @post Entries come from one catalog version, in ascending
 *       (generation, shard_id) order.
 * @post The returned list and entry values are owned by the supplied allocator
 *       and are independent of later catalog seal/drop/retention mutation.
 *       Returned entries retain no store catalog-entry pointers, resident
 *       handles, mapped shard handles, raw mapped containers, or record views.
 */
extern n00b_result_t(n00b_store_catalog_snapshot_t *)
n00b_store_catalog_visible_snapshot(n00b_store_t *store) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Copy the sealed catalog window and hot upper bound.
 *
 * @param store Open store whose tail is being planned.
 * @kw allocator Allocator for the returned sealed snapshot and copied strings.
 * @kw resume Optional lower bound. Only shards holding a position at or after
 *            it are copied, which includes the shard @p resume is in.
 * @kw as_of Optional upper bound. Only shards whose first position is at or
 *           before it are copied.
 * @kw min_seal_ts Optional seal-time floor. Shards sealed before it are not
 *                 copied; shards with no seal timestamp always are.
 * @return Ok(snapshot) for an open store, or a typed store error.
 *
 * @pre @p store is non-null and open.
 * @post Sealed entries come from one catalog version, in ascending
 *       (generation, shard_id) order. Finding the window costs O(log N) in
 *       the catalog, and only entries inside it are visited or copied.
 * @post The returned value contains only copied scalar metadata, owned
 *       strings, and an optional durable hot position. It retains no shard
 *       handles, catalog-entry pointers, mapped containers, or record views.
 */
extern n00b_result_t(n00b_store_tail_snapshot_t)
n00b_store_tail_snapshot(n00b_store_t *store) _kargs
{
    n00b_allocator_t *allocator   = nullptr;
    n00b_store_pos_t *resume      = nullptr;
    n00b_store_pos_t *as_of       = nullptr;
    uint64_t          min_seal_ts = 0;
};

/**
 * @brief Derive process-side schema index descriptors for query planning.
 *
 * @param store Open store whose frozen schema supplies field index policy.
 * @kw allocator Allocator for the returned descriptor list and descriptors.
 * @return Ok(indexes) for a usable store schema, or a typed store error.
 *
 * The returned list contains process-side TERM, FULLTEXT, NGRAM, and internal
 * catch-all descriptors derived from the schema. It is scratch query state:
 * descriptors are never marshaled into shards and callers must continue to use
 * mapped lookup/stat helpers for sealed shard data.
 */
extern n00b_result_t(n00b_plan_index_list_t *)
n00b_store_plan_indexes_for_query(n00b_store_t *store) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return the store's configured commit topic for live-query wakeups.
 *
 * @param store Open store being tailed.
 * @return Ok(some(topic)) when a process-side commit topic is configured,
 *         Ok(none) when live queries must poll authoritative store state
 *         without wakeups, or a typed store error.
 *
 * The returned topic remains owned by the store/conduit configuration. Live
 * query code may use it only to allocate a bounded inbox and subscribe for
 * wakeup hints; commit payloads are not authoritative result state.
 */
extern n00b_result_t(n00b_option_t(n00b_store_commit_topic_t *))
n00b_store_commit_topic_for_query(n00b_store_t *store);

/**
 * @brief Allocate a bounded commit inbox using a configured commit topic.
 *
 * @param topic Configured commit topic borrowed from the store.
 * @param limit Maximum queued wakeup messages. Must be nonzero.
 * @kw allocator Optional inbox allocator.
 * @return Ok(inbox), or a typed store error.
 *
 * The inbox uses drop-newest backpressure so slow live-query wakeup consumers
 * cannot block ingest, seal, retention, or catalog commit.
 */
extern n00b_result_t(n00b_store_commit_inbox_t *)
n00b_store_commit_inbox_for_query(n00b_store_commit_topic_t *topic,
                                  uint32_t                   limit) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Cancel a live-query commit subscription under the topic lock.
 *
 * @param topic Borrowed commit topic used to subscribe.
 * @param sub Subscription handle returned by @ref n00b_store_commit_subscribe.
 * @return Ok(true) when a valid handle was submitted for cancellation,
 *         Ok(false) for an already-invalid handle, or a typed store error.
 */
extern n00b_result_t(bool)
n00b_store_commit_unsubscribe_for_query(n00b_store_commit_topic_t  *topic,
                                        n00b_conduit_sub_handle_t   sub);

/**
 * @brief Scan the current hot shard after a copied durable position.
 *
 * @param store Open store being tailed.
 * @param predicate Borrowed lowered query predicate.
 * @param after Optional last-observed durable position. Null scans from the
 *              beginning of the current hot shard.
 * @kw allocator Allocator for the returned copied position list.
 * @kw through Optional inclusive hot upper bound captured by
 *             @ref n00b_store_tail_snapshot. When present, the current hot
 *             shard must still match this position's generation and shard id;
 *             matching positions and durable progress are capped at this
 *             ordinal. When absent, the current hot shard is scanned through
 *             its current committed end.
 * @kw cancel_cb Optional cooperative-cancellation predicate, handed to the hot
 *               collect and execution and polled every 1024 matches while
 *               they are copied; returning true fails the scan with
 *               @c N00B_STORE_ERR_CANCELED and releases the hot pin.
 *               Borrowed; may be nullptr.
 * @kw cancel_ctx Opaque context passed to @p cancel_cb. Borrowed.
 * @kw result_limit Stop after this many matches in ascending order. The
 *                  returned progress then ends at the last match returned,
 *                  so a later scan after it resumes where this one stopped.
 *                  Zero returns no matches and observes nothing.
 * @kw reverse Take the last @p result_limit matches at or below `through`
 *             instead of the first. Matches still come back ascending, and
 *             the progress reported ends at `through`.
 * @return Ok(scan) with copied matching positions and durable progress, or a
 *         typed store error.
 *
 * The store commit lock is held in read mode while the hot shard is inspected.
 * Hot planning derives current schema descriptors for TERM, FULLTEXT, and
 * NGRAM fields and can consume the corresponding hot posting columns created
 * by ingest. Commit messages are not consulted; this reads authoritative
 * in-process store state and is safe when wakeup messages are dropped.
 *
 * A live-query scan that has already captured a tail snapshot must pass that
 * snapshot's hot upper bound as @p through and must skip this helper when the
 * snapshot observed no hot records. That contract prevents one scan pass from
 * advancing past a shard that sealed after the sealed-boundary snapshot.
 */
extern n00b_result_t(n00b_store_hot_tail_scan_t)
n00b_store_hot_tail_scan_after(n00b_store_t          *store,
                               n00b_plan_predicate_t *predicate,
                               n00b_store_pos_t      *after) _kargs
{
    n00b_allocator_t    *allocator    = nullptr;
    n00b_store_pos_t    *through      = nullptr;
    n00b_plan_cancel_fn  cancel_cb    = nullptr;
    void                *cancel_ctx   = nullptr;
    uint64_t             result_limit = UINT64_MAX;
    bool                 reverse      = false;
};

/**
 * @brief Pin the hot shard a view froze at @p through, for index lookups.
 *
 * @param store Open store.
 * @param through Inclusive hot upper bound captured by
 *        @ref n00b_store_tail_snapshot.
 * @return Ok(some(shard)) while that shard is still the store's hot shard,
 *         pinned until @ref n00b_store_hot_shard_release; Ok(none), with
 *         nothing pinned, once a seal has rotated it away; or a typed store
 *         error.
 *
 * The pin keeps a seal from swapping the shard out and freeing it, and a seal
 * waits for it, so hold it only across in-memory index reads. Writers keep
 * appending while it is held, so postings may name ordinals past @p through,
 * which the caller must ignore.
 */
extern n00b_result_t(n00b_option_t(n00b_store_shard_t *))
n00b_store_hot_shard_acquire(n00b_store_t *store, n00b_store_pos_t through);

/** @brief Release a pin taken by @ref n00b_store_hot_shard_acquire. */
extern void
n00b_store_hot_shard_release(n00b_store_t *store);

/**
 * @brief Borrow a current hot-shard record view for a copied durable position.
 *
 * @param store Open store whose current hot shard may own @p pos.
 * @param pos Durable position copied by an authoritative tail scan.
 * @kw allocator Allocator for the returned record view when present.
 * @return Ok(some(record)) when @p pos is still in the current hot shard,
 *         Ok(none) when it is not current-hot state, or a typed store error.
 *
 * The helper holds the store commit lock while checking the current hot shard
 * and constructing the opaque record view. It returns no shard pointer, JSON
 * node, mapped storage, catalog entry, or resident handle. The returned record
 * view is borrowed from the current hot-shard allocator; callers that need a
 * hit to survive a later seal must materialize/copy the record before exposing
 * it outside the current operation.
 */
extern n00b_result_t(n00b_option_t(n00b_store_record_t *))
n00b_store_hot_record_view_for_pos(n00b_store_t     *store,
                                   n00b_store_pos_t  pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Copy a current hot-shard record view for a durable position.
 *
 * @param store Open store whose current hot shard may own @p pos.
 * @param pos Durable position copied by an authoritative tail scan.
 * @kw allocator Allocator for the returned owned record view and JSON graph.
 * @return Ok(some(record)) when @p pos is still in the current hot shard,
 *         Ok(none) when it is not current-hot state, or a typed store error.
 *
 * The helper holds the store commit lock while checking the current hot shard
 * and materializing the JSON graph. The returned record view is independent of
 * the hot-shard allocator and remains valid if that hot shard is sealed and
 * its allocator is destroyed after this call returns.
 */
extern n00b_result_t(n00b_option_t(n00b_store_record_t *))
n00b_store_hot_record_copy_for_pos(n00b_store_t     *store,
                                   n00b_store_pos_t  pos) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return the current catalog-visible sealed shard count.
 *
 * @param store Open store whose catalog is being planned.
 * @return Ok(count) for an open store, or a typed store error.
 *
 * This is the planner visibility boundary: dropped and stale shards are absent
 * from this count because retention removes their catalog entries.
 */
extern n00b_result_t(uint64_t)
n00b_store_catalog_visible_entry_count(n00b_store_t *store);

/**
 * @brief Borrow one catalog-visible sealed shard by (generation, shard_id)
 *        order.
 *
 * @param store Open store whose catalog is being planned.
 * @param index Zero-based catalog-visible entry index.
 * @return Ok(some(entry)) when present, Ok(none) for out-of-range, or a typed
 *         store error. O(1).
 *
 * Returned entries are borrowed from the store catalog. Callers must not retain
 * them across catalog mutation. Multi-call planning paths that need a stable
 * historical boundary must use @ref n00b_store_catalog_visible_snapshot
 * instead.
 */
extern n00b_result_t(n00b_option_t(n00b_store_catalog_entry_t *))
n00b_store_catalog_visible_entry_at(n00b_store_t *store, uint64_t index);

typedef n00b_list_t(n00b_store_catalog_entry_t *)
    n00b_store_catalog_entry_list_t;

/**
 * @brief Borrow every catalog-visible sealed shard, in catalog order, in one
 *        pass.
 *
 * Same borrowing rules as @ref n00b_store_catalog_visible_entry_at, which
 * costs a catalog scan per call.
 */
extern n00b_result_t(n00b_store_catalog_entry_list_t *)
n00b_store_catalog_visible_entries(n00b_store_t *store) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Copy the catalog-visible sealed entries of one catalog version.
 *
 * @param store Open store whose catalog is being planned.
 * @kw allocator Allocator for the returned list.
 * @return Ok(entries) in ascending (generation, shard_id) order, or a typed
 *         store error.
 *
 * The entries are borrowed from the store catalog, as with
 * @ref n00b_store_catalog_visible_entry_at, but the list is taken from a
 * single catalog version, so a seal or drop during the caller's walk neither
 * shifts nor hides an entry. Cost is one pointer copy per visible entry.
 */
extern n00b_result_t(n00b_store_catalog_entry_list_t *)
n00b_store_catalog_visible_entries(n00b_store_t *store) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

#ifdef N00B_DEBUG
/**
 * @brief Catalog entries examined by catalog reads since the last reset.
 *
 * Counts entries touched by shard-id lookups, visible enumeration, snapshot
 * windows, backlog, stream open, visible_entry_after, and the residency
 * eviction walk. Catalog mutations, which rebuild the catalog index, are not
 * counted.
 * Only in builds with @c N00B_DEBUG.
 */
extern uint64_t
n00b_store_catalog_entries_visited(void);

extern void
n00b_store_catalog_entries_visited_reset(void);

// Points where a store reader calls the read hook from inside its read,
// holding what protects that read, so a test can start a writer at exactly
// that point.
typedef enum {
    // Record stream open, between counting its sealed entries and filling
    // them, holding residency_lock and a view pin.
    N00B_STORE_CATALOG_READ_STREAM_OPEN,
    // Memory stats, holding a view pin, before it reads the view's totals.
    N00B_STORE_CATALOG_READ_MEMORY_STATS,
    // Retired hot allocator detach, holding residency_lock and a view pin,
    // before its walk.
    N00B_STORE_CATALOG_READ_RETIRED_HOT,
    // Shard-id lookup, holding a view pin, after each entry it compares.
    N00B_STORE_CATALOG_READ_FIND_SHARD,
    // visible_entry_after, holding a view pin, before its search.
    N00B_STORE_CATALOG_READ_ENTRY_AFTER,
    // Memory stats, holding the hot pin, after it loads the hot shard.
    N00B_STORE_CATALOG_READ_HOT_STATS,
    // Hot shard acquire, before it pins the hot shard, so a hook can seal
    // on the reader's own thread and make the acquire miss.
    N00B_STORE_CATALOG_READ_HOT_ACQUIRE,
} n00b_store_catalog_read_t;

typedef void (*n00b_store_catalog_read_hook_t)(n00b_store_t             *store,
                                               n00b_store_catalog_read_t site,
                                               void                     *ctx);

/**
 * @brief Install a process-wide catalog read hook, or clear it with nullptr.
 *        Only under @c N00B_DEBUG. Set it before the threads that use it start.
 */
extern void
n00b_store_catalog_read_hook_set(n00b_store_catalog_read_hook_t hook,
                                 void                          *ctx);

/**
 * @brief Whether @p thread is blocked inside @p store: parked on one of its
 *        mutexes, waiting for pinned catalog readers to leave a view it
 *        replaced, or waiting for hot-pin readers to leave the hot shard.
 *        Only under @c N00B_DEBUG.
 */
extern bool
n00b_store_thread_blocked(n00b_store_t *store, n00b_thread_t *thread);

// Fallible steps of a record's commit, where a test can inject a failure.
// RANGE_ steps are taken only by the batch range commit; the rest are taken by
// it and by the single-record commit.
typedef enum {
    // Per range: the array of commit jobs.
    N00B_STORE_FAULT_RANGE_JOB_ARRAY,
    // Per record: its commit job.
    N00B_STORE_FAULT_RANGE_JOB,
    // Per record, under raw retention: its raw span.
    N00B_STORE_FAULT_RANGE_RAW_SPAN,
    // Per range: starting the prepare workers.
    N00B_STORE_FAULT_RANGE_WORKERS,
    // Per record, on a worker: its prepare, which makes the slot a tombstone.
    N00B_STORE_FAULT_RANGE_PREPARE,
    // Per tombstone: building it.
    N00B_STORE_FAULT_RANGE_TOMBSTONE,
    // Per tombstone: preparing its slot.
    N00B_STORE_FAULT_RANGE_TOMBSTONE_SLOT,
    // Per record: its encoded text is missing.
    N00B_STORE_FAULT_RANGE_RECORD_TEXT,
    // Per record journaled from its encoded text: building the frame.
    N00B_STORE_FAULT_RANGE_JOURNAL_BUFFER,
    // Per range: releasing the slots after the one that failed.
    N00B_STORE_FAULT_RANGE_CANCEL,
    // Per record: appending it to the journal.
    N00B_STORE_FAULT_JOURNAL,
    // Per record: filling its slot.
    N00B_STORE_FAULT_FILL,
    // Per record: publishing its slot.
    N00B_STORE_FAULT_PUBLISH,
} n00b_store_fault_t;

// Asked at each step; true fails it. Record is the batch record the step
// works on, or nullptr for a per-range step.
typedef bool (*n00b_store_fault_hook_t)(n00b_store_fault_t fault,
                                        n00b_json_node_t  *record,
                                        void              *ctx);

/**
 * @brief Install a process-wide commit fault hook, or clear it with nullptr.
 *        Only under @c N00B_DEBUG. Set it before the ingest starts; the
 *        prepare step asks it on worker threads.
 */
extern void
n00b_store_fault_hook_set(n00b_store_fault_hook_t hook, void *ctx);

// Points on the catalog's write path where the catalog write hook runs.
typedef enum {
    // After records reach the catalog log durably.
    N00B_STORE_CATALOG_LOG_APPENDED,
    // Before a fold writes the catalog image.
    N00B_STORE_CATALOG_FOLD_BEGIN,
    // After a fold's image is durable, before the log two epochs back is
    // deleted.
    N00B_STORE_CATALOG_FOLD_WRITTEN,
} n00b_store_catalog_write_site_t;

typedef void (*n00b_store_catalog_write_hook_t)(
    n00b_store_t                   *store,
    n00b_store_catalog_write_site_t site,
    void                           *ctx);

/**
 * @brief Install a process-wide catalog write hook, or clear it with
 *        nullptr. Only under @c N00B_DEBUG.
 */
extern void
n00b_store_catalog_write_hook_set(n00b_store_catalog_write_hook_t hook,
                                  void                           *ctx);
#endif

// A fold waits for at least this many catalog log records, and for the log
// to reach half the image's size.
#define N00B_STORE_CATALOG_FOLD_MIN_RECORDS 32

/**
 * @brief Test-control guard for borrowed catalog enumeration helpers.
 *
 * @param store Open store whose borrowed enumeration helpers are guarded.
 * @param disabled When true, borrowed count/entry enumeration helpers return
 *                 @c N00B_STORE_ERR_STATE.
 * @return Ok(true), or a typed store error.
 *
 * This internal helper is for focused regression tests that prove query
 * boundary capture uses @ref n00b_store_catalog_visible_snapshot instead of
 * the unstable borrowed count/entry path. It changes no catalog metadata and
 * does not affect the stable snapshot-copy helper.
 */
extern n00b_result_t(bool)
n00b_store_catalog_test_set_borrowed_enumeration_disabled(
    n00b_store_t *store,
    bool          disabled);

/**
 * @brief Borrow the store partition policy used by planner pruning.
 *
 * @param store Open store.
 * @return Ok(policy), or a typed store error.
 */
extern n00b_result_t(n00b_store_partition_policy_t *)
n00b_store_partition_policy_for_plan(n00b_store_t *store);

/**
 * @brief Borrow the configured partition field when the policy has one.
 *
 * @param policy Partition policy returned by store open/configuration.
 * @return Ok(some(field)) for time/hash partition policies, Ok(none) for the
 *         default/no-partition policy, or a typed store error.
 */
extern n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_store_partition_policy_field_for_plan(
    n00b_store_partition_policy_t *policy);

/**
 * @brief Compute the route key for one partition-field value.
 *
 * @param policy Partition policy returned by store open/configuration.
 * @param value  Borrowed JSON value for the partition field.
 * @kw allocator Allocator for non-default route strings and hash scratch.
 * @return Ok(route key). Values that would route to the store default bucket
 *         return @c default.
 *
 * This helper uses the same policy semantics as store ingest routing while
 * avoiding construction of temporary records in the planner.
 */
extern n00b_result_t(n00b_string_t *)
n00b_store_partition_route_value_for_plan(
    n00b_store_partition_policy_t *policy,
    n00b_json_node_t              *value) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

#ifdef __cplusplus
}
#endif
