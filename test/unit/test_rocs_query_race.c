/*
 * n00b#547: a newest-first reader paging a hot shard while a writer commits
 * batches into it must not crash.
 *
 * crashappsec/wax's rocs_query_race started failing about one attempt in ten
 * after the libn00b repin to 53949e28 (SIGSEGV si_code=128 on Linux x86-64,
 * 0xc0000005 on Windows x64, on the reader thread). wax CI keeps no binaries,
 * so the frames were never symbolicated. This is the same race with no wax in
 * it, mirroring the shape of crayon_rocs_cache_query_stream and wax's test:
 *
 *   - a SERVICE store opened as crayon-gw opens it: one ingest worker, one
 *     seal worker, the wax schema, partition and seal policies and source
 *     decoder, a commit topic, all allocated from rt->user_pool;
 *   - a writer committing 256-row batches built in user_pool and freed the
 *     moment each batch returns;
 *   - a reader that anchors at the newest baseline row and drains from it in
 *     pages of 25. The anchor is a hot-shard position, so each page is NOT
 *     bounded by as_of: it walks from the newest row -- through everything
 *     the writer added -- and skips down to the anchor by hand;
 *   - per query, a view and cursor allocated from an n00b_pool_t that lives in
 *     the reader's stack frame and was initialized with the pool defaults;
 *   - per row, the record and its JSON text fetched with a scratch arena as
 *     the CURRENT allocator, the arena reset after every row, and the text
 *     parsed on the default heap;
 *   - wax's exact-ID oracle: every drain returns each baseline row below the
 *     anchor exactly once.
 *
 * WHAT IT FOUND. Two defects on that path, each fixed alongside this test:
 *
 *   1. The per-query pool. n00b_pool_init's defaults made a pool the collector
 *      could see INTO but never trace OUT of (no inline header, no OOB record),
 *      so the store and shards the view and cursor hold were invisible to the
 *      mark. Under the copying collector the store moved and the view's copy
 *      went stale: in a macOS build without N00B_DEBUG this test crashed 6 of
 *      6 runs, every time at store->state in
 *      n00b_store_hot_record_copy_for_pos, at the store's ORIGINAL address.
 *      With N00B_DEBUG on (CI's configuration) an incidental stack word pins
 *      the store and this race passes even unfixed, so the gate for this one
 *      is test_gc_pool_trace, which fails deterministically under both
 *      collectors without the fix.
 *   2. Debug builds only: _n00b_string_from_raw_at patched each allocation's
 *      OOB file_name after the fact, looking the record up again and writing
 *      through it with no metadata gate held. A collection between the lookup
 *      and the write turned it into a store into a dropped metadata arena:
 *      19 of 23 runs on Linux x86-64 with collection pressure.
 *
 * WHAT IT DID NOT FIND: wax's own Linux/Windows release signature. A release,
 * pin-all build of this test ran 30 of 30 clean on Linux x86-64 under the
 * same pressure, so whatever else wax's binary does is not reproduced here.
 *
 * COLLECTION PRESSURE. A race involving the collector fires only when a
 * collection lands inside the reader's window, and this test alone collects
 * once or twice in 15 s. N00B_QUERY_RACE_GC_PRESSURE=N adds a thread that
 * drops N 32 KB allocations per millisecond; at 16 it collects about 50 times
 * in 10 s, which is what both defects above needed. The registered test runs
 * with it on.
 *
 * Sized by N00B_QUERY_RACE_SECONDS (default 4) and N00B_QUERY_RACE_ROWS.
 * Anti-vacuity: a run where the writer committed nothing, the reader drained
 * nothing, or any drain was inexact fails.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "parsers/json.h"
#include "rocs/filter.h"
#include "rocs/query.h"
#include "rocs/store.h"
#include "rocs/wax.h"
#include "util/assert.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "query race check failed: " #expr);               \
    } while (0)

#define SEEDED_KIND  "proc.spawn"
#define BASELINE     200
#define BATCH_ROWS   256
#define PAGE_ROWS    25
#define MAX_PAGES    16

static uint64_t run_seconds = 4;
static uint64_t max_rows    = 100000;
// 32 KB allocations dropped per millisecond by the pressure thread; 0 = none.
static uint64_t gc_pressure = 0;

static n00b_store_t     *store;
static _Atomic(bool)     stop_flag;
static _Atomic(uint64_t) appended;
static _Atomic(uint64_t) batch_errors;
static _Atomic(uint64_t) batch_worst_ns;

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

// The row wax's test writes, byte for byte apart from the sequence numbers.
static n00b_allocator_t *
user_pool(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->user_pool;
}

static n00b_buffer_t *
row_buffer(uint64_t ts_ns, uint64_t seq)
{
    char line[512];
    int  len = snprintf(
        line,
        sizeof(line),
        "{\"schema\":\"wax.normalized.v1\",\"kind\":\"" SEEDED_KIND "\","
        "\"event_id\":\"wax:queryrace:%llu\",\"ts_ns\":%llu,"
        "\"source\":{\"family\":\"proc\",\"seq\":%llu},"
        "\"quality\":{\"state\":\"synthetic\"},"
        "\"body\":{\"pid\":%llu,\"ppid\":1,\"argv\":\"race-row\"}}",
        (unsigned long long)seq,
        (unsigned long long)ts_ns,
        (unsigned long long)(70000u + seq),
        (unsigned long long)(40000u + seq));
    CHECK(len > 0 && (size_t)len < sizeof(line));
    // From user_pool, as wax's writer allocates them.
    return n00b_buffer_from_bytes(line, (int64_t)len, .allocator = user_pool());
}

static bool
ingest_batch(uint64_t first_seq, uint64_t count)
{
    n00b_store_source_list_t *sources = n00b_alloc_with_opts(
        n00b_store_source_list_t,
        &(n00b_alloc_opts_t){.allocator = user_pool()});
    *sources = n00b_list_new_private(n00b_buffer_t *,
                                     .allocator = user_pool(),
                                     .scan_kind = N00B_GC_SCAN_KIND_ALL);
    for (uint64_t i = 0; i < count; i++) {
        n00b_list_push(*sources, row_buffer(now_ns(), first_seq + i));
    }
    auto r  = n00b_store_ingest_buf_batch(store, sources);
    bool ok = n00b_result_is_ok(r) && n00b_result_get(r) == count;

    // wax frees every source the moment the batch returns
    // (crayon_rocs_cache_ingest_lines_batch). That recycles user_pool memory
    // at once, which is what turns any reference the store kept into a
    // read of someone else's bytes.
    size_t submitted = n00b_list_len(*sources);
    for (size_t i = 0; i < submitted; i++) {
        n00b_buffer_t *line = n00b_list_get(*sources, i);
        if (line != nullptr) {
            n00b_buffer_free(line);
            n00b_free(line);
        }
    }
    return ok;
}

static void *
writer_main(void *unused)
{
    (void)unused;
    uint64_t seq = BASELINE;
    while (!n00b_atomic_load(&stop_flag) && seq < max_rows) {
        uint64_t count = BATCH_ROWS;
        if (seq + count > max_rows) {
            count = max_rows - seq;
        }
        uint64_t t0 = now_ns();
        if (!ingest_batch(seq, count)) {
            n00b_atomic_add(&batch_errors, 1);
        }
        uint64_t took = now_ns() - t0;
        if (took > n00b_atomic_load(&batch_worst_ns)) {
            n00b_atomic_store(&batch_worst_ns, took);
        }
        seq += count;
        n00b_atomic_add(&appended, count);
    }
    return nullptr;
}

static void *
pressure_main(void *unused)
{
    (void)unused;
    while (!n00b_atomic_load(&stop_flag)) {
        for (uint64_t i = 0; i < gc_pressure; i++) {
            void **junk = n00b_alloc_array(void *, 4096);
            CHECK(junk != nullptr);
            junk[0] = junk;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

typedef struct {
    uint64_t queries;
    uint64_t rows;
    uint64_t query_errors;
    uint64_t complete_drains;
    uint64_t incomplete_drains;
    // Seqs delivered by the page in progress, for the drain oracle.
    uint64_t page_seqs[PAGE_ROWS];
    uint64_t page_rows;
} reader_stats_t;

// One newest-first page, shaped like crayon_rocs_cache_query_stream: a
// per-query pool in this frame, a filtered view, a reversed cursor, and per
// hit its position and JSON text. Returns the last position delivered, which
// pages the next call, or a zero position at the end of the set.
static n00b_store_pos_t
newest_page(reader_stats_t  *stats,
            n00b_store_pos_t before,
            bool             have_before,
            uint64_t         limit)
{
    n00b_store_pos_t last = {};
    stats->page_rows      = 0;

    // Deliberately stack-resident, as wax's is.
    n00b_pool_t       qpool  = {};
    n00b_allocator_t *qalloc = n00b_pool_init(&qpool,
                                              .use_epochs = false,
                                              .name       = "rocs_query_race");

    auto field_r = n00b_filter_field(r"kind", .allocator = qalloc);
    CHECK(n00b_result_is_ok(field_r));
    auto filter_r = n00b_filter_eq(
        n00b_result_get(field_r),
        n00b_fv_utf8(n00b_string_from_cstr(SEEDED_KIND, .allocator = qalloc)),
        .allocator = qalloc);
    CHECK(n00b_result_is_ok(filter_r));

    auto pin_r = n00b_store_pin_acquire(store);
    CHECK(n00b_result_is_ok(pin_r));
    n00b_store_pin_t *qpin = n00b_result_get(pin_r);

    // wax does NOT bound a descending page with `as_of` when `before` is in
    // the hot shard: the live view starts at the newest row and the previous
    // page's rows are skipped by hand below. So every hot-shard page walks the
    // shard's live tail, where the writer is mid-commit. Bounding it with
    // as_of instead never reaches the tail and is not the same test.
    bool manual_before_bound = false;
    if (have_before) {
        auto memory_r = n00b_store_memory_stats(store);
        if (n00b_result_is_ok(memory_r)) {
            manual_before_bound
                = before.shard_id != 0
               && before.shard_id == n00b_result_get(memory_r).hot_shard_id;
        }
    }

    auto view_r = n00b_query_view(
        store,
        n00b_result_get(filter_r),
        .as_of     = (have_before && !manual_before_bound) ? &before : nullptr,
        .allocator = qalloc);
    if (n00b_result_is_err(view_r)) {
        stats->query_errors++;
        (void)n00b_store_pin_release(qpin);
        n00b_allocator_destroy(qalloc);
        return last;
    }
    n00b_query_view_t *view = n00b_result_get(view_r);

    auto cursor_r = n00b_query_cursor(view, .allocator = qalloc, .reverse = true);
    if (n00b_result_is_err(cursor_r)) {
        stats->query_errors++;
        (void)n00b_query_view_close(view);
        (void)n00b_store_pin_release(qpin);
        n00b_allocator_destroy(qalloc);
        return last;
    }
    n00b_query_cursor_t *cursor = n00b_result_get(cursor_r);
    n00b_query_cursor_set_streaming(cursor, true);

    n00b_arena_t *scratch = n00b_new_arena(.use_gc = false,
                                           .no_map = true,
                                           .hidden = false,
                                           .name   = "rocs_query_race_scratch");

    uint64_t delivered = 0;
    while (delivered < limit) {
        auto next_r = n00b_query_cursor_next(cursor);
        if (n00b_result_is_err(next_r)) {
            stats->query_errors++;
            break;
        }
        auto hit_opt = n00b_result_get(next_r);
        if (!n00b_option_is_set(hit_opt)) {
            break;
        }
        n00b_query_hit_t *hit = n00b_option_get(hit_opt);

        auto pos_r = n00b_query_hit_pos(hit);
        if (n00b_result_is_err(pos_r)) {
            stats->query_errors++;
            break;
        }
        n00b_store_pos_t pos = n00b_result_get(pos_r);

        if (have_before) {
            int cmp = n00b_store_pos_compare(pos, before);
            // Manual bound: skip everything the previous page covered.
            if (manual_before_bound && cmp >= 0) {
                continue;
            }
            // as_of bound: inclusive, so drop exactly the boundary row.
            if (!manual_before_bound && cmp == 0) {
                continue;
            }
        }

        // wax makes the scratch arena the CURRENT allocator around the record
        // fetch and its text, then resets the arena after every row. Anything
        // this path allocates from the current allocator and caches somewhere
        // longer-lived would dangle into reused scratch memory.
        n00b_allocator_t *prev = n00b_set_current_allocator(
            (n00b_allocator_t *)scratch);
        auto rec_r = n00b_query_hit_record(hit);
        if (n00b_result_is_err(rec_r)) {
            n00b_set_current_allocator(prev);
            stats->query_errors++;
            break;
        }
        auto text_r = n00b_store_record_view_json_string(n00b_result_get(rec_r));
        n00b_set_current_allocator(prev);
        if (n00b_result_is_err(text_r)) {
            stats->query_errors++;
            break;
        }
        n00b_string_t *text = n00b_result_get(text_r);
        CHECK(text != nullptr && text->u8_bytes > 0);
        CHECK(strstr(text->data, "wax:queryrace:") != nullptr);

        // wax's consumer parses every delivered row on the default heap
        // (collect() in tests/test_rocs_query_race.c). That garbage is what
        // drives collections while the writer runs; without it this test
        // never collects, and a race involving stop-the-world cannot fire.
        const char       *perr = nullptr;
        n00b_json_node_t *root = n00b_json_parse(text->data,
                                                 (int64_t)text->u8_bytes,
                                                 &perr);
        CHECK(root != nullptr && n00b_json_is_object(root));
        n00b_json_node_t *event_id = n00b_json_object_get(root, r"event_id");
        CHECK(event_id != nullptr && n00b_json_is_string(event_id));
        unsigned long long seq = 0;
        CHECK(sscanf(n00b_json_as_string(event_id)->data,
                     "wax:queryrace:%llu",
                     &seq)
              == 1);
        if (stats->page_rows < PAGE_ROWS) {
            stats->page_seqs[stats->page_rows++] = (uint64_t)seq;
        }

        last = pos;
        delivered++;
        stats->rows++;
        n00b_arena_reset(scratch);
    }

    n00b_allocator_destroy((n00b_allocator_t *)scratch);
    (void)n00b_query_cursor_close(cursor);
    (void)n00b_query_view_close(view);
    (void)n00b_store_pin_release(qpin);
    n00b_allocator_destroy(qalloc);
    stats->queries++;

    if (delivered < limit) {
        return (n00b_store_pos_t){};
    }
    return last;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    run_seconds = env_u64("N00B_QUERY_RACE_SECONDS", run_seconds);
    max_rows    = env_u64("N00B_QUERY_RACE_ROWS", max_rows);
    gc_pressure = env_u64("N00B_QUERY_RACE_GC_PRESSURE", gc_pressure);

    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);
    auto        be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));

    // wax opens everything below with user_pool (crayon_rocs_cache_open is
    // called with .allocator = &rt->user_pool), and gives the store a commit
    // topic on a conduit of its own.
    n00b_allocator_t *alloc = user_pool();

    auto conduit_r = n00b_conduit_new(.allocator = alloc);
    CHECK(n00b_result_is_ok(conduit_r));
    auto topic_r = n00b_store_commit_topic_get(n00b_result_get(conduit_r),
                                               N00B_CONDUIT_URI_USER_EVENT(547));
    CHECK(n00b_result_is_ok(topic_r));

    auto schema_r    = n00b_rocs_wax_schema_new(.allocator = alloc);
    auto partition_r = n00b_rocs_wax_partition_policy_new(.allocator = alloc);
    auto seal_r      = n00b_rocs_wax_seal_policy_new(.allocator = alloc);
    CHECK(n00b_result_is_ok(schema_r));
    CHECK(n00b_result_is_ok(partition_r));
    CHECK(n00b_result_is_ok(seal_r));

    // crayon-gw's profile (rocs_cache.c), minus the bounds it sizes from
    // its own config.
    auto profile_r = n00b_store_service_profile_new(
        .ingest_worker_count = 1,
        .seal_worker_count   = 1,
        .ingest_backpressure = N00B_STORE_INGEST_BACKPRESSURE_REJECT,
        .source_decoder      = n00b_rocs_wax_record_from_source,
        .allocator           = alloc);
    CHECK(n00b_result_is_ok(profile_r));

    auto store_r = n00b_store_open_service(vfs,
                                           r"/store",
                                           n00b_result_get(schema_r),
                                           n00b_result_get(profile_r),
                                           .partition_policy
                                           = n00b_result_get(partition_r),
                                           .seal_policy  = n00b_result_get(seal_r),
                                           .commit_topic = n00b_result_get(topic_r));
    CHECK(n00b_result_is_ok(store_r));
    store = n00b_result_get(store_r);

    printf("n00b#547: newest-first reader vs batch writer, %llu s\n",
           (unsigned long long)run_seconds);
    // Which collector this process runs matters: a moving collector rewrites
    // every pointer it can see, pin-all moves nothing.
    printf("  gc policy: %s\n", n00b_gc_pin_all_policy() ? "pin-all" : "copying");

    CHECK(ingest_batch(0, BASELINE));

    // The service store commits asynchronously; wait for the baseline to be
    // visible, as wax's test does before it anchors.
    for (int attempt = 0; attempt < 600; attempt++) {
        auto memory_r = n00b_store_memory_stats(store);
        if (n00b_result_is_ok(memory_r)
            && n00b_result_get(memory_r).hot_record_count >= BASELINE) {
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&ts, nullptr);
    }

    // Anchor at the newest baseline row. Every drain pages down from here,
    // and because it is a hot-shard position each page is unbounded at the
    // top: it walks from the newest row -- through everything the writer has
    // added since -- down to the anchor before it delivers anything.
    reader_stats_t   anchor_stats = {};
    n00b_store_pos_t anchor       = newest_page(&anchor_stats, (n00b_store_pos_t){}, false, 1);
    CHECK(anchor.shard_id != 0);
    CHECK(anchor_stats.page_rows == 1);
    CHECK(anchor_stats.page_seqs[0] == BASELINE - 1);

    n00b_atomic_store(&stop_flag, false);
    auto writer_r = n00b_thread_spawn(writer_main, nullptr);
    CHECK(n00b_result_is_ok(writer_r));
    n00b_thread_t *writer = n00b_result_get(writer_r);

    n00b_thread_t *pressure = nullptr;
    if (gc_pressure) {
        auto pressure_r = n00b_thread_spawn(pressure_main, nullptr);
        CHECK(n00b_result_is_ok(pressure_r));
        pressure = n00b_result_get(pressure_r);
    }

    reader_stats_t stats    = {};
    uint64_t       deadline = now_ns() + run_seconds * UINT64_C(1000000000);
    while (now_ns() < deadline) {
        // wax's oracle: one drain from the anchor returns every baseline row
        // below it exactly once, whatever the writer is doing above it.
        bool             seen[BASELINE] = {};
        uint64_t         seen_count     = 0;
        bool             exact          = true;
        n00b_store_pos_t before         = anchor;
        bool             drained        = false;
        for (int page = 0; page < MAX_PAGES; page++) {
            n00b_store_pos_t last = newest_page(&stats, before, true, PAGE_ROWS);
            for (uint64_t i = 0; i < stats.page_rows; i++) {
                uint64_t seq = stats.page_seqs[i];
                if (seq >= BASELINE - 1 || seen[seq]) {
                    exact = false;
                    continue;
                }
                seen[seq] = true;
                seen_count++;
            }
            if (last.shard_id == 0) {
                drained = true;
                break;
            }
            before = last;
        }
        if (drained && exact && seen_count == BASELINE - 1) {
            stats.complete_drains++;
        }
        else {
            stats.incomplete_drains++;
        }
    }

    n00b_atomic_store(&stop_flag, true);
    n00b_thread_join(writer);
    if (pressure != nullptr) {
        n00b_thread_join(pressure);
    }

    uint64_t wrote = n00b_atomic_load(&appended);
    printf("  writer: %llu rows appended, %llu batch error(s), worst batch %.1f ms\n",
           (unsigned long long)wrote,
           (unsigned long long)n00b_atomic_load(&batch_errors),
           (double)n00b_atomic_load(&batch_worst_ns) / 1e6);
    n00b_runtime_t *runtime = n00b_get_runtime();
    printf("  gc: %llu pause(s), worst %.1f ms\n",
           (unsigned long long)n00b_atomic_load(&runtime->gc_pause_count),
           (double)n00b_atomic_load(&runtime->gc_max_pause_ns) / 1e6);
    printf("  reader: %llu queries, %llu rows, %llu query error(s), "
           "%llu exact drains, %llu inexact\n",
           (unsigned long long)stats.queries,
           (unsigned long long)stats.rows,
           (unsigned long long)stats.query_errors,
           (unsigned long long)stats.complete_drains,
           (unsigned long long)stats.incomplete_drains);

    // Anti-vacuity: both sides must actually have run.
    CHECK(wrote > 0);
    CHECK(stats.rows > 0);
    CHECK(stats.complete_drains > 0);
    CHECK(stats.incomplete_drains == 0);
    CHECK(stats.query_errors == 0);

    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    printf("n00b#547: no crash.\n");
    n00b_shutdown();
    return 0;
}
