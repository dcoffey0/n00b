/**
 * @file rocs/shard.h
 * @brief Hot shard root and shard lifecycle declarations for rocs.
 *
 * A rocs shard is a marshalable n00b object graph while it is hot and a
 * read-only resident marshal image after seal. The public root layout below is
 * intentionally part of the mapped-image ABI: WP-001 mapped readers consume the
 * scalar prefix directly from sealed bytes and resolve pointer fields through a
 * shard map, never by unmarshaling.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "n00b.h"
#include "adt/dict.h"
#include "adt/flagset.h"
#include "adt/list.h"
#include "adt/result.h"
#include "conduit/topic.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"

typedef struct n00b_json_node n00b_json_node_t;

/**
 * @brief Public list of parsed records for batch ingest APIs.
 */
typedef n00b_list_t(n00b_json_node_t *) n00b_store_record_list_t;

/**
 * @brief Internal shard payload slots.
 *
 * Hot shards store compact JSON strings. Sealed shards store the same slots as
 * marshal vaddrs; callers must use rocs mapped/view APIs over sealed images.
 * Parsed JSON object graphs are transient ingest/query materialization state,
 * not durable shard payload.
 */
typedef n00b_list_t(n00b_string_t *) n00b_store_record_payload_list_t;

typedef n00b_list_t(uint64_t) n00b_store_posting_ordinal_list_t;

/**
 * @brief Physical posting representation used for one index field.
 *
 * Sparse postings store sorted record ordinals in a scalar list. Dense postings
 * store membership in a bit-backed flag set and materialize ordinals in
 * ascending order for public posting views. Query correctness and result order
 * must be identical for both representations.
 */
typedef enum : int32_t {
    N00B_STORE_POSTINGS_SPARSE = 0,
    N00B_STORE_POSTINGS_DENSE  = 1,
} n00b_store_postings_kind_t;

/**
 * @brief `reserved` bit marking a sparse posting list as ascending.
 *
 * Readers may binary-search a sparse posting list only when this is set.
 * Sealing verifies the order and sets it. A zero `reserved` word reads as
 * unordered and falls back to a linear scan, which keeps an image this bit
 * does not vouch for slow rather than silently short of results: a binary
 * search over unordered data finds only some of what is there.
 */
#define N00B_STORE_POSTINGS_ORDERED ((uint32_t)1u << 0)

/** @brief Marshalable posting object for a normalized field value. */
typedef struct n00b_store_posting_list {
    n00b_store_postings_kind_t         kind;
    // Atomic because the list's one writer clears it while hot readers test
    // it with no lock between them.
    //
    // The image holds this struct byte for byte, so atomicity must not move
    // or resize the field; map.c's static asserts enforce that.
    _Atomic uint32_t                   reserved;
    // Number of members, kept by the list's one writer and read lock-free.
    // Sealing recomputes it for a dense list from the set bits.
    //
    // Atomic because readers on the query path load it while the writer
    // stores it. The same layout constraint as `reserved` applies.
    //
    // A reader may still see an element land before the count describing it,
    // so the count is an estimate for that moment. Every consumer sizes work
    // from it; none decides membership by it.
    _Atomic uint64_t                   count;
    n00b_store_posting_ordinal_list_t *ordinals;
    n00b_flagset_t                    *flags;
} n00b_store_posting_list_t;

/**
 * @brief Establish ascending order on a sparse posting list.
 *
 * Pushes append, whatever order ordinals arrive in, so a list is ascending
 * until one lands below the tail. This sorts once and drops duplicates, then
 * sets @c N00B_STORE_POSTINGS_ORDERED. Idempotent; a no-op on a dense list or
 * one already ordered.
 *
 * @warning For the shard's sealer only, once no hot reader can reach the
 *          list. It renumbers the list in place with no lock, so it must not
 *          run alongside a reader or a push. A store drains the shard's hot
 *          readers before every seal, and sealing calls it, which is why
 *          every image is written ordered.
 *          Readers take a private copy and sort that, or consult
 *          @c N00B_STORE_POSTINGS_ORDERED and scan.
 */
extern void
rocs_posting_list_ensure_ordered(n00b_store_posting_list_t *postings);

