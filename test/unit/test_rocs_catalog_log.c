/*
 * What a catalog change costs on disk, and what a crash leaves of it.
 *
 * Seals, drops, retention, and quarantine append to the catalog log, so the
 * bytes they write do not grow with the catalog. The log folds into a full
 * image on a cadence. These count the bytes the backend is asked to write,
 * through a wrapper over the memory backend, and the folds. Crashes are
 * simulated by failing every backend write from some point on, then opening
 * the store again on what reached the backend.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/runtime.h"
#include "util/assert.h"
#include "vfs/backend.h"
#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include <rocs/n00b_rocs.h>
#include <rocs/store.h>

#include "internal/rocs/store.h"
#include "test_check.h"

#ifndef N00B_DEBUG
#error "test_rocs_catalog_log requires N00B_DEBUG for the catalog write hook"
#endif

// Seals measured at each catalog size.
#define MEASURED 16

typedef struct {
    uint64_t image_bytes;
    uint64_t image_writes;
    uint64_t log_bytes;
    bool     crashed;
    // Faults, each cleared by new_counting_vfs.
    bool     fail_image_sync;
    bool     tear_log_write;
    bool     tore;
    bool     fail_shard_delete;
} counts_t;

static counts_t                     counts;
static const n00b_vfs_backend_ops_t *inner_ops;
static n00b_vfs_backend_ops_t        counting_ops;

static bool
ends_with(n00b_string_t *path, const char *suffix)
{
    size_t n = strlen(suffix);
    return path->u8_bytes >= n
        && memcmp(path->data + path->u8_bytes - n, suffix, n) == 0;
}

static bool
is_catalog_log(n00b_string_t *path)
{
    return ends_with(path, ".log") && strstr(path->data, "catalog.") != nullptr;
}

static void
count_write(n00b_string_t *path, n00b_buffer_t *data)
{
    if (ends_with(path, "catalog.rocs")) {
        counts.image_bytes += (uint64_t)n00b_buffer_len(data);
        counts.image_writes++;
    }
    else if (is_catalog_log(path)) {
        counts.log_bytes += (uint64_t)n00b_buffer_len(data);
    }
}

// Half of a log write reaches the backend and the write fails, once.
static bool
tear(n00b_string_t *path, n00b_buffer_t *data, n00b_buffer_t **half)
{
    if (!counts.tear_log_write || !is_catalog_log(path)) {
        return false;
    }
    counts.tear_log_write = false;
    counts.tore           = true;
    *half = n00b_buffer_from_bytes(data->data, n00b_buffer_len(data) / 2);
    return true;
}

static n00b_result_t(bool)
counting_put(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    if (counts.crashed) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    n00b_buffer_t *half = nullptr;
    if (tear(path, data, &half)) {
        (void)inner_ops->put(ctx, path, half);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    count_write(path, data);
    return inner_ops->put(ctx, path, data);
}

static n00b_result_t(bool)
counting_put_if_absent(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    if (counts.crashed) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    count_write(path, data);
    return inner_ops->put_if_absent(ctx, path, data);
}

static n00b_result_t(bool)
counting_append(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    if (counts.crashed) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    n00b_buffer_t *half = nullptr;
    if (tear(path, data, &half)) {
        (void)inner_ops->append(ctx, path, half);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    count_write(path, data);
    return inner_ops->append(ctx, path, data);
}

static n00b_result_t(bool)
counting_del(void *ctx, n00b_string_t *path)
{
    if (counts.crashed) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    if (counts.fail_shard_delete && strstr(path->data, "shards/") != nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    return inner_ops->del(ctx, path);
}

static n00b_result_t(bool)
counting_sync(void *ctx, n00b_string_t *path)
{
    if (counts.crashed) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    if (counts.fail_image_sync && ends_with(path, "catalog.rocs")) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    return inner_ops->sync(ctx, path);
}

// Syncs reach counting_sync, so a test can fail one.
static bool
counting_durable(void *ctx)
{
    (void)ctx;
    return true;
}

static n00b_vfs_t *
new_counting_vfs(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    auto be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    n00b_vfs_backend_t *be = n00b_result_get(be_r);

    inner_ops                  = be->ops;
    counting_ops               = *be->ops;
    counting_ops.put           = counting_put;
    counting_ops.put_if_absent = counting_put_if_absent;
    counting_ops.append        = counting_append;
    counting_ops.del           = counting_del;
    counting_ops.sync          = counting_sync;
    counting_ops.supports_durable_sync = counting_durable;
    be->ops                    = &counting_ops;

    auto mount_r = n00b_vfs_mount(vfs, r"/", be, 0);
    CHECK(n00b_result_is_ok(mount_r));
    counts = (counts_t){};
    return vfs;
}

static n00b_store_t *
open_store(n00b_vfs_t *vfs)
{
    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    auto store_r = n00b_store_open_vfs(vfs, r"/rocs", n00b_result_get(schema_r));
    CHECK(n00b_result_is_ok(store_r));
    return n00b_result_get(store_r);
}

static uint64_t seal_ts = 1;

static void
seal_one(n00b_store_t *store)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put(record, "id", n00b_json_int_new((int64_t)seal_ts));
    CHECK(n00b_result_is_ok(n00b_store_ingest(store, record)));
    auto seal_r = n00b_store_seal_hot_shard(store, .seal_ts = seal_ts++);
    CHECK(n00b_result_is_ok(seal_r));
}

static int
u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// The catalog bytes a seal writes, as the median over MEASURED seals at a
// catalog of `sealed` shards. A median, because the fold that the cadence
// spreads over many seals lands on one of them.
static uint64_t
bytes_per_seal_at(n00b_store_t *store, uint64_t *sealed, uint64_t target)
{
    while (*sealed < target) {
        seal_one(store);
        (*sealed)++;
    }

    uint64_t each[MEASURED];
    for (int i = 0; i < MEASURED; i++) {
        uint64_t before = counts.image_bytes + counts.log_bytes;
        seal_one(store);
        (*sealed)++;
        each[i] = counts.image_bytes + counts.log_bytes - before;
    }
    qsort(each, MEASURED, sizeof(uint64_t), u64_cmp);
    return each[MEASURED / 2];
}

// A seal writes the same bytes at 40, 150, and 600 shards, and the image is
// written at most once per cadence's worth of seals.
static void
test_bytes_per_seal_flat_in_catalog_size(void)
{
    n00b_vfs_t   *vfs    = new_counting_vfs();
    n00b_store_t *store  = open_store(vfs);
    uint64_t      sealed = 0;
    uint64_t      writes = counts.image_writes;

    const uint64_t sizes[3] = {40, 150, 600};
    uint64_t       bytes[3];
    for (int i = 0; i < 3; i++) {
        bytes[i] = bytes_per_seal_at(store, &sealed, sizes[i]);
        n00b_printf("  «#» sealed shards: «#» catalog bytes per seal",
                    (int64_t)sizes[i],
                    (int64_t)bytes[i]);
    }
    writes = counts.image_writes - writes;
    n00b_printf("  «#» seals: «#» image writes, «#» image bytes, «#» log bytes",
                (int64_t)sealed,
                (int64_t)writes,
                (int64_t)counts.image_bytes,
                (int64_t)counts.log_bytes);

    uint64_t lo = bytes[0];
    uint64_t hi = bytes[0];
    for (int i = 1; i < 3; i++) {
        lo = bytes[i] < lo ? bytes[i] : lo;
        hi = bytes[i] > hi ? bytes[i] : hi;
    }
    // Records differ only in the digits of their ids and timestamps.
    CHECK(hi - lo <= 16);
    // One image when the first seal finds none, then one per cadence.
    CHECK(writes <= 1 + sealed / N00B_STORE_CATALOG_FOLD_MIN_RECORDS);
    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    n00b_printf("  [PASS] bytes per seal are flat in catalog size");
}

typedef struct {
    uint64_t id;
    uint64_t records;
    uint64_t bytes;
    uint64_t seal_ts;
} snap_t;

typedef struct {
    snap_t   at[256];
    uint64_t len;
} catalog_snap_t;

static void
snapshot(n00b_store_t *store, catalog_snap_t *out)
{
    auto n_r = n00b_store_catalog_visible_entry_count(store);
    CHECK(n00b_result_is_ok(n_r));
    out->len = n00b_result_get(n_r);
    CHECK(out->len <= 256);
    for (uint64_t i = 0; i < out->len; i++) {
        auto e_r = n00b_store_catalog_visible_entry_at(store, i);
        CHECK(n00b_result_is_ok(e_r));
        auto opt = n00b_result_get(e_r);
        CHECK(n00b_option_is_set(opt));
        n00b_store_catalog_entry_t *e = n00b_option_get(opt);
        out->at[i] = (snap_t){
            .id      = n00b_result_get(n00b_store_catalog_entry_get_shard_id(e)),
            .records = n00b_result_get(n00b_store_catalog_entry_get_record_count(e)),
            .bytes   = n00b_result_get(n00b_store_catalog_entry_get_byte_len(e)),
            .seal_ts = n00b_result_get(n00b_store_catalog_entry_get_seal_ts(e)),
        };
    }
}

static bool
snap_equal(catalog_snap_t *a, catalog_snap_t *b)
{
    return a->len == b->len
        && memcmp(a->at, b->at, a->len * sizeof(snap_t)) == 0;
}

typedef struct {
    n00b_store_catalog_write_site_t site;
    bool                            armed;
    bool                            fired;
} crash_at_t;

static void
crash_hook(n00b_store_t                   *store,
           n00b_store_catalog_write_site_t site,
           void                           *ctx)
{
    (void)store;
    crash_at_t *at = ctx;
    if (at->armed && !at->fired && site == at->site) {
        at->fired      = true;
        counts.crashed = true;
    }
}

static bool
vfs_has(n00b_vfs_t *vfs, n00b_string_t *path)
{
    return n00b_result_is_ok(n00b_vfs_stat(vfs, path));
}

static n00b_buffer_t *
read_object(n00b_vfs_t *vfs, n00b_string_t *path)
{
    auto open_r = n00b_vfs_open(vfs, path, N00B_VFS_O_R);
    CHECK(n00b_result_is_ok(open_r));
    auto read_r = n00b_vfs_read(vfs, n00b_result_get(open_r), UINT64_C(1) << 20);
    CHECK(n00b_result_is_ok(read_r));
    CHECK(n00b_result_is_ok(n00b_vfs_close(vfs, n00b_result_get(open_r))));
    return n00b_result_get(read_r);
}

static void
write_object(n00b_vfs_t *vfs, n00b_string_t *path, n00b_buffer_t *data)
{
    auto open_r = n00b_vfs_open(vfs, path, N00B_VFS_O_W);
    CHECK(n00b_result_is_ok(open_r));
    CHECK(n00b_result_is_ok(n00b_vfs_write(vfs, n00b_result_get(open_r), data)));
    CHECK(n00b_result_is_ok(n00b_vfs_close(vfs, n00b_result_get(open_r))));
}

// Seal and drop until the hook's site is reached, fail every write from
// there on, and open the store again on what the backend holds. The reopened
// catalog lists exactly what the crashed one did: every change it made was
// durable before it was published, and nothing after the crash was.
static void
crash_and_reopen(n00b_store_catalog_write_site_t site, const char *name)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    crash_at_t    at    = {.site = site};
    n00b_store_catalog_write_hook_set(crash_hook, &at);

    for (int i = 0; i < 4; i++) {
        seal_one(store);
    }

    // Armed only around seals: a drop that hits the crash reinstates its
    // shard in memory after the durable catalog dropped it, which is the
    // drop path's own failure handling and not what this checks.
    uint64_t sealed = 4;
    while (!at.fired && sealed < 200) {
        at.armed = true;
        seal_one(store);
        at.armed = false;
        sealed++;
        // Drops between seals, so the log replays tombstones too.
        if (!at.fired && sealed % 5 == 0) {
            auto first_r = n00b_store_catalog_visible_entry_at(store, 0);
            CHECK(n00b_result_is_ok(first_r));
            n00b_store_catalog_entry_t *first =
                n00b_option_get(n00b_result_get(first_r));
            CHECK(n00b_result_is_ok(n00b_store_drop_sealed_shard(
                store,
                n00b_result_get(n00b_store_catalog_entry_get_shard_id(first)))));
        }
    }
    CHECK(at.fired);
    n00b_store_catalog_write_hook_set(nullptr, nullptr);

    catalog_snap_t before;
    snapshot(store, &before);

    // What the crash left on the backend.
    bool image   = vfs_has(vfs, r"/rocs/catalog.rocs");
    bool log_one = vfs_has(vfs, r"/rocs/catalog.1.log");
    bool log_two = vfs_has(vfs, r"/rocs/catalog.2.log");

    counts.crashed        = false;
    n00b_store_t *reopened = open_store(vfs);
    catalog_snap_t after;
    snapshot(reopened, &after);

    n00b_printf("  «#»: crashed after «#» seals with «#» visible; image «#», "
                "logs for epochs 1 and 2: «#» «#»; reopened with «#» visible",
                n00b_string_from_cstr((char *)name),
                (int64_t)sealed,
                (int64_t)before.len,
                (int64_t)image,
                (int64_t)log_one,
                (int64_t)log_two,
                (int64_t)after.len);
    CHECK(image);
    CHECK(log_one);
    CHECK(!log_two);
    CHECK(snap_equal(&before, &after));
    CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
}

// The append that reached the cadence is durable, and the fold it triggers
// never writes: the old image and the whole log are what reopen reads.
static void
test_crash_between_append_and_fold(void)
{
    crash_and_reopen(N00B_STORE_CATALOG_FOLD_BEGIN, "append, then crash");
    n00b_printf("  [PASS] crash between append and fold reopens exact");
}

// The fold's image is durable and nothing after it ran. The image names the
// next epoch, so reopen ignores the log the fold absorbed.
static void
test_crash_between_fold_and_log_delete(void)
{
    crash_and_reopen(N00B_STORE_CATALOG_FOLD_WRITTEN, "fold, then crash");
    n00b_printf("  [PASS] crash between fold and log delete reopens exact");
}

// Every visible shard's image is still on the backend.
static void
check_objects_present(n00b_vfs_t *vfs, n00b_store_t *store)
{
    auto n_r = n00b_store_catalog_visible_entry_count(store);
    CHECK(n00b_result_is_ok(n_r));
    for (uint64_t i = 0; i < n00b_result_get(n_r); i++) {
        n00b_store_catalog_entry_t *e = n00b_option_get(
            n00b_result_get(n00b_store_catalog_visible_entry_at(store, i)));
        n00b_string_t *path =
            n00b_result_get(n00b_store_catalog_entry_get_object_path(e));
        if (!vfs_has(vfs, path)) {
            n00b_printf("  visible shard «#» has no image at «#»",
                        (int64_t)n00b_result_get(
                            n00b_store_catalog_entry_get_shard_id(e)),
                        path);
            CHECK(false);
        }
    }
}

static uint64_t
first_visible_id(n00b_store_t *store)
{
    auto first_r = n00b_store_catalog_visible_entry_at(store, 0);
    CHECK(n00b_result_is_ok(first_r));
    return n00b_result_get(n00b_store_catalog_entry_get_shard_id(
        n00b_option_get(n00b_result_get(first_r))));
}

static uint64_t
visible_count(n00b_store_t *store)
{
    auto n_r = n00b_store_catalog_visible_entry_count(store);
    CHECK(n00b_result_is_ok(n_r));
    return n00b_result_get(n_r);
}

static n00b_result_t(bool)
seal_try(n00b_store_t *store)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put(record, "id", n00b_json_int_new((int64_t)seal_ts));
    CHECK(n00b_result_is_ok(n00b_store_ingest(store, record)));
    auto seal_r = n00b_store_seal_hot_shard(store, .seal_ts = seal_ts++);
    return n00b_result_is_ok(seal_r) ? n00b_result_ok(bool, true)
                                     : n00b_result_err(bool,
                                                       n00b_result_get_err(seal_r));
}

static n00b_result_t(bool)
drop_oldest(n00b_store_t *store)
{
    return n00b_store_drop_sealed_shard(store, first_visible_id(store));
}

// One retention pass that drops the two oldest shards.
static n00b_result_t(bool)
retain_two_fewer(n00b_store_t *store)
{
    auto policy_r = n00b_store_shard_retention_policy_new(
        .max_sealed_shards = visible_count(store) - 2);
    CHECK(n00b_result_is_ok(policy_r));
    auto pass_r = n00b_store_apply_shard_retention(store,
                                                   n00b_result_get(policy_r));
    return n00b_result_is_ok(pass_r) ? n00b_result_ok(bool, true)
                                     : n00b_result_err(bool,
                                                       n00b_result_get_err(pass_r));
}

static n00b_result_t(bool)
quarantine_oldest(n00b_store_t *store)
{
    auto q_r = n00b_store_quarantine_shard(store, first_visible_id(store));
    return n00b_result_is_ok(q_r) ? n00b_result_ok(bool, true)
                                  : n00b_result_err(bool,
                                                    n00b_result_get_err(q_r));
}

static n00b_result_t(bool)
flush_idle(n00b_store_t *store)
{
    return n00b_store_flush(store);
}

typedef struct {
    const char *name;
    n00b_result_t(bool) (*op)(n00b_store_t *);
} mutator_t;

static const mutator_t mutators[] = {
    {"seal", seal_try},
    {"drop", drop_oldest},
    {"retention", retain_two_fewer},
    {"quarantine", quarantine_oldest},
    {"idle flush", flush_idle},
};

#define NMUTATORS (sizeof(mutators) / sizeof(mutators[0]))

// The median catalog bytes one call of `op` writes on a store of `sealed`
// shards.
static uint64_t
mutator_bytes_at(const mutator_t *m, uint64_t sealed)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (uint64_t i = 0; i < sealed + 2 * MEASURED; i++) {
        seal_one(store);
    }
    uint64_t each[MEASURED];
    for (int i = 0; i < MEASURED; i++) {
        uint64_t before = counts.image_bytes + counts.log_bytes;
        CHECK(n00b_result_is_ok(m->op(store)));
        each[i] = counts.image_bytes + counts.log_bytes - before;
    }
    qsort(each, MEASURED, sizeof(uint64_t), u64_cmp);
    CHECK(n00b_result_is_ok(n00b_store_close(store)));
    return each[MEASURED / 2];
}

// Every catalog mutator writes the same bytes at 40 and 300 shards: none of
// them rewrites the image per call.
static void
test_every_mutator_writes_flat_bytes(void)
{
    bool ok = true;
    for (size_t i = 0; i < NMUTATORS; i++) {
        uint64_t small = mutator_bytes_at(&mutators[i], 40);
        uint64_t large = mutator_bytes_at(&mutators[i], 300);
        uint64_t diff  = large > small ? large - small : small - large;
        n00b_printf("  «#»: «#» bytes at 40 shards, «#» at 300",
                    n00b_string_from_cstr((char *)mutators[i].name),
                    (int64_t)small,
                    (int64_t)large);
        ok = ok && diff <= 32;
    }
    CHECK(ok);
    n00b_printf("  [PASS] every mutator writes flat bytes");
}

// Open the store again on what the backend holds and compare it with what
// the abandoned store published.
static n00b_store_t *
crash_reopen_same(n00b_vfs_t *vfs, n00b_store_t *store, const char *name)
{
    catalog_snap_t before;
    snapshot(store, &before);
    counts.crashed           = false;
    counts.fail_image_sync   = false;
    counts.fail_shard_delete = false;
    counts.tear_log_write    = false;

    n00b_store_t  *reopened = open_store(vfs);
    catalog_snap_t after;
    snapshot(reopened, &after);
    n00b_printf("  «#»: «#» visible before, «#» after reopen, degraded «#»",
                n00b_string_from_cstr((char *)name),
                (int64_t)before.len,
                (int64_t)after.len,
                (int64_t)n00b_store_opened_degraded(reopened));
    check_objects_present(vfs, reopened);
    CHECK(snap_equal(&before, &after));
    return reopened;
}

// A rebuilt catalog takes an epoch whose log an earlier image left behind.
// That log belongs to the earlier image, so nothing replays it and the next
// change does not land after it.
static void
test_rebuild_ignores_stale_log(void)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (int i = 0; i < 3; i++) {
        seal_one(store);
    }
    CHECK(vfs_has(vfs, r"/rocs/catalog.1.log"));

    // The store is abandoned and its image damaged, so the next open
    // rebuilds from the shard images.
    n00b_buffer_t *image = read_object(vfs, r"/rocs/catalog.rocs");
    n00b_buffer_t *bad   = n00b_buffer_from_bytes(image->data,
                                                  n00b_buffer_len(image));
    bad->data[0] ^= 0x5a;
    write_object(vfs, r"/rocs/catalog.rocs", bad);

    n00b_store_t *rebuilt = open_store(vfs);
    CHECK(n00b_store_opened_degraded(rebuilt));
    seal_one(rebuilt);
    CHECK(n00b_result_is_ok(drop_oldest(rebuilt)));

    n00b_store_t *reopened = crash_reopen_same(vfs, rebuilt, "rebuilt");
    CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
    n00b_printf("  [PASS] a rebuilt catalog ignores a stale log");
}

// A fold whose image lands but whose sync fails leaves the durable image
// naming an epoch the owner is not appending to. The next change folds
// instead of appending, so a crash during it loses nothing published.
static void
test_failed_fold_forces_next_fold(void)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (int i = 0; i < 8; i++) {
        seal_one(store);
    }
    CHECK(counts.image_writes == 1);

    counts.fail_image_sync = true;
    while (counts.image_writes == 1) {
        seal_one(store);
    }
    counts.fail_image_sync = false;

    crash_at_t at = {.site = N00B_STORE_CATALOG_FOLD_BEGIN, .armed = true};
    n00b_store_catalog_write_hook_set(crash_hook, &at);
    (void)drop_oldest(store);
    n00b_store_catalog_write_hook_set(nullptr, nullptr);
    CHECK(at.fired);

    n00b_store_t *reopened = crash_reopen_same(vfs, store, "failed fold");
    CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
    n00b_printf("  [PASS] a failed fold makes the next change fold");
}

// A damaged frame with more of the log after it is corruption, not a torn
// tail, so the store opens degraded instead of silently dropping the frames
// after it.
static void
test_corrupt_mid_log_frame_opens_degraded(void)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (int i = 0; i < 6; i++) {
        seal_one(store);
    }
    CHECK(n00b_result_is_ok(drop_oldest(store)));
    CHECK(n00b_result_is_ok(drop_oldest(store)));

    // One byte inside the first frame's payload.
    n00b_buffer_t *log = read_object(vfs, r"/rocs/catalog.1.log");
    n00b_buffer_t *bad = n00b_buffer_from_bytes(log->data, n00b_buffer_len(log));
    bad->data[48] ^= 0x5a;
    write_object(vfs, r"/rocs/catalog.1.log", bad);

    n00b_store_t *reopened = open_store(vfs);
    n00b_printf("  reopened degraded: «#»",
                (int64_t)n00b_store_opened_degraded(reopened));
    check_objects_present(vfs, reopened);
    CHECK(n00b_store_opened_degraded(reopened));
    CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
    n00b_printf("  [PASS] a corrupt frame mid-log opens degraded");
}

// Each mutator's log write tears: half of it reaches the backend and the
// call fails. The reopened catalog is the one published, and the next
// change folds rather than appending after the torn bytes.
static void
test_torn_log_write_changes_nothing(void)
{
    for (size_t i = 0; i < NMUTATORS; i++) {
        const mutator_t *m = &mutators[i];
        if (m->op == flush_idle) {
            continue;
        }
        n00b_vfs_t   *vfs   = new_counting_vfs();
        n00b_store_t *store = open_store(vfs);
        for (int k = 0; k < 10; k++) {
            seal_one(store);
        }

        counts.tear_log_write = true;
        auto op_r             = m->op(store);
        CHECK(counts.tore);
        CHECK(n00b_result_is_err(op_r));

        n00b_store_t *reopened = crash_reopen_same(vfs,
                                                   store,
                                                   m->name);
        CHECK(!n00b_store_opened_degraded(reopened));
        uint64_t writes = counts.image_writes;
        seal_one(reopened);
        CHECK(counts.image_writes == writes + 1);
        CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
    }
    n00b_printf("  [PASS] a torn log write changes nothing on reopen");
}

// A drop whose image will not delete puts the shard back, and a crash then
// reopens with it listed.
static void
test_failed_image_delete_reinstates(void)
{
    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (int k = 0; k < 10; k++) {
        seal_one(store);
    }
    counts.fail_shard_delete = true;
    CHECK(n00b_result_is_err(drop_oldest(store)));
    CHECK(n00b_result_is_err(retain_two_fewer(store)));
    n00b_store_t *reopened = crash_reopen_same(vfs, store, "failed delete");
    CHECK(n00b_result_is_ok(n00b_store_close(reopened)));
    n00b_printf("  [PASS] a failed image delete reinstates durably");
}

// Replaying r log records over n catalog entries visits O(n + r) entries,
// counted as the extra visits an open with a log makes over one without.
static void
test_replay_work_linear(void)
{
    const uint64_t n = 400;
    const uint64_t r = 120;

    n00b_vfs_t   *vfs   = new_counting_vfs();
    n00b_store_t *store = open_store(vfs);
    for (uint64_t i = 0; i < n; i++) {
        seal_one(store);
    }
    CHECK(n00b_result_is_ok(n00b_store_close(store)));

    store           = open_store(vfs);
    uint64_t writes = counts.image_writes;
    for (uint64_t i = 0; i < r; i++) {
        seal_one(store);
    }
    // Under the fold cadence, so the reopen replays all r records.
    CHECK(counts.image_writes == writes);

    n00b_store_catalog_entries_visited_reset();
    n00b_store_t *with_log = open_store(vfs);
    uint64_t      replayed = n00b_store_catalog_entries_visited();
    CHECK(n00b_result_is_ok(n00b_store_close(with_log)));

    n00b_store_catalog_entries_visited_reset();
    n00b_store_t *folded   = open_store(vfs);
    uint64_t      baseline = n00b_store_catalog_entries_visited();
    CHECK(n00b_result_is_ok(n00b_store_close(folded)));

    uint64_t extra = replayed > baseline ? replayed - baseline : 0;
    n00b_printf("  «#» entries, «#» records: «#» visits with the log, «#» "
                "without",
                (int64_t)n,
                (int64_t)r,
                (int64_t)replayed,
                (int64_t)baseline);
    CHECK(extra <= 4 * (n + r));
    n00b_printf("  [PASS] replay work is linear in entries plus records");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    // An argument runs one case, so each can fail in its own run.
    int only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0 || only == 0) {
        test_bytes_per_seal_flat_in_catalog_size();
    }
    if (only < 0 || only == 1) {
        test_crash_between_append_and_fold();
    }
    if (only < 0 || only == 2) {
        test_crash_between_fold_and_log_delete();
    }
    if (only < 0 || only == 3) {
        test_every_mutator_writes_flat_bytes();
    }
    if (only < 0 || only == 4) {
        test_rebuild_ignores_stale_log();
    }
    if (only < 0 || only == 5) {
        test_failed_fold_forces_next_fold();
    }
    if (only < 0 || only == 6) {
        test_corrupt_mid_log_frame_opens_degraded();
    }
    if (only < 0 || only == 7) {
        test_torn_log_write_changes_nothing();
    }
    if (only < 0 || only == 8) {
        test_failed_image_delete_reinstates();
    }
    if (only < 0 || only == 9) {
        test_replay_work_linear();
    }

    n00b_shutdown();
    return 0;
}
