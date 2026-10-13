/*
 * What a store ingest costs in locks and in stops of the world.
 *
 * The hot shard's record list and the store's hot_ready set are written only
 * under commit_lock, so they carry no lock of their own, and a write release
 * on a rwlock always makes a futex wake. An ingest's scratch memory is reset
 * for the next call, because tearing it down goes through the deferred-destroy
 * queue, and a full queue stops the world.
 *
 * These count write acquisitions and write wakes per record through both
 * single-record entry points and the batch path, count stops of the world
 * over many single-record ingests through each entry point, and check that a
 * lock-free reader's record array survives the list growing past it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/runtime.h"
#include "core/rwlock.h"
#include "util/assert.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include <rocs/n00b_rocs.h>
#include <rocs/store.h>

#include "internal/rocs/store.h"
#include "rocs_test_support.h"
#include "test_check.h"

#ifndef N00B_DEBUG
#error "test_rocs_ingest_locks requires N00B_DEBUG for the rwlock counters"
#endif

#define RECORDS UINT64_C(256)

static n00b_vfs_t *
new_memory_vfs(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    auto be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));

    auto mount_r = n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0);
    CHECK(n00b_result_is_ok(mount_r));
    return vfs;
}

static n00b_store_t *
open_store(void)
{
    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    n00b_store_schema_t *schema = n00b_result_get(schema_r);

    CHECK(n00b_result_is_ok(
        n00b_store_schema_add_field(schema,
                                    r"kind",
                                    .index_kind = N00B_STORE_INDEX_TERM,
                                    .postings   = N00B_STORE_POSTINGS_DENSE)));
    CHECK(n00b_result_is_ok(
        n00b_store_schema_add_field(schema,
                                    r"message",
                                    .index_kind = N00B_STORE_INDEX_FULLTEXT)));

    auto store_r = n00b_store_open_vfs(new_memory_vfs(), r"/rocs", schema);
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

static n00b_json_node_t *
record_of(uint64_t i)
{
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put_n00b(rec, r"kind", n00b_json_string_new_from_n00b(r"log"));
    n00b_json_object_put_n00b(rec,
                              r"message",
                              n00b_json_string_new(i % 2 ? "odd tok" : "even tok"));
    return rec;
}

// The single-record entry points: n00b_store_ingest takes a parsed record and
// n00b_store_ingest_buf the record's JSON text.
typedef enum {
    SINGLE_JSON,
    SINGLE_BUF,
} single_path_t;

static n00b_string_t *
single_path_name(single_path_t path)
{
    return path == SINGLE_BUF ? r"n00b_store_ingest_buf" : r"n00b_store_ingest";
}

static void *
single_source_of(single_path_t path, uint64_t i)
{
    if (path == SINGLE_JSON) {
        return record_of(i);
    }
    char *text = i % 2 ? "{\"kind\":\"log\",\"message\":\"odd tok\"}"
                       : "{\"kind\":\"log\",\"message\":\"even tok\"}";
    return n00b_buffer_from_bytes(text, (int64_t)strlen(text));
}

static void
ingest_single(n00b_store_t *store, single_path_t path, void *source)
{
    if (path == SINGLE_JSON) {
        CHECK(n00b_result_is_ok(n00b_store_ingest(store, source)));
    }
    else {
        CHECK(n00b_result_is_ok(n00b_store_ingest_buf(store, source)));
    }
}

// Stops of the world so far: collections, and the barriers that drain
// deferred allocator teardowns. Each takes the world's gate for writing, once,
// on whichever thread starts it.
static uint64_t
world_stops(void)
{
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    CHECK(rt != nullptr);
    return atomic_load(&rt->gc_pause_count)
         + atomic_load(&rt->deferred_destroy_barrier_count);
}

// A single-threaded ingest takes no write lock and makes no write wake. The
// records before the counted ones build the columns and lists, whose one-time
// cost is not what this measures, and every source is built before counting.
static void
test_ingest_takes_no_write_lock(single_path_t path)
{
    n00b_store_t *store = open_store();
    for (uint64_t i = 0; i < 4; i++) {
        ingest_single(store, path, single_source_of(path, i));
    }
    void *sources[RECORDS];
    for (uint64_t i = 0; i < RECORDS; i++) {
        sources[i] = single_source_of(path, i);
    }

    uint64_t acquires = n00b_rw_debug_write_acquires();
    uint64_t wakes    = n00b_rw_debug_write_wakes();
    uint64_t stops    = world_stops();
    for (uint64_t i = 0; i < RECORDS; i++) {
        ingest_single(store, path, sources[i]);
    }
    acquires = n00b_rw_debug_write_acquires() - acquires;
    wakes    = n00b_rw_debug_write_wakes() - wakes;
    stops    = world_stops() - stops;

    n00b_printf("  «#», «#» records: «#» write acquisitions, «#» write wakes, "
                "«#» stops of the world",
                single_path_name(path),
                (int64_t)RECORDS,
                (int64_t)acquires,
                (int64_t)wakes,
                (int64_t)stops);
    CHECK(acquires == 0);
    CHECK(wakes == 0);
    n00b_printf("  [PASS] «#» takes no write lock per record",
                single_path_name(path));
}

static n00b_store_record_list_t *
batch_of(uint64_t count)
{
    n00b_store_record_list_t *records = n00b_alloc(n00b_store_record_list_t);
    *records = n00b_list_new_private(n00b_json_node_t *,
                                     .scan_kind = N00B_GC_SCAN_KIND_ALL);
    for (uint64_t i = 0; i < count; i++) {
        n00b_list_push(*records, record_of(i));
    }
    return records;
}

// The batch path takes no write lock and makes no write wake per record
// either. Each batch commits RECORDS / BATCHES records, and the first batch,
// which builds the columns and lists, is not counted.
static void
test_batch_ingest_takes_no_write_lock(void)
{
    const uint64_t batches = 8;
    const uint64_t per     = RECORDS / batches;

    n00b_store_t *store = open_store();
    auto          first = n00b_store_ingest_batch(store, batch_of(per));
    CHECK(n00b_result_is_ok(first) && n00b_result_get(first) == per);

    n00b_store_record_list_t *lists[8];
    for (uint64_t b = 0; b < batches; b++) {
        lists[b] = batch_of(per);
    }

    uint64_t acquires = n00b_rw_debug_write_acquires();
    uint64_t wakes    = n00b_rw_debug_write_wakes();
    uint64_t stops    = world_stops();
    for (uint64_t b = 0; b < batches; b++) {
        auto batch_r = n00b_store_ingest_batch(store, lists[b]);
        CHECK(n00b_result_is_ok(batch_r) && n00b_result_get(batch_r) == per);
    }
    acquires = n00b_rw_debug_write_acquires() - acquires;
    wakes    = n00b_rw_debug_write_wakes() - wakes;
    stops    = world_stops() - stops;

    n00b_printf("  «#» records in «#» batches: «#» write acquisitions, «#» "
                "write wakes, «#» stops of the world",
                (int64_t)RECORDS,
                (int64_t)batches,
                (int64_t)acquires,
                (int64_t)wakes,
                (int64_t)stops);
    CHECK(acquires == 0);
    CHECK(wakes == 0);
    n00b_printf("  [PASS] batch ingest takes no write lock per record");
}

// Single-record ingest does not stop the world. A full deferred-destroy queue
// stops it, so per-call scratch memory must not go back through that queue.
// Collections are left out: the records this test builds can cause them.
static void
test_single_ingest_stops_no_world(single_path_t path)
{
    const uint64_t ingests = 10000;

    n00b_runtime_t *rt     = n00b_default_runtime_or_null();
    n00b_store_t   *store  = open_store();
    uint64_t        drains = atomic_load(&rt->deferred_destroy_barrier_count);
    for (uint64_t i = 0; i < ingests; i++) {
        ingest_single(store, path, single_source_of(path, i));
    }
    drains = atomic_load(&rt->deferred_destroy_barrier_count) - drains;

    n00b_printf("  «#», «#» ingests: «#» deferred-destroy stops",
                single_path_name(path),
                (int64_t)ingests,
                (int64_t)drains);
    CHECK(drains == 0);
    n00b_printf("  [PASS] «#» stops no world", single_path_name(path));
}

// A reader that loaded the record array before a push grew the list still
// reads the records it saw. The shard pool zeroes a freed slot, so an array
// freed on growth reads back as nulls.
static void
test_record_growth_keeps_old_array(void)
{
    auto shard_r = n00b_store_shard_new(.shard_id  = UINT64_C(0x6C0C),
                                        .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    for (uint64_t i = 0; i < 4; i++) {
        CHECK(n00b_result_is_ok(n00b_store_shard_append(shard, record_of(i))));
    }

    size_t          seen_len = __atomic_load_n(&shard->records->len, __ATOMIC_ACQUIRE);
    n00b_string_t **seen     = __atomic_load_n(&shard->records->data, __ATOMIC_ACQUIRE);
    n00b_string_t  *texts[4];
    CHECK(seen_len == 4);
    for (size_t i = 0; i < seen_len; i++) {
        texts[i] = seen[i];
        CHECK(texts[i] != nullptr);
    }

    for (uint64_t i = 4; i < RECORDS; i++) {
        CHECK(n00b_result_is_ok(n00b_store_shard_append(shard, record_of(i))));
    }
    CHECK(__atomic_load_n(&shard->records->data, __ATOMIC_ACQUIRE) != seen);

    uint64_t intact = 0;
    for (size_t i = 0; i < seen_len; i++) {
        intact += __atomic_load_n(&seen[i], __ATOMIC_ACQUIRE) == texts[i];
    }
    n00b_printf("  «#» of «#» records intact in the outgrown array",
                (int64_t)intact,
                (int64_t)seen_len);
    CHECK(intact == seen_len);

    for (uint64_t i = 0; i < RECORDS; i++) {
        CHECK(n00b_store_shard_record_at(shard, i) != nullptr);
    }
    CHECK(n00b_store_shard_record_at(shard, RECORDS) == nullptr);
    CHECK(n00b_store_shard_records_len(shard) == RECORDS);
    n00b_printf("  [PASS] record growth keeps the old array");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    // An argument runs one case, so each can fail in its own run.
    int only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0 || only == 0) {
        test_ingest_takes_no_write_lock(SINGLE_JSON);
    }
    if (only < 0 || only == 1) {
        test_record_growth_keeps_old_array();
    }
    if (only < 0 || only == 2) {
        test_batch_ingest_takes_no_write_lock();
    }
    if (only < 0 || only == 3) {
        test_single_ingest_stops_no_world(SINGLE_JSON);
    }
    if (only < 0 || only == 4) {
        test_ingest_takes_no_write_lock(SINGLE_BUF);
    }
    if (only < 0 || only == 5) {
        test_single_ingest_stops_no_world(SINGLE_BUF);
    }

    n00b_shutdown();
    return 0;
}