/**
 * @brief Hash-keyed posting table for one field.
 *
 * Keys are kind-tagged 128-bit hashes of normalized values. Values are posting
 * objects containing record ordinals or dense record-membership bitmaps.
 */
typedef n00b_dict_t(n00b_uint128_t, n00b_store_posting_list_t *)
    n00b_store_column_t;

/** @brief Field-name to per-field posting table dictionary. */
typedef n00b_dict_t(n00b_string_t *, n00b_store_column_t *)
    n00b_store_columns_t;

/**
 * @brief Linear byte store for retained raw JSON.
 *
 * The default WP-003 backing is inline in the shard marshal image. Future
 * store/VFS work may leave this field null and satisfy spans from separate
 * durable raw-byte storage, but shard records still name raw bytes by offset
 * and length rather than by hot buffer pointers.
 */
typedef struct {
    uint8_t *data;
    uint64_t byte_len;
} n00b_store_raw_blob_t;

/**
 * @brief Per-record span into the shard raw-byte store.
 *
 * The span is scalar-only and therefore safe to expose in sealed images.
 */
typedef struct {
    uint64_t offset;
    uint64_t byte_len;
} n00b_store_raw_span_t;

/**
 * @brief Prepared payload for one reserved hot-shard slot.
 *
 * This is a process-local handoff object: it is never marshaled. The encoded
 * record text is already copied into the hot-shard allocator; the wrapper may
 * live in caller scratch until the dispatcher/worker publishes it into the
 * reserved ordinal. Raw-retaining shards need explicit raw byte-range
 * reservation before this object can carry raw spans safely.
 */
typedef struct {
    n00b_string_t         *record_text;
    n00b_store_raw_span_t *raw_span;
    uint64_t              byte_delta;
} n00b_store_shard_prepared_slot_t;

/**
 * @brief Borrowed byte span tied to an owning shard/map/stream lifetime.
 *
 * This is for scan paths that need to copy or write record bytes without
 * materializing a hot n00b string/buffer wrapper per record.
 */
typedef struct {
    uint8_t *data;
    uint64_t byte_len;
} n00b_store_byte_span_t;

/** @brief Optional byte-exact raw JSON spans parallel to records. */
typedef n00b_list_t(n00b_store_raw_span_t *) n00b_store_raw_list_t;

/** @brief Per-record accounting overhead used by Phase 2 byte estimates. */
#define N00B_STORE_SHARD_RECORD_OVERHEAD ((uint64_t)sizeof(void *))

/**
 * @brief Mutable shard lifecycle state.
 *
 * The underlying width is fixed because the value is part of the sealed shard
 * root prefix consumed by mapped readers.
 */
typedef enum : uint32_t {
    N00B_SHARD_STATE_OPEN    = 0,
    N00B_SHARD_STATE_SEALED  = 1,
    N00B_SHARD_STATE_DROPPED = 2,
} n00b_shard_state_t;

/**
 * @brief Error domain for hot shard operations.
 */
typedef enum : int32_t {
    N00B_STORE_SHARD_OK          = 0,
    N00B_STORE_SHARD_ERR_ARG     = -1,
    N00B_STORE_SHARD_ERR_STATE   = -2,
    N00B_STORE_SHARD_ERR_MARSHAL = -3,
    N00B_STORE_SHARD_ERR_EVENT   = -4,
} n00b_store_shard_err_t;

/**
 * @brief Shard lifecycle event kind.
 */
typedef enum : int32_t {
    N00B_STORE_LIFECYCLE_SEALED,
    N00B_STORE_LIFECYCLE_DROPPED,
} n00b_store_lifecycle_kind_t;

/**
 * @brief Shard-level lifecycle event payload.
 *
 * Lifecycle events describe shard seal/drop transitions only. They do not carry
 * partition paths, catalog paths, VFS storage handles, retention policy, or
 * per-record live-view payloads.
 */
typedef struct {
    n00b_store_lifecycle_kind_t  kind;
    uint64_t                     shard_id;
    uint64_t                     record_count;
    uint64_t                     byte_size;
    uint64_t                     open_ts;
    uint64_t                     seal_ts;
    n00b_string_t               *drop_reason;
} n00b_store_lifecycle_t;

