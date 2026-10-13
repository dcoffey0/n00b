/*
 * What a retention pass costs when its candidates are PINNED.
 *
 * crashappsec/wax#1229's gateway is at its retention cap with queries in
 * flight, so retention and live readers overlap constantly. n00b#517 and the
 * batching that followed it addressed the write side: a pass now serializes
 * and fsyncs the catalog once rather than once per dropped shard. Neither
 * touches the SELECTION side, which is what runs when a candidate cannot be
 * dropped.
 *
 * `n00b_store_apply_shard_retention` loops calling
 * `rocs_store_oldest_retention_candidate`, and a pinned candidate is pushed
 * onto a `blocked` list and skipped -- without dropping anything, so the loop
 * goes around again. Per iteration that helper does:
 *
 *   - a full O(N) pass to sum sealed bytes and count entries, then
 *   - a full O(N) pass to pick the oldest, and inside it, for every entry,
 *     `rocs_store_shard_id_list_contains(blocked, ...)` -- a LINEAR scan.
 *
 * So iteration k costs O(N x k), and P pinned candidates cost O(N x P^2).
 * With every shard pinned (P = N) that is O(N^3), all of it under
 * commit_lock AND residency_lock, which is exactly the pair that stalls
 * ingest and every reader.
 *
 * This measures that directly. Pinning EVERY shard makes the pass drop
 * nothing and return N00B_STORE_ERR_PINNED, so the elapsed time is pure
 * selection overhead: no serialize, no write, no fsync, no unlink. Sweeping N
 * and fitting the growth exponent between adjacent points says whether the
 * shape is really cubic rather than merely slow.
 *
 * Registered as a test at a small size, where what it checks is that a
 * fully-pinned pass drops nothing and reports PINNED. Run the binary directly
 * for the sweep. ROCS_BENCH_SIZES (comma-separated shard counts) and
 * ROCS_BENCH_RECORDS size it.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "rocs/store.h"
#include "rocs/wax.h"
#include "util/assert.h"
#include "util/path.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include "internal/rocs/store.h"
#include "rocs_test_support.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "pinned retention bench check failed: " #expr);   \
    } while (0)

#define MAX_SIZES 16

static uint64_t records = 2;

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

// The memory backend: this bench measures SELECTION, and a fully-pinned pass
// never reaches the write path at all, so there is nothing for a real VFS to
// contribute except shard-image I/O while building.
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
    auto store_r = n00b_store_open_vfs(vfs, r"/rocs", n00b_result_get(schema_r));
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

// Minimal wax-shaped record. The selection scan walks catalog ENTRIES and
// never looks at record contents, so the records only have to exist.
static n00b_json_node_t *
record(uint64_t g)
{
    char              buf[64];
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec,
                         "schema",
                         n00b_json_string_new("wax.normalized.v1"));
    n00b_json_object_put(rec, "kind", n00b_json_string_new("host.heartbeat"));
    snprintf(buf, sizeof(buf), "wax:pin:%llu", (unsigned long long)g);
    n00b_json_object_put(rec, "event_id", n00b_json_string_new(buf));
    n00b_json_object_put(rec, "ts_ns", n00b_json_int_new((int64_t)g));
    return rec;
}

static uint64_t
entry_count(n00b_store_t *store)
{
    auto count_r = n00b_store_catalog_get_entry_count(store);
    CHECK(n00b_result_is_ok(count_r));
    return n00b_result_get(count_r);
}

typedef struct {
    uint64_t shards;
    double   build_ms;
    double   pinned_ms;   // one pass, every shard pinned: pure selection
    double   unpinned_ms; // same pass with the pin released, for scale
    uint64_t dropped;
} point_t;

static point_t
run(uint64_t shards)
{
    point_t       p     = {.shards = shards};
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);

    uint64_t start = now_ns();
    for (uint64_t s = 0; s < shards; s++) {
        for (uint64_t i = 0; i < records; i++) {
            CHECK(n00b_result_is_ok(
                n00b_store_ingest(store, record(s * records + i))));
        }
        CHECK(n00b_result_is_ok(
            n00b_store_seal_hot_shard(store, .seal_ts = (s + 1) * 1000)));
    }
    p.build_ms = ms_since(start);
    CHECK(entry_count(store) == shards);

    // Pin EVERY sealed shard, the way live queries over the whole window do.
    auto pin_r = n00b_store_pin_acquire(store);
    CHECK(n00b_result_is_ok(pin_r));
    n00b_store_pin_t *pin = n00b_result_get(pin_r);

    // rocs_store_shard_id_list_new is static to store.c; the list is just a
    // n00b_list_t(uint64_t), and narrow_to_shards copies it.
    n00b_store_shard_id_list_t *ids = n00b_alloc(n00b_store_shard_id_list_t);
    CHECK(ids != nullptr);
    *ids = n00b_list_new_private(uint64_t);
    for (uint64_t i = 0; i < shards; i++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
        CHECK(n00b_result_is_ok(entry_r));
        CHECK(n00b_option_is_set(n00b_result_get(entry_r)));
        auto id_r = n00b_store_catalog_entry_get_shard_id(
            n00b_option_get(n00b_result_get(entry_r)));
        CHECK(n00b_result_is_ok(id_r));
        n00b_list_push(*ids, n00b_result_get(id_r));
    }
    CHECK(n00b_result_is_ok(n00b_store_pin_narrow_to_shards(pin, ids)));

    // Drop everything we can -- which, with every shard pinned, is nothing.
    // The pass therefore does selection and only selection.
    auto policy_r = n00b_store_shard_retention_policy_new(
        .max_sealed_shards = 1,
        .drop_reason       = r"bench-pinned");
    CHECK(n00b_result_is_ok(policy_r));

    start    = now_ns();
    auto r1  = n00b_store_apply_shard_retention(store,
                                                n00b_result_get(policy_r));
    p.pinned_ms = ms_since(start);

    // Every candidate was pinned, so the pass must report PINNED and the
    // catalog must be untouched. If this ever stops holding, the number above
    // is measuring something else.
    CHECK(n00b_result_is_err(r1));
    CHECK(n00b_result_get_err(r1) == N00B_STORE_ERR_PINNED);
    CHECK(entry_count(store) == shards);

    // Same pass with nothing pinned, as the scale against which the pinned
    // number should be read.
    CHECK(n00b_result_is_ok(n00b_store_pin_release(pin)));
    start   = now_ns();
    auto r2 = n00b_store_apply_shard_retention(store,
                                               n00b_result_get(policy_r));
    p.unpinned_ms = ms_since(start);
    CHECK(n00b_result_is_ok(r2));
    p.dropped = n00b_result_get(r2);

    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    return p;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    records = env_u64("ROCS_BENCH_RECORDS", records);

    uint64_t    sizes[MAX_SIZES];
    size_t      nsizes = 0;
    const char *spec   = getenv("ROCS_BENCH_SIZES");
    if (spec == nullptr || *spec == '\0') {
        spec = "50,100,200,400";
    }
    const char *q = spec;
    while (*q && nsizes < MAX_SIZES) {
        sizes[nsizes++] = (uint64_t)strtoull(q, (char **)&q, 10);
        while (*q == ',' || *q == ' ') {
            q++;
        }
    }

    printf("retention selection cost with every shard pinned\n");
    printf("(the pass drops nothing and returns PINNED, so this is pure\n");
    printf(" candidate-scan time -- no serialize, no write, no fsync)\n\n");
    printf("  %6s  %10s  %12s  %12s  %8s\n",
           "shards",
           "build ms",
           "PINNED ms",
           "unpinned ms",
           "ratio");

    point_t pts[MAX_SIZES];
    for (size_t i = 0; i < nsizes; i++) {
        pts[i] = run(sizes[i]);
        printf("  %6llu  %10.0f  %12.1f  %12.1f  %7.0fx\n",
               (unsigned long long)pts[i].shards,
               pts[i].build_ms,
               pts[i].pinned_ms,
               pts[i].unpinned_ms,
               pts[i].unpinned_ms > 0.0
                   ? pts[i].pinned_ms / pts[i].unpinned_ms
                   : 0.0);
        fflush(stdout);
    }

    // Growth exponent between adjacent sizes: log(t2/t1) / log(n2/n1). ~1 is
    // linear, ~2 quadratic, ~3 cubic. Reported per step rather than fitted
    // once, so a shape that only turns over at the top is visible as such.
    if (nsizes > 1) {
        printf("\n  growth exponent between adjacent sizes"
               " (1=linear, 2=quadratic, 3=cubic):\n");
        for (size_t i = 1; i < nsizes; i++) {
            double n1 = (double)pts[i - 1].shards;
            double n2 = (double)pts[i].shards;
            double t1 = pts[i - 1].pinned_ms;
            double t2 = pts[i].pinned_ms;
            if (n2 > n1 && t1 > 0.0 && t2 > 0.0) {
                printf("    %4llu -> %-4llu   %.2f\n",
                       (unsigned long long)pts[i - 1].shards,
                       (unsigned long long)pts[i].shards,
                       log(t2 / t1) / log(n2 / n1));
            }
        }
    }

    // Quiesce the runtime's worker threads before `rt` goes out of scope with
    // this frame (n00b#527; same defect as bench_rocs_catalog_rewrite).
    n00b_shutdown();
    return 0;
}
