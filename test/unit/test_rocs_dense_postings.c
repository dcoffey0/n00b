/*
 * A hot posting-list reader racing the list's one writer through a growth.
 *
 * Posting lists take no lock. Their one writer appends and grows them, and a
 * reader loads the list's length, then its array, then reads elements below
 * that length. Growth copies into a larger array and publishes it, so the
 * window that matters is a reader holding the old array while the writer moves
 * past it: if growth freed that array, the shard's pool would zero it and hand
 * it out again, and the reader would read zeroes or someone else's data.
 *
 * Each case parks the reader in that window with the read hook, after it has
 * loaded a 64-element prefix, and lets the writer push 64..ROWS-1, which grows
 * the list several times over. Released, the reader has to report exactly the
 * prefix it loaded. Both representations, through both read paths: a lookup
 * (which copies the list) and a probe (which searches it in place).
 *
 * Sealing also rewrites a dense list's array: it trims the growth slack, which
 * frees the old array. The last case parks a reader that pinned the hot shard
 * the same way and closes the store, whose seal must wait for that reader.
 */

#include <stdint.h>
#include <stdlib.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "util/assert.h"

#include "vfs/backend_memory.h"
#include "vfs/vfs.h"

#include <rocs/n00b_rocs.h>
#include <rocs/store.h>

#include "internal/rocs/index.h"
#include "internal/rocs/store.h"
#include "rocs_test_support.h"
#include "test_check.h"

#ifndef N00B_DEBUG
#error "test_rocs_dense_postings requires N00B_DEBUG for the posting read hook"
#endif

#define PREFIX  UINT64_C(64)
#define ROWS    UINT64_C(8192)
#define HANG_NS (UINT64_C(60) * 1000 * 1000 * 1000)

typedef struct {
    n00b_store_shard_t       *shard;
    n00b_store_index_t       *index;
    n00b_json_node_t         *value;
    n00b_store_index_probe_t *probe;
    bool                      use_probe;
    _Atomic(bool)             armed;
    _Atomic(bool)             parked;
    _Atomic(bool)             go;
    // What the reader saw.
    uint64_t                  len;
    uint64_t                  mismatches;
    uint64_t                  probe_misses;
} race_t;

static void
park_reader(n00b_store_posting_list_t *postings, void *ctx)
{
    (void)postings;
    race_t *race     = ctx;
    bool    expected = true;
    if (!atomic_compare_exchange_strong(&race->armed, &expected, false)) {
        return;
    }
    atomic_store(&race->parked, true);
    uint64_t deadline = now_ns() + HANG_NS;
    while (!atomic_load(&race->go)) {
        __asm__ volatile("" ::: "memory");
        CHECK(now_ns() < deadline);
    }
}

static void *
race_reader(void *raw)
{
    race_t *race = raw;

    atomic_store(&race->armed, true);
    if (race->use_probe) {
        // The parked search is the first, for the prefix's last ordinal: a
        // search for it over a zeroed array finds nothing. The rest spread
        // over the prefix and run against whatever the list holds by then.
        for (uint64_t ord = PREFIX; ord-- > 0;) {
            auto has_r = n00b_store_index_probe_contains(race->probe, ord);
            CHECK(n00b_result_is_ok(has_r));
            if (!n00b_result_get(has_r)) {
                race->probe_misses++;
            }
        }
        race->len = PREFIX;
        return nullptr;
    }

    auto found_r = n00b_store_index_lookup(race->index, race->shard, race->value);
    CHECK(n00b_result_is_ok(found_r));
    n00b_store_postings_t *postings = n00b_result_get(found_r);
    auto                   len_r    = n00b_store_postings_len(postings);
    CHECK(n00b_result_is_ok(len_r));
    race->len = n00b_result_get(len_r);
    for (uint64_t i = 0; i < race->len; i++) {
        auto posting_r = n00b_store_postings_get(postings, i);
        CHECK(n00b_result_is_ok(posting_r));
        n00b_option_t(n00b_store_posting_t) opt = n00b_result_get(posting_r);
        if (!n00b_option_is_set(opt) || n00b_option_get(opt).pos.ordinal != i) {
            race->mismatches++;
        }
    }
    return nullptr;
}