N00B_CONDUIT_INBOX_IMPL(n00b_store_lifecycle_t);

typedef n00b_conduit_message_t(n00b_store_lifecycle_t)
    n00b_store_lifecycle_msg_t;
typedef n00b_conduit_inbox_t(n00b_store_lifecycle_t)
    n00b_store_lifecycle_inbox_t;
typedef n00b_conduit_topic_t(n00b_store_lifecycle_t)
    n00b_store_lifecycle_topic_t;

/** @brief Pop one shard lifecycle message from an inbox. */
#define n00b_store_lifecycle_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_store_lifecycle_t, inbox)

/** @brief Check whether a shard lifecycle inbox has queued messages. */
#define n00b_store_lifecycle_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_store_lifecycle_t, inbox)

/**
 * @brief Marshalable shard root.
 *
 * Ownership:
 * - `records`, `columns`, optional `retain_raw`, and optional `raw_bytes` are
 *   owned by the shard.
 * - Shard dictionaries may use the n00b typed-dict inline migration/bucket
 *   synchronization bits while hot. Mapped readers ignore synchronization-only
 *   bucket flags and never call ordinary dict APIs on sealed images.
 * - Shard lists may carry process-only allocator/lock metadata while hot only
 *   when seal-time sanitization clears that metadata before marshal. n00b list
 *   locks and allocators are process pointers and must not be embedded in a
 *   sealed shard image.
 * - Process-visible shard mutation is coordinated at the store/catalog
 *   boundary; no separate process lock object may be retained by this root.
 * - Process-only resources, including conduit lifecycle topics, VFS handles,
 *   residency pins, caches, and service state, must not be stored here because
 *   this root is sealed byte-for-byte into marshal images.
 *
 * Hot-vs-sealed contract:
 * - While `state == N00B_SHARD_STATE_OPEN`, ordinary hot-container APIs may be
 *   used by rocs implementation code.
 * - Once sealed, rocs readers must access the resident image through mapped
 *   views. They must not pass mapped `records` or `columns` internals to
 *   ordinary n00b list/dict APIs and must never unmarshal shard images.
 * - Raw retention is represented as per-record spans into a linear raw byte
 *   store. Mapped readers use the cold-buffer API rather than treating mapped
 *   raw bytes as hot `n00b_buffer_t` objects.
 */
typedef struct n00b_store_shard {
    n00b_store_record_payload_list_t *records;
    n00b_store_columns_t     *columns;
    n00b_store_raw_list_t    *retain_raw;
    n00b_store_raw_blob_t    *raw_bytes;
    n00b_shard_state_t        state;
    // Zero in every image. Debug builds hold it nonzero while a thread pushes
    // into this shard's posting lists, to catch a second concurrent writer.
    uint32_t                  reserved;
    uint64_t                  record_count;
    uint64_t                  byte_estimate;
    uint64_t                  open_ts;
    uint64_t                  seal_ts;
    uint64_t                  shard_id;
} n00b_store_shard_t;

static_assert(sizeof(n00b_store_raw_blob_t) == 16);
static_assert(offsetof(n00b_store_raw_blob_t, data) == 0);
static_assert(offsetof(n00b_store_raw_blob_t, byte_len) == 8);
static_assert(sizeof(n00b_store_raw_span_t) == 16);
static_assert(offsetof(n00b_store_raw_span_t, offset) == 0);
static_assert(offsetof(n00b_store_raw_span_t, byte_len) == 8);
static_assert(sizeof(n00b_store_byte_span_t) == 16);
static_assert(offsetof(n00b_store_byte_span_t, data) == 0);
static_assert(offsetof(n00b_store_byte_span_t, byte_len) == 8);

