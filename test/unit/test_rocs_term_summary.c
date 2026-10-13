/* test/unit/test_rocs_term_summary.c - proving a TERM value absent must not
 * map every sealed shard in the catalog.
 *
 * Each sealed shard's catalog entry carries, per TERM-indexed field, a Bloom
 * filter over the column keys (normalized-term hashes) present, written at
 * seal (catalog format v5) and capped in bytes by term_summary_max_bytes.
 * The planner consults it before mapping: a TERM
 * equality whose keys the filter rules out skips the map entirely. Catalogs
 * written as v4 carry exact key lists, which load as filters.
 *
 * Measured on a 261-shard store without it: a present value cost 0 maps and a
 * value absent from the store cost 261 maps / 32 GB / 110 ms to return nothing.
 * The counter under test is the residency cache's miss count, which is exactly
 * "shards mapped on demand".
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/runtime.h"
#include "rocs/filter.h"
#include "rocs/query.h"
#include "rocs/store.h"
#include "util/assert.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include "internal/rocs/eval.h"
#include "internal/rocs/plan.h"
#include "internal/rocs/store.h"
#include "internal/rocs/term_bloom.h"
#include "rocs_test_support.h"

#include "test_check.h"

#define CATALOG_PATH r"/rocs/catalog.rocs"

typedef struct {
    uint64_t                  max_bytes;
    uint64_t                  watermark;
    n00b_store_seal_policy_t *seal_policy;
    bool                      keep_standby;
} open_opts_t;

static open_opts_t
default_opts(void)
{
    return (open_opts_t){
        .max_bytes = N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT,
        .watermark = N00B_STORE_SCHEMA_DECLARED_SINCE_NS,
    };
}

static n00b_vfs_t *
new_memory_vfs(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);
    auto be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    return vfs;
}

static n00b_store_schema_t *
term_schema(void)
{
    n00b_store_schema_t *schema = n00b_result_get(n00b_store_schema_new());
    CHECK(n00b_result_is_ok(n00b_store_schema_add_field(schema,
                                                        r"kind",
                                                        .index_kind = N00B_STORE_INDEX_TERM)));
    CHECK(n00b_result_is_ok(n00b_store_schema_add_field(schema,
                                                        r"host",
                                                        .index_kind = N00B_STORE_INDEX_TERM)));
    CHECK(n00b_result_is_ok(n00b_store_schema_add_field(schema,
                                                        r"score",
                                                        .index_kind = N00B_STORE_INDEX_TERM)));
    return schema;
}

static n00b_store_t *
open_with(n00b_vfs_t *vfs, open_opts_t opts)
{
    auto store_r = n00b_store_open_vfs(vfs,
                                       r"/rocs",
                                       term_schema(),
                                       .seal_policy              = opts.seal_policy,
                                       .keep_standby             = opts.keep_standby,
                                       .schema_declared_since_ns = opts.watermark,
                                       .term_summary_max_bytes   = opts.max_bytes);
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

static n00b_store_t *
open_store(n00b_vfs_t *vfs)
{
    return open_with(vfs, default_opts());
}

// A record with a kind and, when host is non-null, a host.
static void
ingest(n00b_store_t *store, int64_t id, const char *kind, const char *host)
{
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec, "id", n00b_json_int_new(id));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kind));
    if (host != nullptr) {
        n00b_json_object_put(rec, "host", n00b_json_string_new(host));
    }
    CHECK(n00b_result_is_ok(n00b_store_ingest(store, rec)));
}

static void
ingest_score(n00b_store_t *store, int64_t id, const char *kind, double score)
{
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec, "id", n00b_json_int_new(id));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kind));
    n00b_json_object_put(rec, "score", n00b_json_double_new(score));
    CHECK(n00b_result_is_ok(n00b_store_ingest(store, rec)));
}

static void
seal(n00b_store_t *store, uint64_t ts)
{
    CHECK(n00b_result_is_ok(n00b_store_seal_hot_shard(store, .seal_ts = ts)));
}

static void
close_store(n00b_store_t *store)
{
    CHECK(n00b_result_is_ok(n00b_store_close(store)));
}

static uint64_t
misses(n00b_store_t *store)
{
    auto r = n00b_store_residency_stats(store);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r).cache_misses;
}

static uint64_t
count_value(n00b_store_t *store, n00b_string_t *field, n00b_filter_value_t value)
{
    auto field_r = n00b_filter_field(field);
    CHECK(n00b_result_is_ok(field_r));
    auto filter_r = n00b_filter_eq(n00b_result_get(field_r), value);
    CHECK(n00b_result_is_ok(filter_r));
    auto query_r = n00b_query_new(n00b_result_get(filter_r), .limit = 1000);
    CHECK(n00b_result_is_ok(query_r));
    auto result_r = n00b_query_run(store, n00b_result_get(query_r));
    CHECK(n00b_result_is_ok(result_r));
    n00b_query_result_t *result = n00b_result_get(result_r);
    uint64_t             n      = n00b_query_count(result);
    (void)n00b_query_result_close(result);
    return n;
}

static uint64_t
count_eq(n00b_store_t *store, n00b_string_t *field, const char *value)
{
    return count_value(store, field, n00b_fv_utf8(n00b_string_from_cstr(value)));
}

// Queries a freshly opened store, so no shard is resident and every catalog
// entry is read back from disk, and reports the answer and the maps it cost.
static uint64_t
query_maps(n00b_vfs_t *vfs, n00b_string_t *field, const char *value, uint64_t *found)
{
    n00b_store_t *store  = open_store(vfs);
    uint64_t      before = misses(store);
    *found               = count_eq(store, field, value);
    uint64_t maps        = misses(store) - before;
    close_store(store);
    return maps;
}

// Runs the per-shard planning path the query cursor takes (no settled plan)
// over every visible sealed shard of a freshly opened store, and reports how
// many shards it mapped and how many records matched.
static uint64_t
gate_maps(n00b_vfs_t *vfs, open_opts_t opts, n00b_plan_predicate_t *pred, uint64_t *matches)
{
    n00b_store_t *store = open_with(vfs, opts);
    auto          idx_r = n00b_store_plan_indexes_for_query(store);
    CHECK(n00b_result_is_ok(idx_r));

    auto n_r = n00b_store_catalog_visible_entry_count(store);
    CHECK(n00b_result_is_ok(n_r));

    uint64_t before = misses(store);
    uint64_t total  = 0;
    for (uint64_t i = 0; i < n00b_result_get(n_r); i++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
        CHECK(n00b_result_is_ok(entry_r));
        CHECK(n00b_option_is_set(n00b_result_get(entry_r)));
        auto shard_r = n00b_plan_catalog_entry_sealed(
            store,
            n00b_option_get(n00b_result_get(entry_r)),
            pred,
            n00b_result_get(idx_r));
        CHECK(n00b_result_is_ok(shard_r));
        auto ords_r = n00b_plan_shard_result_ordinals(n00b_result_get(shard_r));
        CHECK(n00b_result_is_ok(ords_r));
        auto count_r = n00b_plan_ordset_count(n00b_result_get(ords_r));
        CHECK(n00b_result_is_ok(count_r));
        total += n00b_result_get(count_r);
    }
    uint64_t maps = misses(store) - before;
    close_store(store);
    *matches = total;
    return maps;
}

// Runs the sealed fan-out, which folds every kept shard into one plan per
// partition before running any, over a freshly opened store, and reports how
// many shards it mapped and how many records matched.
static uint64_t
fanout_maps(n00b_vfs_t *vfs, open_opts_t opts, n00b_plan_predicate_t *pred, uint64_t *matches)
{
    n00b_store_t *store = open_with(vfs, opts);
    auto          idx_r = n00b_store_plan_indexes_for_query(store);
    CHECK(n00b_result_is_ok(idx_r));

    uint64_t before    = misses(store);
    auto     results_r = n00b_plan_store_sealed(store, pred, n00b_result_get(idx_r));
    CHECK(n00b_result_is_ok(results_r));
    n00b_plan_shard_result_list_t *results = n00b_result_get(results_r);
    auto                           n_r     = n00b_plan_shard_result_count(results);
    CHECK(n00b_result_is_ok(n_r));

    uint64_t total = 0;
    for (uint64_t i = 0; i < n00b_result_get(n_r); i++) {
        auto shard_r = n00b_plan_shard_result_at(results, i);
        CHECK(n00b_result_is_ok(shard_r));
        CHECK(n00b_option_is_set(n00b_result_get(shard_r)));
        auto ords_r = n00b_plan_shard_result_ordinals(
            n00b_option_get(n00b_result_get(shard_r)));
        CHECK(n00b_result_is_ok(ords_r));
        auto count_r = n00b_plan_ordset_count(n00b_result_get(ords_r));
        CHECK(n00b_result_is_ok(count_r));
        total += n00b_result_get(count_r);
    }
    uint64_t maps = misses(store) - before;
    close_store(store);
    *matches = total;
    return maps;
}

static n00b_buffer_t *
read_catalog(n00b_vfs_t *vfs)
{
    auto open_r = n00b_vfs_open(vfs, CATALOG_PATH, N00B_VFS_O_R);
    CHECK(n00b_result_is_ok(open_r));
    auto read_r = n00b_vfs_read(vfs, n00b_result_get(open_r), UINT64_C(1) << 20);
    CHECK(n00b_result_is_ok(read_r));
    CHECK(n00b_result_is_ok(n00b_vfs_close(vfs, n00b_result_get(open_r))));
    return n00b_result_get(read_r);
}

static void
write_catalog(n00b_vfs_t *vfs, n00b_buffer_t *image)
{
    auto open_r = n00b_vfs_open(vfs, CATALOG_PATH, N00B_VFS_O_W);
    CHECK(n00b_result_is_ok(open_r));
    auto write_r = n00b_vfs_write(vfs, n00b_result_get(open_r), image);
    CHECK(n00b_result_is_ok(write_r));
    CHECK(n00b_result_get(write_r) == (uint64_t)n00b_buffer_len(image));
    CHECK(n00b_result_is_ok(n00b_vfs_close(vfs, n00b_result_get(open_r))));
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

static void
put_u64(n00b_buffer_t *image, int64_t at, uint64_t v)
{
    uint8_t *b = (uint8_t *)image->data;
    for (int i = 0; i < 8; i++) {
        b[at + i] = (uint8_t)(v >> (i * 8));
    }
}

// Deterministic 128-bit keys (splitmix64), standing in for the normalized-term
// hashes a column holds.
typedef struct {
    uint64_t x;
} mix_t;

static uint64_t
mix_next(mix_t *m)
{
    m->x += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = m->x;
    z          = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z          = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static n00b_uint128_t
mix_key(mix_t *m)
{
    uint64_t hi = mix_next(m);
    uint64_t lo = mix_next(m);
    return ((n00b_uint128_t)hi << 64) | lo;
}

static int
u128_cmp(const void *a, const void *b)
{
    n00b_uint128_t x = *(const n00b_uint128_t *)a;
    n00b_uint128_t y = *(const n00b_uint128_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// The column keys the planner resolves for field == value.
static size_t
resolve(n00b_string_t *field, const char *value, n00b_uint128_t out[8])
{
    auto keys_r = n00b_store_index_keys_new(index_of(field, N00B_STORE_INDEX_TERM),
                                            n00b_json_string_new(value));
    CHECK(n00b_result_is_ok(keys_r));
    n00b_store_index_keys_t *keys = n00b_result_get(keys_r);
    uint64_t                 n    = n00b_store_index_keys_count(keys);
    CHECK(n > 0 && n <= 8);
    for (uint64_t i = 0; i < n; i++) {
        out[i] = n00b_store_index_keys_at(keys, i);
    }
    return (size_t)n;
}

// Whether the entry's summary leaves room for field == value, asked with the
// same resolved keys the planner hands the catalog check.
static bool
entry_may_contain(n00b_store_catalog_entry_t *entry,
                  n00b_string_t              *field,
                  const char                 *value)
{
    n00b_uint128_t arr[8];
    size_t         n = resolve(field, value, arr);
    return n00b_store_catalog_entry_may_contain_term(entry, field, arr, n);
}

static bool
entry_may_contain_key(n00b_store_catalog_entry_t *entry,
                      n00b_string_t              *field,
                      n00b_uint128_t              key)
{
    return n00b_store_catalog_entry_may_contain_term(entry, field, &key, 1);
}

static n00b_store_catalog_entry_t *
visible_entry(n00b_store_t *store, uint64_t i)
{
    auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
    CHECK(n00b_result_is_ok(entry_r));
    CHECK(n00b_option_is_set(n00b_result_get(entry_r)));
    return n00b_option_get(n00b_result_get(entry_r));
}

// A store with one sealed shard holding kinds a, b, and c.
static n00b_vfs_t *
one_shard_store(void)
{
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);
    ingest(store, 1, "a", nullptr);
    ingest(store, 2, "b", nullptr);
    ingest(store, 3, "c", nullptr);
    seal(store, 1000);
    close_store(store);
    return vfs;
}

// The v7 layout of a catalog holding one entry: magic, ten header u64s
// (version at byte 8, log epoch and log id at bytes 64 and 72, entry count
// at byte 80), then the entry's seven u64s and three length-prefixed
// strings, then its TERM summary trailer. The trailer is
// a field count and, per field, its name and key count, then for a nonzero
// count k, bit count, and the filter bytes as a length and that many bytes.
#define ENTRY_AT   88
// Versions before 7 have no log epoch or log id, the sixteen bytes at
// EPOCH_AT.
#define EPOCH_AT   64
#define EPOCH_LEN  16
#define MAX_FIELDS 4

typedef struct {
    char     name[16];
    int64_t  nkeys_at; // k, nbits, and nbytes follow at 8, 16, and 24
    int64_t  bits_at;
    uint64_t nkeys;
    uint64_t k;
    uint64_t nbits;
    uint64_t nbytes;
} field_rec_t;

typedef struct {
    int64_t     nsum_at;
    uint64_t    nsum;
    field_rec_t f[MAX_FIELDS];
} trailer_t;

static int64_t
skip_string(n00b_buffer_t *image, int64_t at)
{
    return at + 8 + (int64_t)get_u64(image, at);
}

// The zone-map section each entry carries after its TERM summary (catalog
// v6): a count, then per zone a field name, and a kind byte plus a value for
// each of the two bounds. A value is eight bytes unless the kind is STRING
// (3), which is length-prefixed. Skipping it is what keeps the end-of-image
// check below honest -- it is the check that caught this section being added.
static int64_t
skip_zones(n00b_buffer_t *image, int64_t at)
{
    uint64_t nzones = get_u64(image, at);
    at += 8;
    for (uint64_t z = 0; z < nzones; z++) {
        at = skip_string(image, at);
        for (int bound = 0; bound < 2; bound++) {
            uint8_t kind = (uint8_t)image->data[at++];
            at = kind == 3 ? skip_string(image, at) : at + 8;
        }
    }
    return at;
}

// Walks the whole image and checks that it ends where the trailer does, so a
// layout change fails here rather than mutating the wrong bytes.
static trailer_t
walk_trailer(n00b_buffer_t *image)
{
    trailer_t t = {};
    CHECK(memcmp(image->data, "ROCSCAT1", 8) == 0);
    CHECK(get_u64(image, 8) == 7);
    CHECK(get_u64(image, 80) == 1);
    int64_t at = ENTRY_AT + 7 * 8;
    for (int i = 0; i < 3; i++) {
        at = skip_string(image, at);
    }
    t.nsum_at = at;
    t.nsum    = get_u64(image, at);
    CHECK(t.nsum <= MAX_FIELDS);
    at += 8;
    for (uint64_t i = 0; i < t.nsum; i++) {
        field_rec_t *f   = &t.f[i];
        uint64_t     len = get_u64(image, at);
        CHECK(len < sizeof(f->name));
        memcpy(f->name, image->data + at + 8, (size_t)len);
        f->name[len] = '\0';
        at           = skip_string(image, at);
        f->nkeys_at  = at;
        f->nkeys     = get_u64(image, at);
        if (f->nkeys == 0) {
            f->bits_at = at + 8;
            at         = f->bits_at;
            continue;
        }
        f->k       = get_u64(image, at + 8);
        f->nbits   = get_u64(image, at + 16);
        f->nbytes  = get_u64(image, at + 24);
        f->bits_at = at + 32;
        at         = f->bits_at + (int64_t)f->nbytes;
    }
    at = skip_zones(image, at);
    CHECK(at == (int64_t)n00b_buffer_len(image));
    return t;
}

// Growable byte image for hand-built catalogs.
typedef struct {
    uint8_t *data;
    int64_t  len;
    int64_t  cap;
} image_t;

static void
image_put(image_t *img, const void *bytes, int64_t n)
{
    if (img->len + n > img->cap) {
        img->cap  = (img->len + n) * 2;
        img->data = realloc(img->data, (size_t)img->cap);
        CHECK(img->data != nullptr);
    }
    memcpy(img->data + img->len, bytes, (size_t)n);
    img->len += n;
}

static void
image_u64(image_t *img, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (i * 8));
    }
    image_put(img, b, 8);
}

static void
image_name(image_t *img, const char *name)
{
    image_u64(img, strlen(name));
    image_put(img, name, (int64_t)strlen(name));
}

static n00b_buffer_t *
image_done(image_t *img)
{
    n00b_buffer_t *out = n00b_buffer_from_bytes((char *)img->data, img->len);
    free(img->data);
    return out;
}

// The one-entry image relabeled v4, with a v4 trailer: kind holds @p keys as
// given, host and score hold none.
static n00b_buffer_t *
v4_image(n00b_buffer_t *v5, const n00b_uint128_t *keys, size_t n)
{
    trailer_t t   = walk_trailer(v5);
    image_t   img = {};
    image_put(&img, v5->data, EPOCH_AT);
    image_put(&img,
              v5->data + EPOCH_AT + EPOCH_LEN,
              t.nsum_at - EPOCH_AT - EPOCH_LEN);
    image_u64(&img, 3);
    image_name(&img, "kind");
    image_u64(&img, n);
    for (size_t i = 0; i < n; i++) {
        image_u64(&img, (uint64_t)(keys[i] >> 64));
        image_u64(&img, (uint64_t)keys[i]);
    }
    image_name(&img, "host");
    image_u64(&img, 0);
    image_name(&img, "score");
    image_u64(&img, 0);
    n00b_buffer_t *out = image_done(&img);
    put_u64(out, 8, 4);
    return out;
}

// Three sealed shards through the rotation path (an explicit seal): two hold
// proc.exec, one holds file.modify, none holds ai.exec.
static void
test_absent_term_maps_no_shards(void)
{
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);

    ingest(store, 1, "proc.exec", nullptr);
    ingest(store, 2, "proc.exec", nullptr);
    seal(store, 1000);
    ingest(store, 3, "file.modify", nullptr);
    seal(store, 2000);
    ingest(store, 4, "proc.exec", nullptr);
    seal(store, 3000);
    close_store(store);

    uint64_t found     = 0;
    uint64_t proc_maps = query_maps(vfs, r"kind", "proc.exec", &found);
    CHECK(found == 3);
    CHECK(proc_maps == 2);

    CHECK(query_maps(vfs, r"kind", "file.modify", &found) == 1);
    CHECK(found == 1);

    uint64_t absent_maps = query_maps(vfs, r"kind", "ai.exec", &found);
    CHECK(found == 0);
    CHECK(absent_maps == 0);

    n00b_eprintf("  [PASS] absent_term_maps_no_shards (present=[|#:d|] maps, absent=[|#:d|] maps)\n",
                 (int64_t)proc_maps,
                 (int64_t)absent_maps);
}

// n00b_store_close seals a non-empty hot shard through the synchronous path.
static void
test_sync_seal_writes_summary(void)
{
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);
    ingest(store, 1, "proc.exec", nullptr);
    ingest(store, 2, "file.modify", nullptr);
    close_store(store);

    uint64_t found = 0;
    CHECK(query_maps(vfs, r"kind", "ai.exec", &found) == 0);
    CHECK(found == 0);
    CHECK(query_maps(vfs, r"kind", "proc.exec", &found) == 1);
    CHECK(found == 1);
    n00b_eprintf("  [PASS] sync_seal_writes_summary\n");
}

// keep_standby hands every policy-triggered seal to the seal worker pool.
static void
test_async_seal_writes_summary(void)
{
    n00b_vfs_t *vfs    = new_memory_vfs();
    auto        policy = n00b_store_seal_policy_new(.max_records = 2);
    CHECK(n00b_result_is_ok(policy));

    open_opts_t opts  = default_opts();
    opts.seal_policy  = n00b_result_get(policy);
    opts.keep_standby = true;

    n00b_store_t *store = open_with(vfs, opts);
    ingest(store, 1, "a", nullptr);
    ingest(store, 2, "a", nullptr);
    ingest(store, 3, "b", nullptr);
    ingest(store, 4, "b", nullptr);
    CHECK(n00b_result_is_ok(n00b_store_flush(store)));

    auto stats_r = n00b_store_memory_stats(store);
    CHECK(n00b_result_is_ok(stats_r));
    n00b_store_memory_stats_t stats = n00b_result_get(stats_r);
    CHECK(stats.seal_worker_count >= 1);
    CHECK(stats.sealed_shards == 2);
    CHECK(stats.hot_record_count == 0);
    close_store(store);

    uint64_t found = 0;
    CHECK(query_maps(vfs, r"kind", "zzz", &found) == 0);
    CHECK(found == 0);
    CHECK(query_maps(vfs, r"kind", "a", &found) == 1);
    CHECK(found == 2);
    n00b_eprintf("  [PASS] async_seal_writes_summary\n");
}

// A catalog written before format v4 has no summary trailers. It must load,
// and every query must map as it did before summaries existed.
static void
test_v3_catalog_still_maps(void)
{
    n00b_vfs_t    *vfs   = one_shard_store();
    n00b_buffer_t *image = read_catalog(vfs);
    trailer_t      t     = walk_trailer(image);

    // Relabel the header, drop the epoch and log id, and strip the only
    // entry's trailer.
    image_t img = {};
    image_put(&img, image->data, EPOCH_AT);
    image_put(&img,
              image->data + EPOCH_AT + EPOCH_LEN,
              t.nsum_at - EPOCH_AT - EPOCH_LEN);
    n00b_buffer_t *v3 = image_done(&img);
    put_u64(v3, 8, 3);
    write_catalog(vfs, v3);

    n00b_store_t *store = open_store(vfs);
    CHECK(!n00b_store_opened_degraded(store));
    auto count_r = n00b_store_catalog_get_entry_count(store);
    CHECK(n00b_result_is_ok(count_r));
    CHECK(n00b_result_get(count_r) == 1);
    // A shard sealed now gets a summary beside the one that has none.
    ingest(store, 4, "d", nullptr);
    seal(store, 2000);
    close_store(store);

    // The shard without a summary is mapped for every value; the new shard
    // only for a value it holds.
    uint64_t found = 0;
    CHECK(query_maps(vfs, r"kind", "b", &found) == 1);
    CHECK(found == 1);
    CHECK(query_maps(vfs, r"kind", "d", &found) == 2);
    CHECK(found == 1);
    CHECK(query_maps(vfs, r"kind", "zzz", &found) == 1);
    CHECK(found == 0);
    n00b_eprintf("  [PASS] v3_catalog_still_maps\n");
}

// A field whose filter would not fit the cap gets no summary in that shard,
// while a field under it in the same shard keeps its own. Eight bytes hold 64
// bits, and 45 keys in 64 bits is under 1.443 bits a key.
static void
test_cap_omits_field_summary(void)
{
    n00b_vfs_t *vfs  = new_memory_vfs();
    open_opts_t opts = default_opts();
    opts.max_bytes   = 8;

    n00b_store_t *store = open_with(vfs, opts);
    ingest(store, 0, "a", "h1");
    for (int64_t i = 1; i < 45; i++) {
        char kind[32];
        snprintf(kind, sizeof(kind), "k%lld", (long long)i);
        ingest(store, i, kind, "h1");
    }
    seal(store, 1000);
    ingest(store, 100, "a", "h2");
    seal(store, 2000);
    close_store(store);

    uint64_t found = 0;
    // kind is over the cap in the first shard only.
    CHECK(query_maps(vfs, r"kind", "zzz", &found) == 1);
    CHECK(found == 0);
    CHECK(query_maps(vfs, r"kind", "k3", &found) == 1);
    CHECK(found == 1);
    CHECK(query_maps(vfs, r"kind", "a", &found) == 2);
    CHECK(found == 2);
    // host stayed under the cap in both.
    CHECK(query_maps(vfs, r"host", "zzz", &found) == 0);
    CHECK(found == 0);
    CHECK(query_maps(vfs, r"host", "h2", &found) == 1);
    CHECK(found == 1);
    n00b_eprintf("  [PASS] cap_omits_field_summary\n");
}

// The catalog length of a store holding one sealed shard with @p nkinds
// distinct kinds, all on one host.
static int64_t
catalog_len_for_kinds(int64_t nkinds)
{
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);
    for (int64_t i = 0; i < nkinds; i++) {
        char kind[32];
        snprintf(kind, sizeof(kind), "k%lld", (long long)i);
        ingest(store, i, kind, "h1");
    }
    seal(store, 1000);
    close_store(store);

    uint64_t found = 0;
    CHECK(query_maps(vfs, r"kind", "k0", &found) == 1);
    CHECK(found == 1);
    return n00b_buffer_len(read_catalog(vfs));
}

// A shard's summary for one field grows the catalog by at most the default
// cap, however many distinct values the field holds. 5,000 kinds as a key
// list would take 80,000 bytes.
static void
test_summary_bytes_bounded(void)
{
    int64_t few  = catalog_len_for_kinds(1);
    int64_t many = catalog_len_for_kinds(5000);
    CHECK(many > few);
    CHECK((uint64_t)(many - few) <= N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT);
    n00b_eprintf("  [PASS] summary_bytes_bounded (+[|#:d|] bytes for 4,999 more kinds)\n",
                 many - few);
}

// Sealing declares an empty column for every indexed field no record in the
// shard populated, and the executor answers exact-empty for such a column
// whatever the schema watermark says. Its empty summary therefore skips the
// shard even with the watermark at zero, and gives the answer a store without
// summaries gives.
static void
test_declared_empty_column_skips(void)
{
    n00b_vfs_t *with     = new_memory_vfs();
    n00b_vfs_t *without  = new_memory_vfs();
    open_opts_t on       = default_opts();
    on.watermark         = 0;
    open_opts_t off      = on;
    off.max_bytes        = 0;
    n00b_vfs_t *vfss[2]  = {with, without};
    open_opts_t opts[2]  = {on, off};

    for (int i = 0; i < 2; i++) {
        n00b_store_t *store = open_with(vfss[i], opts[i]);
        ingest(store, 1, "a", nullptr);
        seal(store, 1000);
        ingest(store, 2, "b", "h1");
        seal(store, 2000);
        close_store(store);
    }

    uint64_t m_with    = 0;
    uint64_t m_without = 0;
    CHECK(gate_maps(with, on, eq(r"host", r"zzz"), &m_with) == 0);
    CHECK(gate_maps(without, off, eq(r"host", r"zzz"), &m_without) == 2);
    CHECK(m_with == 0 && m_without == 0);
    CHECK(gate_maps(with, on, eq(r"host", r"h1"), &m_with) == 1);
    CHECK(gate_maps(without, off, eq(r"host", r"h1"), &m_without) == 2);
    CHECK(m_with == 1 && m_without == 1);
    n00b_eprintf("  [PASS] declared_empty_column_skips\n");
}

static n00b_store_term_bloom_t *
bloom_of(const n00b_uint128_t *keys, size_t n, uint64_t max_bytes)
{
    uint64_t nbits = 0;
    uint32_t k     = 0;
    CHECK(n00b_store_term_bloom_size(n, max_bytes, &nbits, &k));
    n00b_store_term_bloom_t *bloom = n00b_store_term_bloom_new(nbits, k);
    for (size_t i = 0; i < n; i++) {
        n00b_store_term_bloom_add(bloom, keys[i]);
    }
    CHECK(bloom->nkeys == n);
    return bloom;
}

static n00b_uint128_t *
mix_keys(uint64_t seed, size_t n)
{
    n00b_uint128_t *keys = malloc(n * sizeof(*keys));
    CHECK(keys != nullptr);
    mix_t m = {seed};
    for (size_t i = 0; i < n; i++) {
        keys[i] = mix_key(&m);
    }
    return keys;
}

// Every key added probes present, at full size (k 7) and at a cap that leaves
// one probe (k 1).
static void
test_bloom_no_false_negatives(void)
{
    enum { N = 20000 };
    n00b_uint128_t *keys = mix_keys(1, N);
    uint64_t        caps[2]   = {UINT64_C(1) << 20, 4096};
    uint32_t        expect[2] = {7, 1};
    for (int c = 0; c < 2; c++) {
        n00b_store_term_bloom_t *bloom = bloom_of(keys, N, caps[c]);
        CHECK(bloom->k == expect[c]);
        for (size_t i = 0; i < N; i++) {
            CHECK(n00b_store_term_bloom_may_contain(bloom, keys[i]));
        }
    }
    free(keys);
    n00b_eprintf("  [PASS] bloom_no_false_negatives\n");
}

// Absent keys pass at the rate the sizing targets: within 0.8% to 1.25% at
// full size, and within a tenth of (1 - e^(-kn/m))^k when the cap shrinks the
// filter.
static void
test_bloom_false_positive_rate(void)
{
    enum { N = 10000, ABSENT = 200000 };
    n00b_uint128_t *keys   = mix_keys(1, N);
    n00b_uint128_t *absent = mix_keys(2, ABSENT);

    n00b_store_term_bloom_t *full   = bloom_of(keys, N, UINT64_C(1) << 20);
    n00b_store_term_bloom_t *capped = bloom_of(keys, N, 4096);
    CHECK(full->k == 7 && full->nbits == 95872);
    CHECK(capped->k == 2 && capped->nbits == 32768);

    uint64_t fp_full   = 0;
    uint64_t fp_capped = 0;
    for (size_t i = 0; i < ABSENT; i++) {
        fp_full += n00b_store_term_bloom_may_contain(full, absent[i]);
        fp_capped += n00b_store_term_bloom_may_contain(capped, absent[i]);
    }
    double rate_full   = (double)fp_full / ABSENT;
    double rate_capped = (double)fp_capped / ABSENT;
    double predicted   = pow(1.0 - exp(-2.0 * N / 32768.0), 2.0);
    CHECK(rate_full >= 0.008 && rate_full <= 0.0125);
    CHECK(fabs(rate_capped - predicted) <= predicted / 10);
    free(keys);
    free(absent);
    fprintf(stderr,
            "  [PASS] bloom_false_positive_rate (full %.4f, capped %.4f vs %.4f)\n",
            rate_full,
            rate_capped,
            predicted);
}

static void
check_bytes(n00b_store_term_bloom_t *bloom, const uint8_t *expect, size_t n)
{
    CHECK(bloom->nbits / 8 == n);
    CHECK(memcmp(bloom->bits, expect, n) == 0);
}

// Fixed keys give fixed bytes, so any change to the probe derivation, the bit
// order, or the sizing shows up here. The bytes come from the layout in
// internal/rocs/term_bloom.h, computed independently of this code.
static void
test_bloom_golden_layout(void)
{
    n00b_uint128_t three[3] = {
        ((n00b_uint128_t)UINT64_C(0x0123456789abcdef) << 64)
            | UINT64_C(0xfedcba9876543210),
        ((n00b_uint128_t)UINT64_C(0x0f1e2d3c4b5a6978) << 64)
            | UINT64_C(0x8796a5b4c3d2e1f0),
        ((n00b_uint128_t)UINT64_C(0xdeadbeefcafebabe) << 64) | UINT64_C(1),
    };
    const uint8_t three_bytes[8] = {0x1f, 0x08, 0x22, 0x44, 0x04, 0xa2, 0x08, 0xd1};
    n00b_store_term_bloom_t *a = bloom_of(three, 3, 4096);
    CHECK(a->k == 7 && a->nbits == 64);
    check_bytes(a, three_bytes, sizeof(three_bytes));

    // Twenty keys want 192 bits, so a 16-byte cap takes 128 bits and
    // round(ln 2 * 128 / 20) = 4 probes.
    n00b_uint128_t twenty[20];
    for (uint64_t i = 0; i < 20; i++) {
        uint64_t hi = UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
        uint64_t lo = UINT64_C(0xbf58476d1ce4e5b9) * (i + 7);
        twenty[i]   = ((n00b_uint128_t)hi << 64) | lo;
    }
    const uint8_t twenty_bytes[16] = {0xb4, 0x80, 0x3b, 0x48, 0x59, 0x1d, 0x8b, 0xbc,
                                      0x87, 0x94, 0x1d, 0x31, 0xe8, 0x8b, 0x18, 0xd7};
    n00b_store_term_bloom_t *b = bloom_of(twenty, 20, 16);
    CHECK(b->k == 4 && b->nbits == 128);
    check_bytes(b, twenty_bytes, sizeof(twenty_bytes));
    n00b_eprintf("  [PASS] bloom_golden_layout\n");
}

static void
check_size(uint64_t nkeys, uint64_t max_bytes, bool fits, uint64_t nbits, uint32_t k)
{
    uint64_t got_bits = 0;
    uint32_t got_k    = 0;
    CHECK(n00b_store_term_bloom_size(nkeys, max_bytes, &got_bits, &got_k) == fits);
    if (fits) {
        CHECK(got_bits == nbits);
        CHECK(got_k == k);
    }
}

// Where the sizing switches from the 1% size to the cap, and from the cap to
// leaving the field out.
static void
test_bloom_size_boundaries(void)
{
    check_size(0, 4096, true, 0, 7);
    check_size(1, 7, false, 0, 0);
    // Six keys want 58 bits, which rounds to the 64 an 8-byte cap holds; seven
    // want 68, so they get the cap and six probes.
    check_size(6, 8, true, 64, 7);
    check_size(7, 8, true, 64, 6);
    // 44 keys in 64 bits is 1.45 bits a key; 45 is 1.42, under 1 / ln 2.
    check_size(44, 8, true, 64, 1);
    check_size(45, 8, false, 0, 0);
    // The default cap: 3,418 keys get the 1% size exactly, and 22,708 is the
    // most it summarizes.
    check_size(3418, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT, true, 32768, 7);
    check_size(3419, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT, true, 32768, 7);
    check_size(10000, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT, true, 32768, 2);
    check_size(22708, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT, true, 32768, 1);
    check_size(22709, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT, false, 0, 0);
    n00b_eprintf("  [PASS] bloom_size_boundaries\n");
}

// The catalog stores exactly the filter the column keys build, in the layout
// walk_trailer reads, and a declared empty column as a bare zero key count.
static void
test_catalog_layout(void)
{
    n00b_vfs_t    *vfs   = one_shard_store();
    n00b_buffer_t *image = read_catalog(vfs);
    trailer_t      t     = walk_trailer(image);
    CHECK(t.nsum == 3);
    CHECK(strcmp(t.f[0].name, "kind") == 0);
    CHECK(strcmp(t.f[1].name, "host") == 0);
    CHECK(strcmp(t.f[2].name, "score") == 0);

    n00b_uint128_t keys[3];
    const char    *kinds[3] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        n00b_uint128_t one[8];
        CHECK(resolve(r"kind", kinds[i], one) == 1);
        keys[i] = one[0];
    }
    n00b_store_term_bloom_t *expect = bloom_of(keys, 3, N00B_STORE_TERM_SUMMARY_MAX_BYTES_DEFAULT);
    field_rec_t             *kind   = &t.f[0];
    CHECK(kind->nkeys == 3 && kind->k == expect->k && kind->nbits == expect->nbits);
    CHECK(kind->nbytes == expect->nbits / 8);
    CHECK(memcmp(image->data + kind->bits_at, expect->bits, kind->nbytes) == 0);
    for (int i = 1; i < 3; i++) {
        CHECK(t.f[i].nkeys == 0);
        CHECK(t.f[i].bits_at == t.f[i].nkeys_at + 8);
    }
    n00b_eprintf("  [PASS] catalog_layout\n");
}

// A v4 catalog's key lists load as the filters a seal would build over them:
// no listed key is missed, absent keys pass at the 1% rate, and the next seal
// rewrites the catalog as v5 holding those same filters.
static void
test_v4_catalog_loads_as_filter(void)
{
    enum { RANDOM = 2000, ABSENT = 5000 };
    size_t          n    = RANDOM + 3;
    n00b_uint128_t *keys = mix_keys(7, n);
    const char     *kinds[3] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        n00b_uint128_t one[8];
        CHECK(resolve(r"kind", kinds[i], one) == 1);
        keys[RANDOM + i] = one[0];
    }
    qsort(keys, n, sizeof(*keys), u128_cmp);
    n00b_uint128_t *absent = mix_keys(8, ABSENT);
    bool           *passed = calloc(ABSENT, sizeof(*passed));
    CHECK(passed != nullptr);

    n00b_vfs_t *vfs = one_shard_store();
    write_catalog(vfs, v4_image(read_catalog(vfs), keys, n));

    n00b_store_t *store = open_store(vfs);
    CHECK(!n00b_store_opened_degraded(store));
    n00b_store_catalog_entry_t *entry     = visible_entry(store, 0);
    uint64_t                    false_pos = 0;
    for (size_t i = 0; i < n; i++) {
        CHECK(entry_may_contain_key(entry, r"kind", keys[i]));
    }
    for (size_t i = 0; i < ABSENT; i++) {
        passed[i] = entry_may_contain_key(entry, r"kind", absent[i]);
        false_pos += passed[i];
    }
    // 2,003 keys get the 1% size under the default cap: about 50 of 5,000.
    CHECK(false_pos > 0 && false_pos < 150);
    CHECK(!entry_may_contain(entry, r"host", "h1"));
    ingest(store, 4, "d", nullptr);
    seal(store, 2000);
    close_store(store);
    CHECK(get_u64(read_catalog(vfs), 8) == 7);

    store = open_store(vfs);
    entry = visible_entry(store, 0);
    for (size_t i = 0; i < n; i++) {
        CHECK(entry_may_contain_key(entry, r"kind", keys[i]));
    }
    for (size_t i = 0; i < ABSENT; i++) {
        CHECK(entry_may_contain_key(entry, r"kind", absent[i]) == passed[i]);
    }
    CHECK(!entry_may_contain(entry, r"host", "h1"));
    close_store(store);

    uint64_t found = 0;
    CHECK(query_maps(vfs, r"kind", "b", &found) == 1);
    CHECK(found == 1);
    CHECK(query_maps(vfs, r"kind", "d", &found) == 1);
    CHECK(found == 1);
    free(keys);
    free(absent);
    free(passed);
    fprintf(stderr,
            "  [PASS] v4_catalog_loads_as_filter (%llu of %d absent keys pass)\n",
            (unsigned long long)false_pos,
            (int)ABSENT);
}

// The filter a seal builds answers the same after close and reopen, where it
// is read back from the catalog.
static void
test_v5_round_trip(void)
{
    enum { NVALUES = 300, NSHARDS = 3 };
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);
    bool          before[NSHARDS][NVALUES];
    char          value[32];

    for (int s = 0; s < NSHARDS; s++) {
        for (int i = 0; i < 40; i++) {
            snprintf(value, sizeof(value), "v%d", s * 40 + i);
            ingest(store, s * 100 + i, value, value);
        }
        auto entry_r = n00b_store_seal_hot_shard(store, .seal_ts = (uint64_t)(s + 1) * 1000);
        CHECK(n00b_result_is_ok(entry_r));
        n00b_store_catalog_entry_t *entry = n00b_result_get(entry_r);
        for (int v = 0; v < NVALUES; v++) {
            snprintf(value, sizeof(value), "v%d", v);
            before[s][v] = entry_may_contain(entry, r"kind", value);
            if (v >= s * 40 && v < (s + 1) * 40) {
                CHECK(before[s][v]);
                CHECK(entry_may_contain(entry, r"host", value));
            }
        }
    }
    close_store(store);

    store = open_store(vfs);
    for (int s = 0; s < NSHARDS; s++) {
        n00b_store_catalog_entry_t *entry = visible_entry(store, (uint64_t)s);
        for (int v = 0; v < NVALUES; v++) {
            snprintf(value, sizeof(value), "v%d", v);
            CHECK(entry_may_contain(entry, r"kind", value) == before[s][v]);
        }
    }
    close_store(store);
    n00b_eprintf("  [PASS] v5_round_trip\n");
}

// Every summary shape the parser must refuse makes the catalog CORRUPT, which
// open answers with a degraded open. The untouched image opens clean.
static void
expect_catalog(n00b_buffer_t *(*mutate)(n00b_buffer_t *, trailer_t), bool degraded)
{
    n00b_vfs_t    *vfs   = one_shard_store();
    n00b_buffer_t *image = read_catalog(vfs);
    write_catalog(vfs, mutate(image, walk_trailer(image)));
    n00b_store_t *store = open_store(vfs);
    CHECK(n00b_store_opened_degraded(store) == degraded);
    close_store(store);
}

static n00b_buffer_t *
unchanged(n00b_buffer_t *image, trailer_t t)
{
    (void)t;
    return image;
}

static n00b_buffer_t *
bits_not_bytes(n00b_buffer_t *image, trailer_t t)
{
    put_u64(image, t.f[0].nkeys_at + 16, t.f[0].nbits + 64);
    return image;
}

// Shrinks kind's 8-byte filter to its first @p keep bytes, with bit and byte
// counts that agree.
static n00b_buffer_t *
shrink_kind_filter(n00b_buffer_t *image, trailer_t t, int64_t keep)
{
    CHECK(t.f[0].nbytes == 8);
    put_u64(image, t.f[0].nkeys_at + 16, (uint64_t)keep * 8);
    put_u64(image, t.f[0].nkeys_at + 24, (uint64_t)keep);
    image_t img = {};
    image_put(&img, image->data, t.f[0].bits_at + keep);
    image_put(&img,
              image->data + t.f[0].bits_at + 8,
              (int64_t)n00b_buffer_len(image) - t.f[0].bits_at - 8);
    return image_done(&img);
}

// 56 bits in 7 bytes, but no filter is sized off a 64-bit word.
static n00b_buffer_t *
bits_not_words(n00b_buffer_t *image, trailer_t t)
{
    return shrink_kind_filter(image, t, 7);
}

static n00b_buffer_t *
k_zero(n00b_buffer_t *image, trailer_t t)
{
    put_u64(image, t.f[0].nkeys_at + 8, 0);
    return image;
}

static n00b_buffer_t *
k_over_max(n00b_buffer_t *image, trailer_t t)
{
    put_u64(image, t.f[0].nkeys_at + 8, N00B_STORE_TERM_BLOOM_MAX_K + 1);
    return image;
}

static n00b_buffer_t *
filter_truncated(n00b_buffer_t *image, trailer_t t)
{
    return n00b_buffer_from_bytes(image->data, t.f[0].bits_at + 3);
}

// Bit and byte counts that agree, and claim far more than the buffer holds.
static n00b_buffer_t *
filter_past_buffer(n00b_buffer_t *image, trailer_t t)
{
    put_u64(image, t.f[0].nkeys_at + 16, UINT64_C(1) << 43);
    put_u64(image, t.f[0].nkeys_at + 24, UINT64_C(1) << 40);
    return image;
}

// Keys, and a filter of zero bits that could hold none.
static n00b_buffer_t *
keys_without_bits(n00b_buffer_t *image, trailer_t t)
{
    return shrink_kind_filter(image, t, 0);
}

static n00b_buffer_t *
nsum_over_bound(n00b_buffer_t *image, trailer_t t)
{
    put_u64(image, t.nsum_at, 4097);
    return image;
}

// The v4 reader keeps its own checks.
static n00b_uint128_t v4_keys[3] = {1, 2, 3};

static n00b_buffer_t *
v4_sorted(n00b_buffer_t *image, trailer_t t)
{
    (void)t;
    return v4_image(image, v4_keys, 3);
}

static n00b_buffer_t *
v4_unsorted(n00b_buffer_t *image, trailer_t t)
{
    (void)t;
    n00b_uint128_t keys[3] = {1, 3, 2};
    return v4_image(image, keys, 3);
}

static n00b_buffer_t *
v4_duplicate(n00b_buffer_t *image, trailer_t t)
{
    (void)t;
    n00b_uint128_t keys[3] = {1, 2, 2};
    return v4_image(image, keys, 3);
}

static void
test_parser_rejects_bad_summaries(void)
{
    expect_catalog(unchanged, false);
    expect_catalog(bits_not_bytes, true);
    expect_catalog(bits_not_words, true);
    expect_catalog(k_zero, true);
    expect_catalog(k_over_max, true);
    expect_catalog(filter_truncated, true);
    expect_catalog(filter_past_buffer, true);
    expect_catalog(keys_without_bits, true);
    expect_catalog(nsum_over_bound, true);
    expect_catalog(v4_sorted, false);
    expect_catalog(v4_unsorted, true);
    expect_catalog(v4_duplicate, true);
    n00b_eprintf("  [PASS] parser_rejects_bad_summaries\n");
}

static n00b_plan_predicate_t *
not_of(n00b_plan_predicate_t *child)
{
    auto r = n00b_plan_predicate_not(child);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// Two shards, {kind a, host h1} and {kind b, host h2}, planned through the
// catalog check the way the query cursor plans them.
static void
test_plan_shapes_through_gate(void)
{
    n00b_vfs_t   *vfs   = new_memory_vfs();
    n00b_store_t *store = open_store(vfs);
    ingest(store, 1, "a", "h1");
    seal(store, 1000);
    ingest(store, 2, "b", "h2");
    seal(store, 2000);
    close_store(store);

    open_opts_t o = default_opts();
    uint64_t    m = 0;

    // INTERSECT: one absent child rules a shard out.
    CHECK(gate_maps(vfs, o, group(eq(r"kind", r"a"), eq(r"host", r"h2"), true), &m) == 0);
    CHECK(m == 0);
    CHECK(gate_maps(vfs, o, group(eq(r"kind", r"a"), eq(r"host", r"h1"), true), &m) == 1);
    CHECK(m == 1);

    // UNION: a shard is ruled out only when every child is absent from it.
    CHECK(gate_maps(vfs, o, group(eq(r"kind", r"a"), eq(r"kind", r"zzz"), false), &m) == 1);
    CHECK(m == 1);
    CHECK(gate_maps(vfs, o, group(eq(r"kind", r"a"), eq(r"host", r"h2"), false), &m) == 2);
    CHECK(m == 2);
    CHECK(gate_maps(vfs, o, group(eq(r"kind", r"zzz"), eq(r"host", r"zzz"), false), &m) == 0);
    CHECK(m == 0);

    // COMPLEMENT: an absent value matches everything, so nothing is skipped.
    CHECK(gate_maps(vfs, o, not_of(eq(r"kind", r"zzz")), &m) == 2);
    CHECK(m == 2);
    CHECK(gate_maps(vfs, o, not_of(eq(r"kind", r"a")), &m) == 2);
    CHECK(m == 1);

#ifdef N00B_DEBUG
    // One plan per shard on the cursor path, shared by the catalog check and
    // the execution, whether the shard is skipped or mapped.
    n00b_plan_plans_built_reset();
    CHECK(gate_maps(vfs, o, eq(r"kind", r"a"), &m) == 1);
    CHECK(m == 1);
    CHECK(n00b_plan_plans_built() == 2);
#endif
    n00b_eprintf("  [PASS] plan_shapes_through_gate\n");
}

// Two shards, {kind a, host h1} and {kind b, host h3}, planned through the
// sealed fan-out. A shard the catalog rules out is neither folded into the
// partition's plan nor run, and every answer matches the same data in a store
// without summaries. host == h3 exists only in the ruled-out shard, so its
// count is left out of the plan the other shard runs under the complement.
static void
test_fanout_skips_ruled_out_shards(void)
{
    n00b_vfs_t *with    = new_memory_vfs();
    n00b_vfs_t *without = new_memory_vfs();
    open_opts_t on      = default_opts();
    open_opts_t off     = on;
    off.max_bytes       = 0;
    n00b_vfs_t *vfss[2] = {with, without};
    open_opts_t opts[2] = {on, off};

    // Both stores are sealed without value bounds. Every value these cases
    // rule out lies outside its shard's bounds, so bounds would skip the
    // same shards the summary does, and the counts below would hold with
    // the summary skip deleted. Without them, the summary store's skips are
    // the summary's alone, and the control store maps both shards.
    bool zones = n00b_store_zone_maps_enabled();
    n00b_store_zone_maps_set_enabled(false);
    for (int i = 0; i < 2; i++) {
        n00b_store_t *store = open_with(vfss[i], opts[i]);
        ingest(store, 1, "a", "h1");
        seal(store, 1000);
        ingest(store, 2, "b", "h3");
        seal(store, 2000);
        close_store(store);
    }
    n00b_store_zone_maps_set_enabled(zones);

    struct {
        n00b_plan_predicate_t *pred;
        uint64_t               maps;
        uint64_t               matches;
    } cases[] = {
        {eq(r"kind", r"zzz"), 0, 0},
        {group(eq(r"kind", r"zzz"), eq(r"host", r"h1"), true), 0, 0},
        {eq(r"kind", r"a"), 1, 1},
        {group(eq(r"kind", r"a"), not_of(eq(r"host", r"h3")), true), 1, 1},
        {group(eq(r"kind", r"a"), eq(r"host", r"h3"), false), 2, 2},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t m_with    = 0;
        uint64_t m_without = 0;
#ifdef N00B_DEBUG
        n00b_plan_shards_collected_reset();
#endif
        CHECK(fanout_maps(with, on, cases[i].pred, &m_with) == cases[i].maps);
#ifdef N00B_DEBUG
        CHECK(n00b_plan_shards_collected() <= cases[i].maps);
#endif
        CHECK(fanout_maps(without, off, cases[i].pred, &m_without) == 2);
        CHECK(m_with == cases[i].matches);
        CHECK(m_without == cases[i].matches);
    }
    n00b_eprintf("  [PASS] fanout_skips_ruled_out_shards\n");
}

// A plan that is EMPTY matches nothing on any shard, so it skips the map with
// no summary to consult: here the store writes none at all.
static void
test_empty_plan_skips_without_summary(void)
{
    n00b_vfs_t *vfs  = new_memory_vfs();
    open_opts_t opts = default_opts();
    opts.max_bytes   = 0;

    n00b_store_t *store = open_with(vfs, opts);
    ingest(store, 1, "a", "h1");
    seal(store, 1000);
    ingest(store, 2, "b", "h2");
    seal(store, 2000);
    close_store(store);

    auto false_r = n00b_plan_predicate_false();
    CHECK(n00b_result_is_ok(false_r));

    uint64_t m = 0;
    CHECK(gate_maps(vfs, opts, n00b_result_get(false_r), &m) == 0);
    CHECK(m == 0);
    // No summaries, so an absent value maps both.
    CHECK(gate_maps(vfs, opts, eq(r"kind", r"zzz"), &m) == 2);
    CHECK(m == 0);
    n00b_eprintf("  [PASS] empty_plan_skips_without_summary\n");
}

// Every answer from a store with summaries matches the same data in a store
// without them, including values that differ from the stored one only in a
// way TERM normalization erases (a signed zero) or keeps (letter case).
static void
test_normalized_values_agree(void)
{
    n00b_vfs_t *with    = new_memory_vfs();
    n00b_vfs_t *without = new_memory_vfs();
    open_opts_t off     = default_opts();
    off.max_bytes       = 0;

    n00b_store_t *both[2] = {open_store(with), open_with(without, off)};
    for (int i = 0; i < 2; i++) {
        ingest_score(both[i], 1, "Proc.Exec", -0.0);
        seal(both[i], 1000);
        ingest_score(both[i], 2, "file.modify", 1.5);
        seal(both[i], 2000);
        close_store(both[i]);
    }

    n00b_store_t *a = open_store(with);
    n00b_store_t *b = open_store(without);

    CHECK(count_value(a, r"score", n00b_fv_f64(0.0)) == 1);
    CHECK(count_value(a, r"score", n00b_fv_f64(-0.0)) == 1);
    CHECK(count_eq(a, r"kind", "Proc.Exec") == 1);

    n00b_filter_value_t scores[] = {
        n00b_fv_f64(0.0),
        n00b_fv_f64(-0.0),
        n00b_fv_f64(1.5),
        n00b_fv_f64(2.0),
    };
    for (size_t i = 0; i < sizeof(scores) / sizeof(scores[0]); i++) {
        CHECK(count_value(a, r"score", scores[i])
              == count_value(b, r"score", scores[i]));
    }
    const char *kinds[] = {"Proc.Exec", "proc.exec", "PROC.EXEC", "file.modify"};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        CHECK(count_eq(a, r"kind", kinds[i]) == count_eq(b, r"kind", kinds[i]));
    }

    close_store(a);
    close_store(b);
    n00b_eprintf("  [PASS] normalized_values_agree\n");
}


// 45 kinds on host h1, sealed. With an 8-byte cap, kind gets no summary (45
// keys in 64 bits) and host (one key) does.
#define CAPPED_KINDS 45

static n00b_json_node_t *
capped_record(int64_t i)
{
    char kind[32];
    snprintf(kind, sizeof(kind), "k%lld", (long long)i);
    n00b_json_node_t *rec = n00b_json_object_new();
    n00b_json_object_put(rec, "id", n00b_json_int_new(i));
    n00b_json_object_put(rec, "kind", n00b_json_string_new(kind));
    n00b_json_object_put(rec, "host", n00b_json_string_new("h1"));
    return rec;
}

static void
check_capped_seal(n00b_store_t *store)
{
    auto entry_r = n00b_store_seal_hot_shard(store, .seal_ts = 1000);
    CHECK(n00b_result_is_ok(entry_r));
    n00b_store_catalog_entry_t *entry = n00b_result_get(entry_r);
    CHECK(entry != nullptr);
    CHECK(entry_may_contain(entry, r"kind", "zzz"));
    CHECK(entry_may_contain(entry, r"kind", "k0"));
    CHECK(!entry_may_contain(entry, r"host", "zzz"));
    CHECK(entry_may_contain(entry, r"host", "h1"));
}

// A hang detector, bounded far past what a slow box needs; a passing run
// returns as soon as the records land.
static void
wait_for_live_records(n00b_store_t *store, uint64_t expected)
{
    for (uint32_t i = 0; i < 12000; i++) {
        auto stats_r = n00b_store_memory_stats(store);
        CHECK(n00b_result_is_ok(stats_r));
        if (n00b_result_get(stats_r).hot_live_index >= expected) {
            return;
        }
        usleep(10000);
    }
    CHECK(!"service ingest did not publish the records");
}

#define TEST_WATERMARK UINT64_C(1234567)

// The service entry point forwards both knobs to the store it opens.
static void
test_open_service_forwards_knobs(void)
{
    auto profile_r = n00b_store_service_profile_new(
        .ingest_worker_count = 1,
        .seal_worker_count   = 1,
        .ingest_queue_bound  = 4,
        .ingest_backpressure = N00B_STORE_INGEST_BACKPRESSURE_BLOCK);
    CHECK(n00b_result_is_ok(profile_r));

    auto store_r = n00b_store_open_service(new_memory_vfs(),
                                           r"/rocs",
                                           term_schema(),
                                           n00b_result_get(profile_r),
                                           .schema_declared_since_ns = TEST_WATERMARK,
                                           .term_summary_max_bytes   = 8);
    CHECK(n00b_result_is_ok(store_r));
    n00b_store_t *store = n00b_result_get(store_r);

    auto watermark_r = n00b_store_schema_declared_since_ns(store);
    CHECK(n00b_result_is_ok(watermark_r));
    CHECK(n00b_result_get(watermark_r) == TEST_WATERMARK);

    for (int64_t i = 0; i < CAPPED_KINDS; i++) {
        auto payload_r = n00b_store_ingest_payload_record(capped_record(i));
        CHECK(n00b_result_is_ok(payload_r));
        auto submit_r = n00b_store_ingest_submit(store, n00b_result_get(payload_r));
        CHECK(n00b_result_is_ok(submit_r));
        CHECK(n00b_result_get(submit_r).state
              == N00B_STORE_INGEST_RECEIPT_ADMITTED_QUEUED);
    }
    wait_for_live_records(store, CAPPED_KINDS);
    check_capped_seal(store);
    close_store(store);
    n00b_eprintf("  [PASS] open_service_forwards_knobs\n");
}

// So does the config entry point, which carries both as keyword arguments;
// the config struct and its ROCS_* environment keys have neither.
static void
test_open_config_forwards_knobs(void)
{
    auto config_r = n00b_store_config_default(N00B_STORE_PROFILE_EMBEDDED_LOCAL);
    CHECK(n00b_result_is_ok(config_r));
    auto store_r = n00b_store_open_config(term_schema(),
                                          n00b_result_get(config_r),
                                          .schema_declared_since_ns = TEST_WATERMARK,
                                          .term_summary_max_bytes   = 8);
    CHECK(n00b_result_is_ok(store_r));
    n00b_store_t *store = n00b_result_get(store_r);

    auto watermark_r = n00b_store_schema_declared_since_ns(store);
    CHECK(n00b_result_is_ok(watermark_r));
    CHECK(n00b_result_get(watermark_r) == TEST_WATERMARK);

    for (int64_t i = 0; i < CAPPED_KINDS; i++) {
        CHECK(n00b_result_is_ok(n00b_store_ingest(store, capped_record(i))));
    }
    check_capped_seal(store);
    close_store(store);
    n00b_eprintf("  [PASS] open_config_forwards_knobs\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    fprintf(stderr, "test_rocs_term_summary:\n");
    test_bloom_no_false_negatives();
    test_bloom_false_positive_rate();
    test_bloom_golden_layout();
    test_bloom_size_boundaries();
    test_catalog_layout();
    test_absent_term_maps_no_shards();
    test_sync_seal_writes_summary();
    test_async_seal_writes_summary();
    test_v3_catalog_still_maps();
    test_v4_catalog_loads_as_filter();
    test_v5_round_trip();
    test_cap_omits_field_summary();
    test_summary_bytes_bounded();
    test_declared_empty_column_skips();
    test_parser_rejects_bad_summaries();
    test_plan_shapes_through_gate();
    test_fanout_skips_ruled_out_shards();
    test_empty_plan_skips_without_summary();
    test_normalized_values_agree();
    test_open_service_forwards_knobs();
    test_open_config_forwards_knobs();
    n00b_shutdown();
    return 0;
}
