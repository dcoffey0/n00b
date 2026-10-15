/*
 * What a hot posting list costs in locks.
 *
 * Every push into a shard's posting lists runs under the store's commit_lock,
 * so the lists have one writer, and readers pin the shard and read below the
 * published bound. Neither side needs a lock of the list's own. A write
 * release on a rwlock always makes a futex wake, so a lock taken per push
 * costs a syscall per indexed term, and a read lock taken per membership test
 * costs an acquisition per candidate.
 *
 * These count write acquisitions, write wakes, and read acquisitions with the
 * debug rwlock counters, and check that every hot posting list an ingest
 * builds carries no lock.
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
#error "test_rocs_posting_writes requires N00B_DEBUG for the rwlock counters"
#endif

#define RECORDS UINT64_C(256)
#define TOKENS  32

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
open_store(bool with_fulltext)
{
    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    n00b_store_schema_t *schema = n00b_result_get(schema_r);

    CHECK(n00b_result_is_ok(
        n00b_store_schema_add_field(schema,
                                    r"kind",
                                    .index_kind = N00B_STORE_INDEX_TERM,
                                    .postings   = N00B_STORE_POSTINGS_DENSE)));
    if (with_fulltext) {
        CHECK(n00b_result_is_ok(
            n00b_store_schema_add_field(schema,
                                        r"message",
                                        .index_kind = N00B_STORE_INDEX_FULLTEXT)));
    }

    auto store_r = n00b_store_open_vfs(new_memory_vfs(), r"/rocs", schema);
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

// The same token TOKENS times. Every repeat resolves to the one posting list,
// so a store indexing the message pushes into it TOKENS times per record, and
// all but the first push is the duplicate the tail check drops.
static n00b_json_node_t *
record_of(void)
{
    char message[TOKENS * 4 + 1] = {};
    for (int i = 0; i < TOKENS; i++) {
        memcpy(message + i * 4, "tok ", 4);
    }
    message[TOKENS * 4 - 1] = '\0';

    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put_n00b(rec, r"kind", n00b_json_string_new_from_n00b(r"log"));
    n00b_json_object_put_n00b(rec, r"message", n00b_json_string_new(message));
    return rec;
}

typedef struct {
    uint64_t write_acquires;
    uint64_t write_wakes;
} ingest_cost_t;

// Write acquisitions and write wakes this thread makes ingesting RECORDS
// records, after a few that build the columns and lists so their one-time
// cost is not counted.
static ingest_cost_t
ingest_cost(n00b_store_t *store)
{
    for (int i = 0; i < 4; i++) {
        CHECK(n00b_result_is_ok(n00b_store_ingest(store, record_of())));
    }

    uint64_t acquires = n00b_rw_debug_write_acquires();
    uint64_t wakes    = n00b_rw_debug_write_wakes();
    for (uint64_t i = 0; i < RECORDS; i++) {
        CHECK(n00b_result_is_ok(n00b_store_ingest(store, record_of())));
    }
    return (ingest_cost_t){
        .write_acquires = n00b_rw_debug_write_acquires() - acquires,
        .write_wakes    = n00b_rw_debug_write_wakes() - wakes,
    };
}

static n00b_store_shard_t *
pin_hot_shard(n00b_store_t *store)
{
    auto snap_r = n00b_store_tail_snapshot(store);
    CHECK(n00b_result_is_ok(snap_r));
    n00b_store_tail_snapshot_t snap = n00b_result_get(snap_r);
    CHECK(snap.has_hot_through);

    auto hot_r = n00b_store_hot_shard_acquire(store, snap.hot_through);
    CHECK(n00b_result_is_ok(hot_r));
    n00b_option_t(n00b_store_shard_t *) hot_opt = n00b_result_get(hot_r);
    CHECK(n00b_option_is_set(hot_opt));
    return n00b_option_get(hot_opt);
}

// A store indexing 32 extra terms per record takes the same write locks per
// record, and makes the same write wakes, as one that does not: the pushes
// take no lock.
static void
test_posting_pushes_take_no_write_lock(void)
{
    n00b_store_t *dense_only    = open_store(false);
    n00b_store_t *with_fulltext = open_store(true);

    ingest_cost_t dense    = ingest_cost(dense_only);
    ingest_cost_t fulltext = ingest_cost(with_fulltext);

    n00b_printf("  write acquisitions per record: «#» dense only, «#» with «#» "
                "full-text terms each",
                (int64_t)(dense.write_acquires / RECORDS),
                (int64_t)(fulltext.write_acquires / RECORDS),
                (int64_t)TOKENS);
    n00b_printf("  write wakes over «#» records: «#» dense only, «#» with full "
                "text",
                (int64_t)RECORDS,
                (int64_t)dense.write_wakes,
                (int64_t)fulltext.write_wakes);

    CHECK(fulltext.write_acquires == dense.write_acquires);
    CHECK(fulltext.write_wakes == dense.write_wakes);
    n00b_printf("  [PASS] posting pushes take no write lock");
}

// Every posting list an ingest built is lock-free by construction.
static void
test_ingested_posting_lists_carry_no_lock(void)
{
    n00b_store_t *store = open_store(true);
    for (uint64_t i = 0; i < 16; i++) {
        CHECK(n00b_result_is_ok(n00b_store_ingest(store, record_of())));
    }

    n00b_store_shard_t *hot     = pin_hot_shard(store);
    uint64_t            dense   = 0;
    uint64_t            sparse  = 0;
    uint64_t            locked  = 0;
    n00b_dict_foreach(hot->columns, field, column, {
        (void)field;
        n00b_dict_foreach(column, key, postings, {
            (void)key;
            if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
                dense++;
                locked += postings->flags->lock != nullptr;
            }
            else {
                sparse++;
                locked += postings->ordinals->lock != nullptr;
            }
        });
    });
    n00b_store_hot_shard_release(store);

    n00b_printf("  «#» dense and «#» sparse lists, «#» locked",
                (int64_t)dense,
                (int64_t)sparse,
                (int64_t)locked);
    CHECK(dense > 0);
    CHECK(sparse > 0);
    CHECK(locked == 0);
    n00b_printf("  [PASS] ingested posting lists carry no lock");
}

// A two-term lookup enumerates the smaller list and tests each candidate
// against the other. The read acquisitions it makes must not grow with the
// number of candidates.
static void
test_lookup_reads_take_no_lock_per_candidate(void)
{
    const uint64_t rows  = 1024;
    auto           shard_r = n00b_store_shard_new(.shard_id  = UINT64_C(0x10C4),
                                                  .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);
    n00b_store_index_t *index = index_of(r"message", N00B_STORE_INDEX_FULLTEXT);

    for (uint64_t i = 0; i < rows; i++) {
        n00b_json_node_t *rec = n00b_json_object_new();
        n00b_json_object_put_n00b(
            rec,
            r"message",
            n00b_json_string_new_from_n00b(i < 16 ? r"beta few many"
                                                  : r"beta many"));
        auto a_r = n00b_store_shard_append(shard, rec);
        CHECK(n00b_result_is_ok(a_r));
        CHECK(n00b_result_is_ok(
            n00b_store_index_add(index, shard, n00b_result_get(a_r))));
    }

    n00b_json_node_t *few  = n00b_json_string_new_from_n00b(r"few beta");
    n00b_json_node_t *many = n00b_json_string_new_from_n00b(r"many beta");
    uint64_t          acquires[2];
    uint64_t          found[2];
    n00b_json_node_t *values[2] = {few, many};

    for (int v = 0; v < 2; v++) {
        // Once untimed, so anything the first lookup builds is not counted.
        CHECK(n00b_result_is_ok(n00b_store_index_lookup(index, shard, values[v])));

        uint64_t before = n00b_rw_debug_read_acquires();
        auto     look_r = n00b_store_index_lookup(index, shard, values[v]);
        acquires[v]     = n00b_rw_debug_read_acquires() - before;
        CHECK(n00b_result_is_ok(look_r));
        auto len_r = n00b_store_postings_len(n00b_result_get(look_r));
        CHECK(n00b_result_is_ok(len_r));
        found[v] = n00b_result_get(len_r);
    }

    n00b_printf("  read acquires: «#» for «#» candidates, «#» for «#»",
                (int64_t)acquires[0],
                (int64_t)found[0],
                (int64_t)acquires[1],
                (int64_t)found[1]);
    CHECK(found[0] == 16);
    CHECK(found[1] == rows);
    // Larger result arrays can cost an allocation a few gate acquisitions
    // more; one per candidate would be about a thousand.
    CHECK(acquires[1] <= acquires[0] + 8);
    n00b_printf("  [PASS] lookup read locks do not scale with candidates");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    // An argument runs one case, so each can fail in its own run.
    int only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0 || only == 0) {
        test_posting_pushes_take_no_write_lock();
    }
    if (only < 0 || only == 1) {
        test_ingested_posting_lists_carry_no_lock();
    }
    if (only < 0 || only == 2) {
        test_lookup_reads_take_no_lock_per_candidate();
    }

    n00b_shutdown();
    return 0;
}