static_assert(sizeof(n00b_store_shard_t) == 80);
static_assert(offsetof(n00b_store_shard_t, records) == 0);
static_assert(offsetof(n00b_store_shard_t, columns) == 8);
static_assert(offsetof(n00b_store_shard_t, retain_raw) == 16);
static_assert(offsetof(n00b_store_shard_t, raw_bytes) == 24);
static_assert(offsetof(n00b_store_shard_t, state) == 32);
static_assert(offsetof(n00b_store_shard_t, reserved) == 36);
static_assert(offsetof(n00b_store_shard_t, record_count) == 40);
static_assert(offsetof(n00b_store_shard_t, byte_estimate) == 48);
static_assert(offsetof(n00b_store_shard_t, open_ts) == 56);
static_assert(offsetof(n00b_store_shard_t, seal_ts) == 64);
static_assert(offsetof(n00b_store_shard_t, shard_id) == 72);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Static diagnostic string for a shard error code.
 *
 * @param err  A @c N00B_STORE_SHARD_* code, usually from a result error branch.
 * @return A n00b string naming the code, or @c UNKNOWN for an unrecognized
 *         value.
 */
extern n00b_string_t *n00b_store_shard_err_str(n00b_err_t err);

/**
 * @brief Create or retrieve a typed process-side shard lifecycle topic.
 *
 * @param conduit Conduit instance that owns the topic.
 * @param uri     Conduit URI selected by the caller or store/service layer.
 *
 * @return Ok(topic) on success. Returns @c N00B_STORE_SHARD_ERR_EVENT when the
 *         conduit rejects topic creation.
 *
 * The returned topic is process-side state. It must not be stored in
 * @c n00b_store_shard_t or any other marshalable shard graph.
 */
extern n00b_result_t(n00b_store_lifecycle_topic_t *)
n00b_store_lifecycle_topic_get(n00b_conduit_t *conduit,
                               n00b_conduit_uri_t uri);

/**
 * @brief Allocate and initialize a typed lifecycle inbox.
 *
 * @param conduit Conduit instance whose allocator and notification domain own
 *                the inbox.
 *
 * @kw backpressure Inbox backpressure policy; defaults to unbounded.
 * @kw limit        Maximum queue depth for bounded policies; zero means
 *                  unbounded.
 * @kw allocator    Optional inbox allocator. Defaults to the conduit allocator.
 *
 * @return Ok(inbox) on success. Returns @c N00B_STORE_SHARD_ERR_ARG when
 *         @p conduit is null, or @c N00B_STORE_SHARD_ERR_EVENT when inbox
 *         allocation or initialization fails.
 */
extern n00b_result_t(n00b_store_lifecycle_inbox_t *)
n00b_store_lifecycle_inbox_new(n00b_conduit_t *conduit) _kargs
{
    n00b_conduit_backpressure_t backpressure = N00B_CONDUIT_BP_UNBOUNDED;
    uint32_t                    limit        = 0;
    n00b_allocator_t           *allocator    = nullptr;
};

/**
 * @brief Subscribe an inbox to a shard lifecycle topic.
 *
 * @param topic Process-side lifecycle topic returned by
 *              @c n00b_store_lifecycle_topic_get.
 * @param inbox Lifecycle inbox returned by @c n00b_store_lifecycle_inbox_new.
 *
 * @kw operations    Conduit operation mask. Zero defaults to all operations.
 * @kw flags         Conduit subscription flags. Zero uses normal active
 *                   subscription behavior.
 * @kw timeout_ms    Optional subscription timeout in milliseconds. Zero means
 *                   no timeout.
 * @kw backpressure  Subscription backpressure policy; defaults to unbounded.
 * @kw inbox_limit   Subscription inbox limit for bounded policies. Zero means
 *                   unbounded.
 *
 * @return Ok(subscription handle) on success. Returns
 *         @c N00B_STORE_SHARD_ERR_ARG for null required inputs, or
 *         @c N00B_STORE_SHARD_ERR_EVENT when the topic is inactive or the
 *         conduit rejects subscription.
 */
extern n00b_result_t(n00b_conduit_sub_handle_t)
n00b_store_lifecycle_subscribe(n00b_store_lifecycle_topic_t *topic,
                               n00b_store_lifecycle_inbox_t *inbox) _kargs
{
    uint32_t                    operations   = N00B_CONDUIT_OP_ALL;
    uint32_t                    flags        = 0;
    uint32_t                    timeout_ms   = 0;
    n00b_conduit_backpressure_t backpressure = N00B_CONDUIT_BP_UNBOUNDED;
    uint32_t                    inbox_limit  = 0;
};

