/*
 * What the per-shard TERM summary filters cost and what they skip, on a store
 * with the wax schema.
 *
 * Every shard holds records shaped like wax's: one schema value, a dozen
 * kinds, a few classes and source families, a few hundred source names, a
 * couple of thousand pids, a unique event_id per record, and a lineage
 * event_id on every other record. Each byte cap is measured through the same
 * path production takes: ingest, seal, close, reopen, and n00b_query_run.
 * Per cap it reports seal time, open time, catalog bytes (total, per entry,
 * and per field), the false positive rate of absent event_id lookups against
 * each shard's filter, and the shards an absent or present lookup maps. The
 * exact key lists of catalog v4 are costed from the key counts the catalog
 * records, both capped at 256 keys and uncapped.
 *
 * Registered as a test at a small size, where what it checks is that no
 * present value is missed and that the maps a query takes are exactly the
 * shards its filters pass. Run the binary directly for the full measurement.
 * ROCS_BENCH_SHARDS, ROCS_BENCH_RECORDS (per shard), ROCS_BENCH_ABSENT,
 * ROCS_BENCH_QUERIES, and ROCS_BENCH_OPENS size it, and ROCS_BENCH_CAPS picks
 * the byte caps (comma separated). At the default size one cap peaks near
 * 1.5 GB resident, so give each cap its own process:
 *
 *   for cap in 0 4096 16384 32768; do
 *       ROCS_BENCH_CAPS=$cap ./build/bench_rocs_term_summary
 *   done
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
#include "vfs/vfs.h"

#include "internal/rocs/store.h"
#include "rocs_test_support.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "term summary bench check failed: " #expr);      \
    } while (0)

#define CATALOG_PATH r"/rocs/catalog.rocs"
#define BASE_MAX_KEYS 256

static uint64_t shards  = 8;
static uint64_t records = 20000;
static uint64_t absent  = 2000;
static uint64_t queries = 20;
static uint64_t opens   = 50;

static uint64_t
env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return (uint64_t)strtoull(v, nullptr, 10);
}

// Shard images go to a local directory, as in production. A memory VFS would
// hold every image for the whole run.
static n00b_vfs_t *
new_local_vfs(n00b_string_t **dir)
{
    auto tmp_r = n00b_new_temp_dir(r"n00b_bench_term_summary_", nullptr);
    CHECK(n00b_result_is_ok(tmp_r));
    *dir = n00b_result_get(tmp_r);

    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);
    auto be_r = n00b_vfs_backend_local_new(*dir);
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static n00b_store_t *
open_store(n00b_vfs_t *vfs, uint64_t max_bytes)
{
    auto schema_r = n00b_rocs_wax_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    auto store_r = n00b_store_open_vfs(vfs,
                                       r"/rocs",
                                       n00b_result_get(schema_r),
                                       .term_summary_max_bytes = max_bytes);
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

static void
close_store(n00b_store_t *store)
{
    CHECK(n00b_result_is_ok(n00b_store_close(store)));
}

static const char *kinds[12] = {
    "proc.spawn",    "proc.exit",       "file.modify",     "file.open",
    "file.rename",   "net.connect",     "ai.session_start", "ai.api.request_metadata",
    "repo.snapshot", "chalker.observe", "host.heartbeat",  "artifact_attestation.policy_decision",
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

// Record g of the whole store; shard s holds g in [s * records, (s+1) * records).
static n00b_json_node_t *
record(uint64_t g)
{
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec, "schema", n00b_json_string_new("wax.normalized.v1"));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kinds[g % 12]));
    n00b_json_object_put(rec, "class", n00b_json_string_new(classes[g % 4]));
    n00b_json_object_put(rec, "event_id", str_node("wax:bench:%llu", g));
    n00b_json_object_put(rec, "ts_ns", n00b_json_int_new((int64_t)g));

    n00b_json_node_t *source = n00b_json_object_new();
    n00b_json_object_put(source, "family", n00b_json_string_new(families[g % 3]));
    n00b_json_object_put(source, "name", str_node("host-%03llu", (g * 7919) % 300));
    n00b_json_object_put(rec, "source", source);

    if (g % 2 == 1) {
        n00b_json_node_t *lineage = n00b_json_object_new();
        n00b_json_object_put(lineage, "event_id", str_node("wax:bench:%llu", g - 1));
        n00b_json_object_put(rec, "lineage", lineage);
    }

    n00b_json_node_t *body = n00b_json_object_new();
    n00b_json_object_put(body, "pid", n00b_json_int_new(1000 + (int64_t)((g * 31) % 2000)));
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
        auto read_r = n00b_vfs_read(vfs, n00b_result_get(open_r), UINT64_C(1) << 24);
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

// Catalog bytes per summarized field, summed over entries, from the v5
// layout: per entry seven u64s, three length-prefixed strings, a field count,
// then per field its name and key count, and for a nonzero count k, bit
// count, and filter bytes.
#define MAX_NAMES 128

typedef struct {
    char     name[48];
    uint64_t bytes;     // v5 bytes, summed over entries
    uint64_t v4_bytes;  // what uncapped v4 lists would take
    uint64_t base_bytes; // what v4 lists capped at 256 keys would take
    uint64_t entries;   // entries summarizing the field
    uint64_t max_keys;
} field_cost_t;

typedef struct {
    uint64_t     catalog_bytes;
    uint64_t     entries;
    uint64_t     trailer_bytes;
    uint64_t     v4_trailer_bytes;
    uint64_t     base_trailer_bytes;
    uint64_t     nnames;
    field_cost_t f[MAX_NAMES];
} catalog_cost_t;

static field_cost_t *
cost_slot(catalog_cost_t *c, const char *name, size_t len)
{
    for (uint64_t i = 0; i < c->nnames; i++) {
        if (strlen(c->f[i].name) == len && memcmp(c->f[i].name, name, len) == 0) {
            return &c->f[i];
        }
    }
    CHECK(c->nnames < MAX_NAMES && len < sizeof(c->f[0].name));
    field_cost_t *f = &c->f[c->nnames++];
    memcpy(f->name, name, len);
    f->name[len] = '\0';
    return f;
}

static catalog_cost_t
walk_catalog(n00b_buffer_t *image)
{
    catalog_cost_t c = {};
    c.catalog_bytes  = (uint64_t)n00b_buffer_len(image);
    // v7 header: version at byte 8, log epoch and id at 64 and 72, entry
    // count at 80, entries from 88 (same layout test_rocs_term_summary walks).
    CHECK(get_u64(image, 8) == 7);
    c.entries  = get_u64(image, 80);
    int64_t at = 88;
    for (uint64_t e = 0; e < c.entries; e++) {
        at += 7 * 8;
        for (int i = 0; i < 3; i++) {
            at += 8 + (int64_t)get_u64(image, at);
        }
        int64_t  trailer_at = at;
        uint64_t nsum       = get_u64(image, at);
        at += 8;
        c.v4_trailer_bytes += 8;
        c.base_trailer_bytes += 8;
        for (uint64_t s = 0; s < nsum; s++) {
            int64_t  field_at = at;
            uint64_t len      = get_u64(image, at);
            field_cost_t *f   = cost_slot(&c, image->data + at + 8, (size_t)len);
            at += 8 + (int64_t)len;
            uint64_t nkeys = get_u64(image, at);
            at += 8;
            if (nkeys > 0) {
                at += 24 + (int64_t)get_u64(image, at + 16);
            }
            uint64_t v4 = 8 + len + 8 + 16 * nkeys;
            f->bytes += (uint64_t)(at - field_at);
            f->v4_bytes += v4;
            f->base_bytes += nkeys <= BASE_MAX_KEYS ? v4 : 0;
            f->entries++;
            if (nkeys > f->max_keys) {
                f->max_keys = nkeys;
            }
            c.v4_trailer_bytes += v4;
            c.base_trailer_bytes += nkeys <= BASE_MAX_KEYS ? v4 : 0;
        }
        c.trailer_bytes += (uint64_t)(at - trailer_at);

        // Skip the zone-map section (catalog v6) that follows the TERM
        // summary: a count, then per zone a field name and, for each of the
        // two bounds, a kind byte and a value -- eight bytes unless the kind
        // is STRING (3), which is length-prefixed. Counted after
        // trailer_bytes on purpose: this bench measures the TERM summary, and
        // folding zone bytes into that number would quietly change what it
        // reports.
        uint64_t nzones = get_u64(image, at);
        at += 8;
        for (uint64_t z = 0; z < nzones; z++) {
            at += 8 + (int64_t)get_u64(image, at);
            for (int bound = 0; bound < 2; bound++) {
                uint8_t kind = (uint8_t)image->data[at++];
                at = kind == 3 ? at + 8 + (int64_t)get_u64(image, at) : at + 8;
            }
        }
    }
    CHECK(at == (int64_t)n00b_buffer_len(image));
    return c;
}

// Whether this shard's summary lets field == value through, asked with the
// keys the planner resolves.
static bool
passes(n00b_store_catalog_entry_t *entry, n00b_string_t *field, const char *value)
{
    auto keys_r = n00b_store_index_keys_new(index_of(field, N00B_STORE_INDEX_TERM),
                                            n00b_json_string_new(value));
    CHECK(n00b_result_is_ok(keys_r));
    n00b_store_index_keys_t *keys = n00b_result_get(keys_r);
    uint64_t                 n    = n00b_store_index_keys_count(keys);
    CHECK(n > 0 && n <= 8);
    n00b_uint128_t arr[8];
    for (uint64_t i = 0; i < n; i++) {
        arr[i] = n00b_store_index_keys_at(keys, i);
    }
    return n00b_store_catalog_entry_may_contain_term(entry, field, arr, (size_t)n);
}

static uint64_t
passing_shards(n00b_store_t *store, n00b_string_t *field, const char *value)
{
    uint64_t n = 0;
    for (uint64_t s = 0; s < shards; s++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, s);
        CHECK(n00b_result_is_ok(entry_r));
        CHECK(n00b_option_is_set(n00b_result_get(entry_r)));
        n += passes(n00b_option_get(n00b_result_get(entry_r)), field, value);
    }
    return n;
}

typedef struct {
    uint64_t maps;
    uint64_t found;
} query_cost_t;

// One lookup on a freshly opened store, so every shard it touches is mapped
// on demand and counted as a residency miss.
static query_cost_t
query(n00b_vfs_t *vfs, uint64_t max_bytes, n00b_string_t *field, const char *value)
{
    n00b_store_t *store  = open_store(vfs, max_bytes);
    auto          before = n00b_store_residency_stats(store);
    CHECK(n00b_result_is_ok(before));
    uint64_t predicted = passing_shards(store, field, value);

    auto field_r = n00b_filter_field(field);
    CHECK(n00b_result_is_ok(field_r));
    auto filter_r = n00b_filter_eq(n00b_result_get(field_r),
                                   n00b_fv_utf8(n00b_string_from_cstr(value)));
    CHECK(n00b_result_is_ok(filter_r));
    auto query_r = n00b_query_new(n00b_result_get(filter_r), .limit = 1000);
    CHECK(n00b_result_is_ok(query_r));
    auto result_r = n00b_query_run(store, n00b_result_get(query_r));
    CHECK(n00b_result_is_ok(result_r));
    query_cost_t cost = {.found = n00b_query_count(n00b_result_get(result_r))};
    (void)n00b_query_result_close(n00b_result_get(result_r));

    auto after = n00b_store_residency_stats(store);
    CHECK(n00b_result_is_ok(after));
    cost.maps = n00b_result_get(after).cache_misses - n00b_result_get(before).cache_misses;
    close_store(store);
    // The query maps exactly the shards the catalog check lets through.
    CHECK(cost.maps == predicted);
    return cost;
}

typedef struct {
    uint64_t       max_bytes;
    double         ingest_ms;
    double         seal_ms;
    double         open_ms;
    catalog_cost_t catalog;
    double         event_fp;
    double         lineage_fp;
    double         absent_event_maps;
    double         present_event_maps;
    uint64_t       absent_host_maps;
    uint64_t       absent_kind_maps;
} run_t;

static double
ms_since(uint64_t start)
{
    return (double)(now_ns() - start) / 1e6;
}

static double
fp_rate(n00b_store_t *store, n00b_string_t *field)
{
    uint64_t pass = 0;
    char     value[64];
    for (uint64_t i = 0; i < absent; i++) {
        snprintf(value, sizeof(value), "wax:absent:%llu", (unsigned long long)i);
        pass += passing_shards(store, field, value);
    }
    return (double)pass / (double)(absent * shards);
}

static run_t
run(uint64_t max_bytes)
{
    run_t          r   = {.max_bytes = max_bytes};
    n00b_string_t *dir = nullptr;
    n00b_vfs_t    *vfs = new_local_vfs(&dir);

    n00b_store_t *store = open_store(vfs, max_bytes);
    for (uint64_t s = 0; s < shards; s++) {
        uint64_t start = now_ns();
        for (uint64_t i = 0; i < records; i++) {
            CHECK(n00b_result_is_ok(n00b_store_ingest(store, record(s * records + i))));
        }
        r.ingest_ms += ms_since(start);
        start = now_ns();
        CHECK(n00b_result_is_ok(n00b_store_seal_hot_shard(store, .seal_ts = (s + 1) * 1000)));
        r.seal_ms += ms_since(start);
    }
    close_store(store);

    r.catalog = walk_catalog(read_catalog(vfs));
    CHECK(r.catalog.entries == shards);

    uint64_t start = now_ns();
    for (uint64_t q = 0; q < opens; q++) {
        close_store(open_store(vfs, max_bytes));
    }
    r.open_ms = ms_since(start) / (double)opens;

    store         = open_store(vfs, max_bytes);
    r.event_fp    = fp_rate(store, r"event_id");
    r.lineage_fp  = fp_rate(store, r"lineage.event_id");
    // No present value is ruled out of the shard that holds it.
    char value[64];
    for (uint64_t s = 0; s < shards; s++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, s);
        CHECK(n00b_result_is_ok(entry_r));
        n00b_store_catalog_entry_t *entry = n00b_option_get(n00b_result_get(entry_r));
        for (uint64_t i = 0; i < records; i += 97) {
            snprintf(value, sizeof(value), "wax:bench:%llu", (unsigned long long)(s * records + i));
            CHECK(passes(entry, r"event_id", value));
        }
    }
    close_store(store);

    for (uint64_t q = 0; q < queries; q++) {
        snprintf(value, sizeof(value), "wax:absent:%llu", (unsigned long long)q);
        query_cost_t a = query(vfs, max_bytes, r"event_id", value);
        CHECK(a.found == 0);
        r.absent_event_maps += (double)a.maps;

        uint64_t g = (q * 7919 + 13) % (shards * records);
        snprintf(value, sizeof(value), "wax:bench:%llu", (unsigned long long)g);
        query_cost_t p = query(vfs, max_bytes, r"event_id", value);
        CHECK(p.found == 1);
        CHECK(p.maps >= 1);
        r.present_event_maps += (double)p.maps;
    }
    r.absent_event_maps /= (double)queries;
    r.present_event_maps /= (double)queries;
    r.absent_host_maps = query(vfs, max_bytes, r"source.name", "host-999").maps;
    r.absent_kind_maps = query(vfs, max_bytes, r"kind", "ai.exec").maps;
    if (max_bytes != 0) {
        CHECK(r.absent_kind_maps == 0);
    }
    CHECK(n00b_result_is_ok(n00b_path_remove_tree(dir, .ignore_missing = true)));
    return r;
}

static void
print_fields(catalog_cost_t *c)
{
    uint64_t empty       = 0;
    uint64_t empty_bytes = 0;
    printf("    %-22s %9s %10s %10s %10s\n", "field", "max keys", "filter B", "v4 B", "base B");
    for (uint64_t i = 0; i < c->nnames; i++) {
        field_cost_t *f = &c->f[i];
        if (f->max_keys == 0) {
            empty++;
            empty_bytes += f->bytes;
            continue;
        }
        printf("    %-22s %9llu %10.0f %10.0f %10.0f\n",
               f->name,
               (unsigned long long)f->max_keys,
               (double)f->bytes / (double)c->entries,
               (double)f->v4_bytes / (double)c->entries,
               (double)f->base_bytes / (double)c->entries);
    }
    printf("    %llu declared empty fields: %.0f B per entry\n",
           (unsigned long long)empty,
           (double)empty_bytes / (double)c->entries);
}

#define MAX_CAPS 8

static size_t
parse_caps(uint64_t caps[MAX_CAPS])
{
    const char *v = getenv("ROCS_BENCH_CAPS");
    if (v == nullptr || *v == '\0') {
        caps[0] = 0;
        caps[1] = N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT;
        caps[2] = 32768;
        return 3;
    }
    size_t n = 0;
    while (*v != '\0' && n < MAX_CAPS) {
        char *end = nullptr;
        caps[n++] = (uint64_t)strtoull(v, &end, 10);
        CHECK(end != v);
        v = *end == ',' ? end + 1 : end;
    }
    return n;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    shards  = env_u64("ROCS_BENCH_SHARDS", shards);
    records = env_u64("ROCS_BENCH_RECORDS", records);
    absent  = env_u64("ROCS_BENCH_ABSENT", absent);
    queries = env_u64("ROCS_BENCH_QUERIES", queries);
    opens   = env_u64("ROCS_BENCH_OPENS", opens);
    CHECK(shards > 0 && records > 0 && queries > 0 && absent > 0 && opens > 0);

    printf("bench_rocs_term_summary: %llu shards x %llu records, wax schema\n",
           (unsigned long long)shards,
           (unsigned long long)records);

    uint64_t caps[MAX_CAPS];
    size_t   ncaps = parse_caps(caps);
    run_t    runs[MAX_CAPS];
    size_t   widest = 0;
    for (size_t i = 0; i < ncaps; i++) {
        runs[i] = run(caps[i]);
        if (caps[i] > caps[widest]) {
            widest = i;
        }
    }

    printf("\n  %-10s %9s %9s %8s %12s %11s %9s %9s %8s %8s %8s %8s\n",
           "cap B",
           "seal ms",
           "open ms",
           "cat KB",
           "summary B/e",
           "event FP",
           "lin FP",
           "maps abs",
           "maps hit",
           "host abs",
           "kind abs",
           "ingest s");
    for (size_t i = 0; i < ncaps; i++) {
        run_t *r = &runs[i];
        printf("  %-10llu %9.1f %9.2f %8.1f %12.0f %11.4f %9.4f %9.2f %8.2f %8llu %8llu %8.1f\n",
               (unsigned long long)r->max_bytes,
               r->seal_ms,
               r->open_ms,
               (double)r->catalog.catalog_bytes / 1024.0,
               (double)r->catalog.trailer_bytes / (double)r->catalog.entries,
               r->event_fp,
               r->lineage_fp,
               r->absent_event_maps,
               r->present_event_maps,
               (unsigned long long)r->absent_host_maps,
               (unsigned long long)r->absent_kind_maps,
               r->ingest_ms / 1000.0);
    }

    // v4 lists costed from the key counts of the widest cap's catalog, which
    // are complete only when that cap summarized every field.
    catalog_cost_t *full = &runs[widest].catalog;
    printf("\n  v4 exact lists from the key counts at cap %llu, per entry: capped at "
           "%d keys %.0f B, uncapped %.0f B\n",
           (unsigned long long)caps[widest],
           BASE_MAX_KEYS,
           (double)full->base_trailer_bytes / (double)full->entries,
           (double)full->v4_trailer_bytes / (double)full->entries);
    for (size_t i = 0; i < ncaps; i++) {
        if (caps[i] == 0) {
            continue;
        }
        printf("\n  per field, per entry, at cap %llu:\n", (unsigned long long)caps[i]);
        print_fields(&runs[i].catalog);
    }
    n00b_shutdown();
    return 0;
}
