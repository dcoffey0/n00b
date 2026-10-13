/* test/unit/test_alloc_hdr_stw_race.c - an allocation header used across a
 * collection.
 *
 * n00b_find_alloc_info returns an OOB header that lives in its allocator's
 * metadata pool, and a collection of a movable arena destroys that pool. Stops
 * are preemptive, so a caller that drops the STW gate between the lookup and
 * its use of the header can be stopped in between, and the use then reads or
 * writes unmapped memory. test_rocs_async_seal_stress hit this on CI as a
 * SIGSEGV in the debug alloc-site write on every new string.
 *
 * Each case parks a worker in that window through n00b_alloc_hdr_lookup_hook
 * and collects the default arena twice from main: the first collection
 * retires the metadata pool and the next stop reclaims it.
 *
 *   * Unpinned, both collections run while the worker waits, and the worker
 *     then uses a header from a reclaimed pool.
 *   * Pinned, the first collection queues behind the pin. The hook sees the
 *     queued writer and returns, so the worker finishes with a valid header
 *     and the collection runs after it unpins.
 *
 * Every wait is on lock or flag state. The polling bounds are hang detectors.
 *
 * The guard case re-runs this binary to show that an unpinned lookup of such
 * a header aborts in a debug build, which is what catches the next caller
 * that forgets the pin.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/gc.h"
#include "core/hash.h"
#include "core/runtime.h"
#include "core/rwlock.h"
#include "core/thread.h"
#include "core/type_info.h"
#include "conduit/print.h"
#include "util/assert.h"

#include "test_check.h"

#if defined(N00B_DEBUG)

typedef enum {
    RACE_STRING,
    RACE_TYPEHASH,
    RACE_HASH,
} race_kind_t;

static n00b_thread_t  *g_target           = nullptr;
static _Atomic int64_t g_armed            = 0;
static _Atomic int64_t g_in_window        = 0;
static _Atomic int64_t g_pinned_in_window = 0;
static _Atomic int64_t g_collected        = 0;
static _Atomic int64_t g_op_done          = 0;

static void
lookup_hook(void *addr)
{
    (void)addr;

    if (n00b_thread_self() != g_target) {
        return;
    }
    int64_t armed = 1;
    if (!n00b_cas(&g_armed, &armed, 0)) {
        return;
    }

    n00b_rwlock_t *gate   = &n00b_get_runtime()->critical_execution;
    bool           pinned = n00b_rw_held_by_self(gate);

    n00b_atomic_store(&g_pinned_in_window, pinned ? 1 : 0);
    n00b_atomic_store(&g_in_window, 1);

    while (!n00b_atomic_load(&g_collected)) {
        if (pinned && (n00b_atomic_load(&gate->futex) & N00B_RW_W_LOCK)) {
            break;
        }
        sched_yield();
    }
}

static void
check_site_recorded(n00b_string_t *s)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(s, .pin = &pin);

    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->file_name != nullptr);
    CHECK(strstr(info.hdr.oob->file_name, "test_alloc_hdr_stw_race") != nullptr);
    n00b_alloc_hdr_unpin(pin);
}

static void *
race_worker(void *arg)
{
    race_kind_t kind = (race_kind_t)(intptr_t)arg;

    g_target = n00b_thread_self();

    if (kind == RACE_STRING) {
        n00b_atomic_store(&g_armed, 1);
        n00b_string_t *s = n00b_string_from_raw("seal", 4);
        n00b_atomic_store(&g_armed, 0);
        CHECK(s->u8_bytes == 4);
        check_site_recorded(s);
    }
    else if (kind == RACE_TYPEHASH) {
        n00b_string_t *s = n00b_string_from_raw("seal", 4);
        n00b_atomic_store(&g_armed, 1);
        uint64_t th = n00b_obj_typehash(s);
        n00b_atomic_store(&g_armed, 0);
        CHECK(th == typehash(n00b_string_t *));
    }
    else {
        n00b_string_t *s = n00b_string_from_raw("seal", 4);
        n00b_atomic_store(&g_armed, 1);
        n00b_uint128_t hv = n00b_hash(s, nullptr);
        n00b_atomic_store(&g_armed, 0);
        CHECK(hv != 0);
        CHECK(n00b_hash(s, nullptr) == hv);
    }

    n00b_atomic_store(&g_op_done, 1);
    return nullptr;
}

static void
run_race(race_kind_t kind, const char *name)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    n00b_atomic_store(&g_armed, 0);
    n00b_atomic_store(&g_in_window, 0);
    n00b_atomic_store(&g_pinned_in_window, 0);
    n00b_atomic_store(&g_collected, 0);
    n00b_atomic_store(&g_op_done, 0);
    n00b_alloc_hdr_lookup_hook = lookup_hook;

    auto r = n00b_thread_spawn(race_worker, (void *)(intptr_t)kind);
    CHECK(n00b_result_is_ok(r));
    n00b_thread_t *worker = n00b_result_get(r);

    for (int i = 0; i < 120000 && !n00b_atomic_load(&g_in_window)
                    && !n00b_atomic_load(&g_op_done);
         i++) {
        usleep(1000);
    }
    CHECK(n00b_atomic_load(&g_in_window) || n00b_atomic_load(&g_op_done));

    bool in_window = n00b_atomic_load(&g_in_window) != 0;
    if (in_window) {
        n00b_collect(rt->default_arena);
        n00b_collect(rt->default_arena);
    }
    n00b_atomic_store(&g_collected, 1);

    n00b_thread_join(worker);
    n00b_alloc_hdr_lookup_hook = nullptr;

    CHECK(n00b_atomic_load(&g_op_done) == 1);
    // A header looked up in the window must have been pinned. The string case
    // may not look one up at all, because the site rides the allocation.
    if (kind != RACE_STRING) {
        CHECK(in_window);
    }
    if (in_window) {
        CHECK(n00b_atomic_load(&g_pinned_in_window) == 1);
    }

    n00b_eprintf("  [PASS] [|#|] (in_window=[|#|])\n",
                 n00b_string_from_cstr(name),
                 (int64_t)in_window);
}

static int
guard_child(void)
{
    n00b_string_t    *s    = n00b_string_from_raw("seal", 4);
    n00b_alloc_info_t info = n00b_find_alloc_info(s);
    (void)info;
    n00b_shutdown();
    return 0;
}

static void
run_guard(const char *self)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        execl(self, self, "guard-child", (char *)nullptr);
        _exit(43);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    // The child exits 0 only if an unpinned lookup of a header that needs a
    // pin returned without the assertion firing.
    CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
    CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == 43));
    n00b_eprintf("  [PASS] guard\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    const char *only = argc > 1 ? argv[1] : nullptr;

    if (only != nullptr && strcmp(only, "guard-child") == 0) {
        return guard_child();
    }

    n00b_eprintf("test_alloc_hdr_stw_race:\n");
    if (only == nullptr || strcmp(only, "string") == 0) {
        run_race(RACE_STRING, "string");
    }
    if (only == nullptr || strcmp(only, "typehash") == 0) {
        run_race(RACE_TYPEHASH, "typehash");
    }
    if (only == nullptr || strcmp(only, "hash") == 0) {
        run_race(RACE_HASH, "hash");
    }
    if (only == nullptr || strcmp(only, "guard") == 0) {
        run_guard(argv[0]);
    }

    n00b_shutdown();
    return 0;
}

#else

int
main(void)
{
    return 0;
}

#endif