/**
 * @brief Publish one lifecycle event payload to a lifecycle topic.
 *
 * @param topic Process-side lifecycle topic.
 * @param event Lifecycle payload. The payload is copied into the queued
 *              message; referenced strings remain caller-owned/GC-owned.
 *
 * @return Ok(true) when a message was published, Ok(false) for a null topic,
 *         or @c N00B_STORE_SHARD_ERR_EVENT when a supplied topic is inactive
 *         or cannot accept the event.
 * @post The topic and queued event remain process-side only.
 */
extern n00b_result_t(bool)
n00b_store_lifecycle_publish(n00b_store_lifecycle_topic_t *topic,
                             n00b_store_lifecycle_t        event);

/**
 * @brief Construct an empty open shard root.
 *
 * @kw shard_id   Store-global shard identifier to place in the marshalable root.
 * @kw retain_raw If true, allocate an empty raw-span list parallel to records
 *                 plus an inline raw-byte backing blob. If false, both
 *                 `retain_raw` and `raw_bytes` remain `nullptr`.
 * @kw open_ts    Opening timestamp to place in the marshalable root. A value of
 *                 zero means the caller has not assigned one yet.
 * @kw allocator  Allocator for the shard root and owned hot containers.
 * @kw record_cap Pre-sized capacity for the (locked) record list so it never
 *                 reallocs as records are appended. Callers creating the growing
 *                 hot shard pass the seal policy's max_records; 0 keeps the
 *                 default-capacity list.
 *
 * @return A result containing an owned hot shard root on success.
 *
 * No lifecycle topic is accepted here: topic handles are process-owned conduit
 * resources, not marshalable shard state. Lifecycle seal/drop operations
 * configure and emit through process-side handles.
 */
extern n00b_result_t(n00b_store_shard_t *)
n00b_store_shard_new() _kargs
{
    uint64_t          shard_id   = 0;
    bool              retain_raw = false;
    uint64_t          open_ts    = 0;
    n00b_allocator_t *allocator  = nullptr;
    uint64_t          record_cap = 0;
};

/**
 * @brief Append one parsed JSON record to an open hot shard.
 *
 * @param shard  Hot shard root returned by @c n00b_store_shard_new.
 * @param record Parsed JSON record to append. The shard stores a compact JSON
 *               text copy and does not retain the parsed object graph.
 *
 * @kw raw Optional byte-exact source buffer for raw retention. If the shard was
 *         constructed with @c .retain_raw = true, this kwarg is required and the
 *         shard appends an independent byte copy to its linear raw-byte store,
 *         recording a scalar span for the record. If raw retention is disabled,
 *         this kwarg is ignored after decode and no raw-retention storage is
 *         allocated.
 *
 * @return Ok(ordinal) on success, where ordinal is the zero-based record
 *         position within the shard. Returns @c N00B_STORE_SHARD_ERR_ARG for
 *         null required inputs or accounting overflow, and
 *         @c N00B_STORE_SHARD_ERR_STATE when the shard is not open.
 *
 * @post On success, @c record_count mirrors @c records length and
 *       @c byte_estimate increases by @c N00B_STORE_SHARD_RECORD_OVERHEAD plus
 *       compact JSON text bytes plus retained source byte length when raw
 *       retention is enabled.
 * @post On error, shard contents and counters are unchanged.
 *
 * Index population is intentionally out of scope for this function; WP-004 owns
 * @c columns contents.
 */
extern n00b_result_t(uint64_t)
n00b_store_shard_append(n00b_store_shard_t *shard,
                        n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t *raw = nullptr;
};

/**
 * @brief Reserve contiguous hot-shard record slots in consume order.
 *
 * @param shard Hot shard root returned by @c n00b_store_shard_new.
 * @param count Number of slots to reserve.
 *
 * @return Ok(start_ordinal) on success. The reserved range is
 *         [start_ordinal, start_ordinal + count). Zero count is a no-op that
 *         returns the current tail ordinal.
 *
 * @post On success, @c record_count and record-list length increase by
 *       @p count, and every new record slot is null until explicitly filled.
 *       For raw-retaining shards, the raw-span list grows in parallel with
 *       null spans.
 * @post Reserved but unfilled slots are not visible to query/egress; callers
 *       must advance the catalog live watermark only after filling row data and
 *       configured indexes.
 */