static void
run_case(n00b_store_postings_kind_t kind, bool use_probe, n00b_string_t *name)
{
    auto shard_r = n00b_store_shard_new(.shard_id  = UINT64_C(0xDE05),
                                        .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));

    race_t race = {
        .shard     = n00b_result_get(shard_r),
        .value     = n00b_json_string_new_from_n00b(r"log"),
        .use_probe = use_probe,
    };
    auto index_r = n00b_store_index_new(r"kind",
                                        N00B_STORE_INDEX_TERM,
                                        .postings = kind);
    CHECK(n00b_result_is_ok(index_r));
    race.index = n00b_result_get(index_r);

    for (uint64_t i = 0; i < ROWS; i++) {
        n00b_json_node_t *rec = n00b_json_object_new();
        n00b_json_object_put_n00b(rec,
                                  r"kind",
                                  n00b_json_string_new_from_n00b(r"log"));
        CHECK(n00b_result_is_ok(n00b_store_shard_append(race.shard, rec)));
    }
    for (uint64_t ord = 0; ord < PREFIX; ord++) {
        CHECK(n00b_result_is_ok(
            n00b_store_index_add(race.index, race.shard, ord)));
    }
    if (use_probe) {
        auto probe_r = n00b_store_index_probe_hot(race.index,
                                                  race.shard,
                                                  race.value);
        CHECK(n00b_result_is_ok(probe_r));
        race.probe = n00b_result_get(probe_r);
    }

    rocs_posting_read_hook_set(park_reader, &race);
    auto t_r = n00b_thread_spawn(race_reader, &race);
    CHECK(n00b_result_is_ok(t_r));

    uint64_t deadline = now_ns() + HANG_NS;
    while (!atomic_load(&race.parked)) {
        __asm__ volatile("" ::: "memory");
        CHECK(now_ns() < deadline);
    }

    // The reader holds a 64-element view. Grow the list well past it.
    for (uint64_t ord = PREFIX; ord < ROWS; ord++) {
        CHECK(n00b_result_is_ok(
            n00b_store_index_add(race.index, race.shard, ord)));
    }
    atomic_store(&race.go, true);
    n00b_thread_join(n00b_result_get(t_r));
    rocs_posting_read_hook_set(nullptr, nullptr);

    n00b_printf("  «#»: reader saw «#» ordinals, «#» wrong, «#» probes missed",
                name,
                (int64_t)race.len,
                (int64_t)race.mismatches,
                (int64_t)race.probe_misses);
    CHECK(race.len == PREFIX);
    CHECK(race.mismatches == 0);
    CHECK(race.probe_misses == 0);

    // And every push the writer made landed.
    auto all_r = n00b_store_index_lookup(race.index, race.shard, race.value);
    CHECK(n00b_result_is_ok(all_r));
    auto all_len_r = n00b_store_postings_len(n00b_result_get(all_r));
    CHECK(n00b_result_is_ok(all_len_r));
    CHECK(n00b_result_get(all_len_r) == ROWS);
}

typedef struct {
    n00b_store_t           *store;
    _Atomic(bool)           armed;
    _Atomic(bool)           parked;
    _Atomic(bool)           go;
    // The dense list the reader probes, and the array it held when it parked.
    n00b_flagset_t         *flags;
    uint64_t               *words;
    bool                    found;
} close_race_t;

static void
park_close_reader(n00b_store_posting_list_t *postings, void *ctx)
{
    (void)postings;
    close_race_t *race     = ctx;
    bool          expected = true;
    if (!atomic_compare_exchange_strong(&race->armed, &expected, false)) {
        return;
    }
    atomic_store(&race->parked, true);
    uint64_t deadline = now_ns() + HANG_NS;
    while (!atomic_load(&race->go)) {
        __asm__ volatile("" ::: "memory");
        CHECK(now_ns() < deadline);
    }
}

static void *
close_race_reader(void *raw)
{
    close_race_t *race   = raw;
    auto          snap_r = n00b_store_tail_snapshot(race->store);
    CHECK(n00b_result_is_ok(snap_r));
    auto hot_r = n00b_store_hot_shard_acquire(race->store,
                                              n00b_result_get(snap_r).hot_through);
    CHECK(n00b_result_is_ok(hot_r));
    CHECK(n00b_option_is_set(n00b_result_get(hot_r)));
    n00b_store_shard_t *hot = n00b_option_get(n00b_result_get(hot_r));

    auto probe_r = n00b_store_index_probe_hot(
        index_of(r"kind", N00B_STORE_INDEX_TERM),
        hot,
        n00b_json_string_new_from_n00b(r"log"));
    CHECK(n00b_result_is_ok(probe_r));
    n00b_dict_foreach(hot->columns, field, column, {
        (void)field;
        n00b_dict_foreach(column, key, postings, {
            (void)key;
            if (postings->kind == N00B_STORE_POSTINGS_DENSE) {
                race->flags = postings->flags;
                race->words = __atomic_load_n(&postings->flags->contents,
                                              __ATOMIC_ACQUIRE);
            }
        });
    });
    CHECK(race->flags != nullptr);

    atomic_store(&race->armed, true);
    auto has_r = n00b_store_index_probe_contains(n00b_result_get(probe_r), 5);
    CHECK(n00b_result_is_ok(has_r));
    race->found = n00b_result_get(has_r);
    n00b_store_hot_shard_release(race->store);
    return nullptr;
}

