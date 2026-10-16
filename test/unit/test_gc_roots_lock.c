/*
 * test_gc_roots_lock.c - The runtime's GC root list excludes concurrent
 * registrations.
 *
 * A condition variable outside the GC heap, such as a static one, registers
 * a GC root when it is initialized, on whichever thread initializes it. Two
 * threads each initialize their own. A hook holds the first inside the root
 * list update, and the second must then wait on the list's lock rather than
 * update the list alongside it.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/condition.h"
#include "core/futex.h"
#include "core/gc.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/time.h"

#define HANG_GUARD_MS 30000

static n00b_condition_t cv_a;
static n00b_condition_t cv_b;

static n00b_futex_t held   = 0;
static n00b_futex_t go     = 0;
static n00b_futex_t b_done = 0;

static _Atomic(n00b_thread_t *) thread_b;
static _Atomic(bool)            b_inside_with_a = false;

static bool
await_count(n00b_futex_t *c, uint32_t want, int64_t guard_ms)
{
    int64_t  deadline = n00b_ns_timestamp() + guard_ms * N00B_NS_PER_MS;
    uint32_t cur;
    while ((cur = n00b_atomic_load(c)) < want) {
        int64_t left = deadline - n00b_ns_timestamp();
        if (left <= 0) {
            return false;
        }
        (void)n00b_futex_wait(c, cur, (uint64_t)left);
    }
    return true;
}

static void
bump(n00b_futex_t *c)
{
    n00b_atomic_add(c, 1);
    n00b_futex_wake(c, true);
}

// Each condition variable registers its cv_param words as the root.
static void
hold_first_registration(void *addr)
{
    if (addr == (void *)&cv_a.cv_param) {
        bump(&held);
        assert(await_count(&go, 1, HANG_GUARD_MS));
    }
    else if (addr == (void *)&cv_b.cv_param && n00b_atomic_load(&go) == 0) {
        n00b_atomic_store(&b_inside_with_a, true);
    }
}

static void *
init_a(void *arg)
{
    (void)arg;
    n00b_condition_init(&cv_a);
    return nullptr;
}

static void *
init_b(void *arg)
{
    (void)arg;
    n00b_atomic_store(&thread_b, n00b_thread_self());
    n00b_condition_init(&cv_b);
    bump(&b_done);
    return nullptr;
}

// True once the second thread waits on the root list's lock, false once it
// has updated the list while the first is still inside.
static bool
second_registration_waits(n00b_rwlock_t *lock)
{
    int64_t deadline = n00b_ns_timestamp()
                     + (int64_t)HANG_GUARD_MS * N00B_NS_PER_MS;
    while (n00b_ns_timestamp() < deadline) {
        if (n00b_atomic_load(&b_inside_with_a)
            || n00b_atomic_load(&b_done) != 0) {
            return false;
        }
        n00b_thread_t *t = n00b_atomic_load(&thread_b);
        if (lock != nullptr && t != nullptr && t->record != nullptr
            && t->record->lock_wait_target == (void *)lock) {
            return true;
        }
        base_nanosleep_ns(100ULL * 1000);
    }
    assert(false);
}

static void
test_static_condition_inits_on_two_threads(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    n00b_gc_register_root_hook = hold_first_registration;
    auto a = n00b_thread_spawn(init_a, nullptr);
    assert(n00b_result_is_ok(a));
    assert(await_count(&held, 1, HANG_GUARD_MS));

    auto b = n00b_thread_spawn(init_b, nullptr);
    assert(n00b_result_is_ok(b));
    bool waited = second_registration_waits(rt->gc_roots.lock);

    bump(&go);
    n00b_thread_join(n00b_result_get(a));
    n00b_thread_join(n00b_result_get(b));
    n00b_gc_register_root_hook = nullptr;

    n00b_condition_destroy(&cv_a);
    n00b_condition_destroy(&cv_b);

    if (!waited) {
        printf("  [FAIL] a second thread registered a GC root while the first"
               " was inside the root list (list lock %p)\n",
               (void *)rt->gc_roots.lock);
        assert(false);
    }
    printf("  [PASS] GC root registrations on two threads take turns\n");
}

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_gc_roots_lock:\n");
    fflush(stdout);
    test_static_condition_inits_on_two_threads();

    printf("All gc_roots_lock tests passed.\n");
    fflush(stdout);
    n00b_shutdown();
    return 0;
}