extern n00b_result_t(uint64_t)
n00b_store_shard_reserve(n00b_store_shard_t *shard,
                         uint64_t            count);

/**
 * @brief Length of a shard's record list, read without a lock.
 *
 * The list has one writer. This counts reserved slots too, so a slot below
 * the length may still be null until it is filled.
 */
extern uint64_t n00b_store_shard_records_len(n00b_store_shard_t *shard);

/**
 * @brief The stored text of one record, read without a lock.
 *
 * @return The record's compact JSON text, or null when @p ordinal is past the
 *         list or names a slot that is reserved but not yet filled.
 */
extern n00b_string_t *n00b_store_shard_record_at(n00b_store_shard_t *shard,
                                                 uint64_t            ordinal);

/**
 * @brief Fill one previously reserved hot-shard slot.
 *
 * @param shard   Hot shard root returned by @c n00b_store_shard_new.
 * @param ordinal Reserved ordinal to fill.
 * @param record  Parsed JSON record. The shard stores a compact JSON text copy.
 *
 * @kw raw Optional byte-exact source buffer; required for raw-retaining shards.
 *
 * @return Ok(true) when the slot is populated. Returns STATE if the slot is not
 *         reserved, already populated, or the shard is not open.
 *
 * Index population is intentionally out of scope; callers install index entries
 * before publishing the slot through the catalog live watermark.
 */
extern n00b_result_t(bool)
n00b_store_shard_fill_reserved(n00b_store_shard_t *shard,
                               uint64_t            ordinal,
                               n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t *raw = nullptr;
};

/**
 * @brief Reserve and copy one raw source buffer into a raw-retaining shard.
 *
 * This mutates the shard's linear raw-byte store and is therefore intended for
 * the single dispatcher / serialized reservation path. The returned scalar span
 * can be handed to a worker and later installed into the reserved ordinal via
 * @ref n00b_store_shard_fill_prepared_reserved.
 */
extern n00b_result_t(n00b_store_raw_span_t *)
n00b_store_shard_reserve_raw_span(n00b_store_shard_t *shard,
                                  n00b_buffer_t      *raw) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Encode one record for later publication into a reserved slot.
 *
 * @param shard  Hot shard whose allocator owns the encoded record text.
 * @param record Parsed JSON record to encode.
 *
 * @kw raw Optional raw source for byte-estimate calculation when raw retention
 *         is disabled. Raw-retaining callers must pass @c raw_span instead.
 * @kw raw_span Pre-reserved raw span for raw-retaining shards.
 * @kw allocator Optional allocator for the small prepared-slot wrapper.
 *
 * @return Ok(prepared) on success. The encoded record text is owned by the hot
 *         shard allocator, not by the wrapper allocator.
 */
extern n00b_result_t(n00b_store_shard_prepared_slot_t *)
n00b_store_shard_prepare_reserved_slot(n00b_store_shard_t *shard,
                                       n00b_json_node_t   *record) _kargs
{
    n00b_buffer_t          *raw       = nullptr;
    n00b_store_raw_span_t  *raw_span  = nullptr;
    n00b_allocator_t       *allocator = nullptr;
};

/**
 * @brief Publish a prepared record payload into one reserved hot-shard slot.
 *
 * The caller must already have reserved @p ordinal and must install all
 * configured indexes before advancing the catalog live watermark.
 */
extern n00b_result_t(bool)
n00b_store_shard_fill_prepared_reserved(
    n00b_store_shard_t               *shard,
    uint64_t                          ordinal,
    n00b_store_shard_prepared_slot_t *prepared) _kargs
{
    bool account_byte_estimate = true;
};

/**
 * @brief Cancel an unfilled tail reservation.
 *
 * @param shard Hot shard root returned by @c n00b_store_shard_new.
 * @param start Start ordinal of the tail reservation.
 * @param count Number of reserved slots to cancel.
 *
 * This is a rollback helper for the current single-worker path. Multi-worker
 * ingest must not shrink arbitrary holes; malformed rows should be filled with
 * tombstone/error records so the live watermark can progress.
 */