static void *
close_store(void *raw)
{
    close_race_t *race = raw;
    CHECK(n00b_result_is_ok(n00b_store_close(race->store)));
    return nullptr;
}

// A reader pins the hot shard and parks inside a probe of its dense list,
// holding the list's array. Closing the store seals the hot shard, and the
// seal trims that list into a smaller array and frees the old one. The close
// has to wait for the reader: once it blocks, the reader's array must still be
// the list's, and the reader must still find what it was looking for.
static void
test_close_waits_for_pinned_dense_reader(void)
{
    auto vfs_r = n00b_vfs_new();
    CHECK(n00b_result_is_ok(vfs_r));
    n00b_vfs_t *vfs  = n00b_result_get(vfs_r);
    auto        be_r = n00b_vfs_backend_memory_new();
    CHECK(n00b_result_is_ok(be_r));
    CHECK(n00b_result_is_ok(
        n00b_vfs_mount(vfs, r"/", n00b_result_get(be_r), 0)));
    auto schema_r = n00b_store_schema_new();
    CHECK(n00b_result_is_ok(schema_r));
    n00b_store_schema_t *schema = n00b_result_get(schema_r);
    CHECK(n00b_result_is_ok(
        n00b_store_schema_add_field(schema,
                                    r"kind",
                                    .index_kind = N00B_STORE_INDEX_TERM,
                                    .postings   = N00B_STORE_POSTINGS_DENSE)));
    auto store_r = n00b_store_open_vfs(vfs, r"/rocs", schema);
    CHECK(n00b_result_is_ok(store_r));

    close_race_t race = {.store = n00b_result_get(store_r)};
    // Bits 0..128 grow the list's array to 4 words with 3 in use, so the
    // seal's trim moves it to a 3-word array and frees this one.
    for (int i = 0; i < 129; i++) {
        n00b_json_node_t *rec = n00b_json_object_new();
        n00b_json_object_put_n00b(rec,
                                  r"kind",
                                  n00b_json_string_new_from_n00b(r"log"));
        CHECK(n00b_result_is_ok(n00b_store_ingest(race.store, rec)));
    }

    rocs_posting_read_hook_set(park_close_reader, &race);
    auto reader_r = n00b_thread_spawn(close_race_reader, &race);
    CHECK(n00b_result_is_ok(reader_r));
    uint64_t deadline = now_ns() + HANG_NS;
    while (!atomic_load(&race.parked)) {
        __asm__ volatile("" ::: "memory");
        CHECK(now_ns() < deadline);
    }

    auto closer_r = n00b_thread_spawn(close_store, &race);
    CHECK(n00b_result_is_ok(closer_r));
    n00b_thread_t *closer = n00b_result_get(closer_r);
    while (!n00b_store_thread_blocked(race.store, closer)) {
        __asm__ volatile("" ::: "memory");
        CHECK(now_ns() < deadline);
    }
    bool kept = __atomic_load_n(&race.flags->contents, __ATOMIC_ACQUIRE)
             == race.words;

    atomic_store(&race.go, true);
    n00b_thread_join(n00b_result_get(reader_r));
    n00b_thread_join(closer);
    rocs_posting_read_hook_set(nullptr, nullptr);

    n00b_printf("  close with a parked dense reader: array kept «#», "
                "ordinal 5 found «#»",
                (int64_t)kept,
                (int64_t)race.found);
    CHECK(kept);
    CHECK(race.found);
    n00b_printf("  [PASS] close waits for a pinned dense reader");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    // An argument runs one case, so each can fail in its own run.
    int only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0 || only == 0) {
        run_case(N00B_STORE_POSTINGS_DENSE, false, r"dense lookup");
    }
    if (only < 0 || only == 1) {
        run_case(N00B_STORE_POSTINGS_DENSE, true, r"dense probe");
    }
    if (only < 0 || only == 2) {
        run_case(N00B_STORE_POSTINGS_SPARSE, false, r"sparse lookup");
    }
    if (only < 0 || only == 3) {
        run_case(N00B_STORE_POSTINGS_SPARSE, true, r"sparse probe");
    }
    if (only < 0 || only == 4) {
        test_close_waits_for_pinned_dense_reader();
    }
    n00b_printf("  [PASS] a reader parked across growth sees its prefix intact");

    n00b_shutdown();
    return 0;
}
