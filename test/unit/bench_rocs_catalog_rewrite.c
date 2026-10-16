/*
 * What a full rocs catalog rewrite actually costs, and where the cost is.
 *
 * n00b#517 attributes catalog-rewrite CPU to the per-byte append loop, and
 * crashappsec/wax#1229 shows that loop on top of a `sample` profile. Fixing
 * the appenders (n00b#518) removes a real share of it. This measures what is
 * left, because the shape of the remainder decides whether #518 closes
 * wax#1229 or only dents it.
 *
 * Three things get measured, all through the public store API on a store
 * built the way production builds one (wax schema, wax-shaped records, local
 * VFS, real seals):
 *
 *   1. Catalog composition. How many of the catalog's bytes go through the
 *      per-byte u64 path versus the bulk paths (strings, bloom filter bytes).
 *      This is what decides how much of a rewrite #517's loop could ever have
 *      been responsible for.
 *
 *   2. Cost of one rewrite, as a function of N. n00b_store_drop_sealed_shard
 *      is exactly one serialize + one whole-image VFS write + one view
 *      rebuild, so dropping shards one at a time times a rewrite directly and
 *      sweeps N downward as it goes.
 *
 *   3. Cost of a retention pass that drops K shards in one call, versus K
 *      single drops. n00b_store_apply_shard_retention loops drop-one-rewrite-
 *      all while holding commit_lock and residency_lock, so this is the
 *      figure that matters at the retention cap.
 *
 * Registered as a test at a small size, where what it checks is that the
 * drops happen and the catalog stays readable. Run the binary directly for
 * the measurement. ROCS_BENCH_SHARDS, ROCS_BENCH_RECORDS (per shard) and
 * ROCS_BENCH_DROPS size it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "rocs/filter.h"
#include "rocs/query.h"
#include "rocs/store.h"
#include "rocs/wax.h"
#include "util/assert.h"
#include "util/path.h"
#include "vfs/backend_local.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include "internal/rocs/store.h"
#include "rocs_test_support.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "catalog rewrite bench check failed: " #expr);    \
    } while (0)

#define CATALOG_PATH r"/rocs/catalog.rocs"

static uint64_t shards  = 119;
static uint64_t records = 20000;
static uint64_t drops   = 20;

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

static double
ms_since(uint64_t start)
{
    return (double)(now_ns() - start) / 1e6;
}

// Shard images go to a local directory, as in production.
static n00b_vfs_t *
new_local_vfs(n00b_string_t **dir)
{
    auto tmp_r = n00b_new_temp_dir(r"n00b_bench_catalog_rewrite_", nullptr);
    CHECK(n00b_result_is_ok(tmp_r));
    *dir = n00b_result_get(tmp_r);

    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);
    auto        be_r = n00b_vfs_backend_local_new(*dir);
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

// The memory backend does not implement sync, so rocs_store_sync_if_supported
// gets NOT_SUPPORTED and no-ops. Running the same workload on both backends
// splits a rewrite's cost into the CPU of building the image and the two
// fsyncs (file, then parent directory) that committing it costs.
static n00b_vfs_t *
new_memory_vfs(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    auto be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static n00b_store_t *
open_store(n00b_vfs_t *vfs)
{
    auto schema_r = n00b_rocs_wax_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    auto store_r = n00b_store_open_vfs(vfs,
                                       r"/rocs",
                                       n00b_result_get(schema_r));
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

static const char *kinds[12] = {
    "proc.spawn",
    "proc.exit",
    "file.modify",
    "file.open",
    "file.rename",
    "net.connect",
    "ai.session_start",
    "ai.api.request_metadata",
    "repo.snapshot",
    "chalker.observe",
    "host.heartbeat",
    "artifact_attestation.policy_decision",
};
static const char *classes[4]  = {"process", "file", "network", "ai"};
static const char *families[3] = {"ebpf", "endpoint_security", "gateway"};

static n00b_json_node_t *
str_node(const char *fmt, uint64_t v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, (unsigned long long)v);
    return n00b_json_string_new(buf);
}

// The same wax-shaped record bench_rocs_term_summary ingests, so the catalog
// this builds carries the same TERM summary and zone trailer production does.
static n00b_json_node_t *
record(uint64_t g)
{
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec,
                         "schema",
                         n00b_json_string_new("wax.normalized.v1"));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kinds[g % 12]));
    n00b_json_object_put(rec, "class", n00b_json_string_new(classes[g % 4]));
    n00b_json_object_put(rec, "event_id", str_node("wax:bench:%llu", g));
    n00b_json_object_put(rec, "ts_ns", n00b_json_int_new((int64_t)g));

    n00b_json_node_t *source = n00b_json_object_new();
    n00b_json_object_put(source,
                         "family",
                         n00b_json_string_new(families[g % 3]));
    n00b_json_object_put(source,
                         "name",
                         str_node("host-%03llu", (g * 7919) % 300));
    n00b_json_object_put(rec, "source", source);

    if (g % 2 == 1) {
        n00b_json_node_t *lineage = n00b_json_object_new();
        n00b_json_object_put(lineage,
                             "event_id",
                             str_node("wax:bench:%llu", g - 1));
        n00b_json_object_put(rec, "lineage", lineage);
    }

    n00b_json_node_t *body = n00b_json_object_new();
    n00b_json_object_put(body,
                         "pid",
                         n00b_json_int_new(1000 + (int64_t)((g * 31) % 2000)));
    n00b_json_object_put(rec, "body", body);
    return rec;
}

static n00b_buffer_t *
read_catalog(n00b_vfs_t *vfs)
{
    auto open_r = n00b_vfs_open(vfs, CATALOG_PATH, N00B_VFS_O_R);
    CHECK(n00b_result_is_ok(open_r));
    n00b_buffer_t *image = n00b_buffer_new(0);
    for (;;) {
        auto read_r = n00b_vfs_read(vfs,
                                    n00b_result_get(open_r),
                                    UINT64_C(1) << 24);
        CHECK(n00b_result_is_ok(read_r));
        if (n00b_buffer_len(n00b_result_get(read_r)) == 0) {
            break;
        }
        n00b_buffer_concat(image, n00b_result_get(read_r));
    }
    CHECK(n00b_result_is_ok(n00b_vfs_close(vfs, n00b_result_get(open_r))));
    return image;
}

static uint64_t
get_u64(n00b_buffer_t *image, int64_t at)
{
    uint8_t *b = (uint8_t *)image->data;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)b[at + i]) << (i * 8);
    }
    return v;
}

// Which bytes of the catalog image went through which appender. The u64 and
// u8 counts are the bytes #517's per-byte loop ever touched; the rest were
// already bulk-appended before #518 (strings via from_bytes + concat, filter
// bytes via append_bytes).
typedef struct {
    uint64_t total;
    uint64_t u64_bytes;    // per-byte loop, 8 bytes at a time
    uint64_t u8_bytes;     // per-byte loop, 1 byte at a time
    uint64_t string_bytes; // bulk: string payloads (length prefix counted above)
    uint64_t filter_bytes; // bulk: bloom filter bytes
    uint64_t entries;
} composition_t;

static composition_t
walk_catalog(n00b_buffer_t *image)
{
    composition_t c = {};
    c.total         = (uint64_t)n00b_buffer_len(image);

    // Header (v7): 8 magic bytes (per-byte u8 path) then 10 u64s -- version
    // at byte 8, the append-only log's epoch and id at bytes 64 and 72, and
    // the entry count at byte 80. Entries start at byte 88.
    c.u8_bytes += 8;
    CHECK(get_u64(image, 8) == 7);
    c.u64_bytes += 10 * 8;
    c.entries = get_u64(image, 80);

    int64_t at = 88;
    for (uint64_t e = 0; e < c.entries; e++) {
        at += 7 * 8; // state, shard_id, generation, byte_len, records, schema, seal_ts
        c.u64_bytes += 7 * 8;

        for (int i = 0; i < 3; i++) { // object_path, partition_key, etag
            uint64_t len = get_u64(image, at);
            c.u64_bytes += 8; // the length prefix is a u64
            c.string_bytes += len;
            at += 8 + (int64_t)len;
        }

        uint64_t nsum = get_u64(image, at);
        at += 8;
        c.u64_bytes += 8;
        for (uint64_t s = 0; s < nsum; s++) {
            uint64_t len = get_u64(image, at);
            c.u64_bytes += 8;
            c.string_bytes += len;
            at += 8 + (int64_t)len;

            uint64_t nkeys = get_u64(image, at);
            at += 8;
            c.u64_bytes += 8;
            if (nkeys > 0) {
                // k, nbits, then the length-prefixed filter bytes.
                uint64_t nbytes = get_u64(image, at + 16);
                c.u64_bytes += 3 * 8;
                c.filter_bytes += nbytes;
                at += 24 + (int64_t)nbytes;
            }
        }

        uint64_t nzones = get_u64(image, at);
        at += 8;
        c.u64_bytes += 8;
        for (uint64_t z = 0; z < nzones; z++) {
            uint64_t len = get_u64(image, at);
            c.u64_bytes += 8;
            c.string_bytes += len;
            at += 8 + (int64_t)len;
            for (int bound = 0; bound < 2; bound++) {
                uint8_t kind = (uint8_t)image->data[at++];
                c.u8_bytes += 1;
                if (kind == 3) {
                    uint64_t slen = get_u64(image, at);
                    c.u64_bytes += 8;
                    c.string_bytes += slen;
                    at += 8 + (int64_t)slen;
                }
                else {
                    c.u64_bytes += 8;
                    at += 8;
                }
            }
        }
    }
    CHECK(at == (int64_t)n00b_buffer_len(image));
    return c;
}

static uint64_t
entry_count(n00b_store_t *store)
{
    auto count_r = n00b_store_catalog_get_entry_count(store);
    CHECK(n00b_result_is_ok(count_r));
    return n00b_result_get(count_r);
}

// The shard ids still in the catalog, oldest first.
static void
visible_shard_ids(n00b_store_t *store, uint64_t *out, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
        CHECK(n00b_result_is_ok(entry_r));
        CHECK(n00b_option_is_set(n00b_result_get(entry_r)));
        auto id_r = n00b_store_catalog_entry_get_shard_id(
            n00b_option_get(n00b_result_get(entry_r)));
        CHECK(n00b_result_is_ok(id_r));
        out[i] = n00b_result_get(id_r);
    }
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    shards  = env_u64("ROCS_BENCH_SHARDS", shards);
    records = env_u64("ROCS_BENCH_RECORDS", records);
    drops   = env_u64("ROCS_BENCH_DROPS", drops);
    if (drops > shards / 2) {
        drops = shards / 2;
    }

    bool           memory = env_u64("ROCS_BENCH_MEMORY_VFS", 0) != 0;
    n00b_string_t *dir    = nullptr;
    n00b_vfs_t    *vfs    = memory ? new_memory_vfs() : new_local_vfs(&dir);
    n00b_store_t  *store  = open_store(vfs);

    printf("backend: %s\n", memory ? "memory (no fsync)" : "local (fsync)");
    printf("building %llu shards x %llu records\n",
           (unsigned long long)shards,
           (unsigned long long)records);

    double ingest_ms = 0;
    double seal_ms   = 0;
    for (uint64_t s = 0; s < shards; s++) {
        uint64_t start = now_ns();
        for (uint64_t i = 0; i < records; i++) {
            CHECK(n00b_result_is_ok(
                n00b_store_ingest(store, record(s * records + i))));
        }
        ingest_ms += ms_since(start);
        start = now_ns();
        CHECK(n00b_result_is_ok(
            n00b_store_seal_hot_shard(store, .seal_ts = (s + 1) * 1000)));
        seal_ms += ms_since(start);
    }

    printf("  ingest %.0f ms, seal %.0f ms (%.1f ms/shard)\n",
           ingest_ms,
           seal_ms,
           seal_ms / (double)shards);

    uint64_t n = entry_count(store);
    CHECK(n == shards);

    composition_t c = walk_catalog(read_catalog(vfs));
    printf("\ncatalog: %llu bytes over %llu entries (%llu B/entry)\n",
           (unsigned long long)c.total,
           (unsigned long long)c.entries,
           (unsigned long long)(c.total / c.entries));
    printf("  per-byte appenders (what #517 is about):\n");
    printf("    u64 fields      %10llu B  %5.2f%%\n",
           (unsigned long long)c.u64_bytes,
           100.0 * (double)c.u64_bytes / (double)c.total);
    printf("    u8  fields      %10llu B  %5.2f%%\n",
           (unsigned long long)c.u8_bytes,
           100.0 * (double)c.u8_bytes / (double)c.total);
    printf("  already bulk before #518:\n");
    printf("    string payloads %10llu B  %5.2f%%\n",
           (unsigned long long)c.string_bytes,
           100.0 * (double)c.string_bytes / (double)c.total);
    printf("    filter bytes    %10llu B  %5.2f%%\n",
           (unsigned long long)c.filter_bytes,
           100.0 * (double)c.filter_bytes / (double)c.total);

    // One drop == one full serialize + one whole-image VFS write + one view
    // rebuild. Timing them one at a time sweeps N downward as it goes.
    uint64_t *ids = n00b_alloc_array(uint64_t, n);
    visible_shard_ids(store, ids, n);

    printf("\none rewrite, by catalog size (single drops):\n");
    printf("  %5s  %10s  %12s\n", "N", "rewrite ms", "catalog B");
    for (uint64_t i = 0; i < drops; i++) {
        uint64_t before = entry_count(store);
        uint64_t start  = now_ns();
        auto     drop_r = n00b_store_drop_sealed_shard(store, ids[i]);
        double   ms     = ms_since(start);
        CHECK(n00b_result_is_ok(drop_r));
        CHECK(entry_count(store) == before - 1);
        if (i % 4 == 0 || i == drops - 1) {
            printf("  %5llu  %10.1f  %12llu\n",
                   (unsigned long long)before,
                   ms,
                   (unsigned long long)n00b_buffer_len(read_catalog(vfs)));
        }
    }

    // What the retention cap actually runs: one call that drops K shards,
    // rewriting the whole catalog once per shard, under commit_lock and
    // residency_lock the whole time.
    uint64_t remaining = entry_count(store);
    uint64_t target    = remaining > drops ? remaining - drops : 1;
    auto     policy_r  = n00b_store_shard_retention_policy_new(
        .max_sealed_shards = target,
        .drop_reason       = r"bench-retention");
    CHECK(n00b_result_is_ok(policy_r));

    uint64_t start    = now_ns();
    auto     apply_r  = n00b_store_apply_shard_retention(
        store,
        n00b_result_get(policy_r));
    double   apply_ms = ms_since(start);
    CHECK(n00b_result_is_ok(apply_r));
    uint64_t dropped = n00b_result_get(apply_r);

    printf("\none retention pass at N=%llu dropping %llu shards:\n",
           (unsigned long long)remaining,
           (unsigned long long)dropped);
    printf("  %.0f ms total, %.1f ms per shard dropped\n",
           apply_ms,
           dropped == 0 ? 0.0 : apply_ms / (double)dropped);
    printf("  (commit_lock and residency_lock are held for all of it)\n");

    CHECK(n00b_result_is_ok(n00b_store_close(store)));

    // n00b#527 class: `rt` lives in this frame, and n00b_init started worker
    // threads that read it. Returning without quiescing them let a worker read
    // the runtime out of a dead frame during process teardown -- caught on
    // Windows x64 as an access violation on a worker at roughly 1 run in 100.
    n00b_shutdown();
    return 0;
}