extern n00b_result_t(bool)
n00b_store_shard_cancel_tail_reservation(n00b_store_shard_t *shard,
                                         uint64_t            start,
                                         uint64_t            count);

/**
 * @brief Seal an open hot shard into an owned resident-image buffer.
 *
 * @param shard  Open hot shard root returned by @c n00b_store_shard_new.
 *
 * @kw seal_ts      Seal timestamp to store in the marshalable root. A value of
 *                  zero means the caller has not assigned one yet.
 * @kw base_address High 32 bits to place in marshal vaddrs. Zero is valid and
 *                  remains the default.
 * @kw topic        Optional process-side lifecycle topic. When supplied, a
 *                  sealed event is emitted after the shard image is produced.
 * @kw allocator    Allocator for the returned image buffer.
 *
 * @return Ok(buffer) containing the complete marshal v4 payload-front image on
 *         success. Returns @c N00B_STORE_SHARD_ERR_ARG for invalid inputs,
 *         @c N00B_STORE_SHARD_ERR_STATE when the shard is not open, and
 *         @c N00B_STORE_SHARD_ERR_MARSHAL if serialization fails, or
 *         @c N00B_STORE_SHARD_ERR_EVENT when a supplied lifecycle topic is
 *         inactive or cannot be published to.
 *
 * @post On success, @c state becomes @c N00B_SHARD_STATE_SEALED, @c seal_ts is
 *       recorded in the shard root and the returned image, and later mutation
 *       attempts fail with @c N00B_STORE_SHARD_ERR_STATE.
 * @post On marshal failure, @c state and @c seal_ts are restored to their
 *       pre-call values.
 * @post If lifecycle event emission fails, @c state and @c seal_ts are
 *       restored and @c N00B_STORE_SHARD_ERR_EVENT is returned.
 *
 * The returned buffer is process-owned output. It is not stored in the shard
 * root because residency, VFS handles, cache files, and lifecycle topics are
 * process-only resources. rocs readback opens this image through mapped-view
 * APIs and never unmarshals shard images.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_store_shard_seal(n00b_store_shard_t *shard) _kargs
{
    uint64_t                      seal_ts      = 0;
    uint32_t                      base_address = 0;
    n00b_store_lifecycle_topic_t *topic        = nullptr;
    n00b_allocator_t             *allocator    = nullptr;
};

/**
 * @brief Transition a sealed shard to dropped and optionally emit an event.
 *
 * @param shard Sealed hot shard root returned by @c n00b_store_shard_new and
 *              sealed by @c n00b_store_shard_seal.
 *
 * @kw topic       Optional process-side lifecycle topic. When supplied, a
 *                 dropped event is emitted after the state transition.
 * @kw drop_reason Optional human-readable drop reason carried only by dropped
 *                 lifecycle events. The pointer is retained by the queued event
 *                 message but is not copied into the shard root.
 * @kw byte_size   Actual sealed-object byte size when known by the caller.
 *                 Zero defaults to the shard's current byte estimate.
 *
 * @return Ok(true) on success. Returns @c N00B_STORE_SHARD_ERR_ARG for invalid
 *         inputs, @c N00B_STORE_SHARD_ERR_STATE when the shard is not sealed,
 *         and @c N00B_STORE_SHARD_ERR_EVENT when a supplied lifecycle topic is
 *         inactive or cannot be published to.
 *
 * @post On success, @c state becomes @c N00B_SHARD_STATE_DROPPED and later
 *       mutation/seal/drop attempts fail with @c N00B_STORE_SHARD_ERR_STATE.
 * @post On event failure, @c state is restored.
 *
 * This is an in-memory shard lifecycle transition only. Durable retention
 * deletion, VFS paths, catalog generations, and residency policy are store-layer
 * responsibilities in later work plans.
 */
extern n00b_result_t(bool)
n00b_store_shard_drop(n00b_store_shard_t *shard) _kargs
{
    n00b_store_lifecycle_topic_t *topic       = nullptr;
    n00b_string_t                *drop_reason = nullptr;
    uint64_t                      byte_size   = 0;
};

#ifdef __cplusplus
}
#endif
